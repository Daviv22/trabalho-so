/*
 * FMS - Flexible Monitor System v2.0
 * Compilar: gcc -Wall -O2 -D_GNU_SOURCE -o fms fms.c -lpthread -lm
 *
 * Bugs corrigidos v2.0:
 *  - collect_tree: d_type não é confiável em /proc; usa strtol() para validar PIDs
 *  - pid_cpu_s: leitura de /proc/PID/stat robusta (busca último ')' para evitar
 *    quebra com nomes de processo que têm espaços/parênteses)
 *  - pid_rss_kb: usa /proc/PID/statm em vez de parsear VmRSS de /proc/PID/status
 *  - Monitor: primeira amostra em 200ms; nanosleep preciso
 *  - cpu_session_before: monitor recebe snapshot de CPU antes do fork para
 *    calcular corretamente se a quota total foi ultrapassada
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <sys/time.h>

#define VERSION      "2.0"
#define MONITOR_MS   500
#define MAX_PATH     512
#define MAX_ARGS     64
#define MAX_PROCS    512

#define C_RESET   "\033[0m"
#define C_DIM     "\033[2m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_BRED    "\033[1;31m"
#define C_BGREEN  "\033[1;32m"
#define C_BYELLOW "\033[1;33m"
#define C_BCYAN   "\033[1;36m"
#define C_BWHITE  "\033[1;37m"
#define C_YELLOW  "\033[33m"

typedef struct {
    pid_t   child_pid;
    double  timeout_s;
    double  cpu_quota_s;
    double  cpu_session_before;
    long    mem_limit_kb;
    long   *peak_mem_out;
    int    *kill_reason;
    int    *done;
    pthread_mutex_t *mtx;
} MonitorArgs;

typedef struct {
    double cpu_quota_s;
    double cpu_used_s;
    long   mem_limit_kb;
    int    prepaid;
    double credits;
} Session;

/* protótipos */
static void   print_banner(void);
static void   print_separator(const char *label);
static void   print_status(Session *s);
static int    ask_session_config(Session *s);
static int    ask_program(char *binary, char **argv, int *argc);
static void  *monitor_thread(void *arg);
static int    collect_tree(pid_t root, pid_t *out, int max);
static double pid_cpu_s(pid_t pid);
static double get_process_tree_cpu_s(pid_t root);
static long   pid_rss_kb(pid_t pid);
static long   get_process_tree_mem_kb(pid_t root);
static void   kill_tree(pid_t root, int sig);
static double timeval_to_s(struct timeval tv);
static void   bar(double used, double total, int width, const char *color);
static void   sleep_ms(long ms);

/* ═══════════════════════ MAIN ═══════════════════════ */
int main(void)
{
    Session sess = {0};
    int run = 1;

    print_banner();

    if (!ask_session_config(&sess)) {
        fprintf(stderr, C_BRED "Configuração inválida. Saindo.\n" C_RESET);
        return 1;
    }

    while (run) {
        double remaining = sess.cpu_quota_s - sess.cpu_used_s;
        if (remaining <= 0.0) {
            printf(C_BRED "\n[FMS] Quota de CPU esgotada (%.3fs / %.3fs). "
                   "Encerrando FMS.\n" C_RESET,
                   sess.cpu_used_s, sess.cpu_quota_s);
            break;
        }

        print_status(&sess);

        char  binary[MAX_PATH];
        char *argv[MAX_ARGS];
        int   argc = 0;
        memset(argv, 0, sizeof(argv));

        if (!ask_program(binary, argv, &argc)) {
            printf(C_YELLOW "[FMS] Saindo por solicitação do usuário.\n" C_RESET);
            break;
        }

        double timeout_s = 0.0;
        printf(C_CYAN "Timeout de relógio para esta execução (0 = sem limite): " C_RESET);
        fflush(stdout);
        if (scanf("%lf", &timeout_s) != 1) timeout_s = 0.0;
        getchar();

        if (access(binary, X_OK) != 0) {
            printf(C_YELLOW "[FMS] '%s' não encontrado/executável. "
                   "CPU não descontada.\n" C_RESET, binary);
            for (int i = 0; i < argc; i++) free(argv[i]);
            continue;
        }

        print_separator("INICIANDO PROCESSO");
        printf(C_BWHITE "  Binário     : %s\n" C_RESET, binary);
        if (timeout_s > 0)
            printf(C_BWHITE "  Timeout     : %.1fs\n" C_RESET, timeout_s);
        else
            printf(C_BWHITE "  Timeout     : sem limite\n" C_RESET);
        printf(C_BWHITE "  CPU restante: %.3fs\n\n" C_RESET, remaining);

        struct rusage ru_before;
        getrusage(RUSAGE_CHILDREN, &ru_before);

        struct timeval wall_start, wall_end;
        gettimeofday(&wall_start, NULL);

        pid_t child = fork();
        if (child < 0) {
            perror("fork");
            for (int i = 0; i < argc; i++) free(argv[i]);
            continue;
        }

        if (child == 0) {
            if (sess.mem_limit_kb > 0) {
                struct rlimit rl;
                rl.rlim_cur = (rlim_t)sess.mem_limit_kb * 1024;
                rl.rlim_max = rl.rlim_cur;
                setrlimit(RLIMIT_AS, &rl);
            }
            execv(binary, argv);
            perror("execv");
            _exit(127);
        }

        /* pai */
        long   peak_mem_kb = 0;
        int    kill_reason = 0;
        int    done        = 0;
        pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

        MonitorArgs margs = {
            .child_pid          = child,
            .timeout_s          = timeout_s,
            .cpu_quota_s        = sess.cpu_quota_s,
            .cpu_session_before = sess.cpu_used_s,
            .mem_limit_kb       = sess.mem_limit_kb,
            .peak_mem_out       = &peak_mem_kb,
            .kill_reason        = &kill_reason,
            .done               = &done,
            .mtx                = &mtx,
        };

        pthread_t mon_tid;
        pthread_create(&mon_tid, NULL, monitor_thread, &margs);

        int status = 0;
        waitpid(child, &status, 0);

        pthread_mutex_lock(&mtx);
        done = 1;
        pthread_mutex_unlock(&mtx);
        pthread_join(mon_tid, NULL);
        pthread_mutex_destroy(&mtx);

        gettimeofday(&wall_end, NULL);
        double wall_s = (wall_end.tv_sec  - wall_start.tv_sec) +
                        (wall_end.tv_usec - wall_start.tv_usec) / 1e6;

        struct rusage ru_after;
        getrusage(RUSAGE_CHILDREN, &ru_after);
        double cpu_user_s = timeval_to_s(ru_after.ru_utime)
                          - timeval_to_s(ru_before.ru_utime);
        double cpu_sys_s  = timeval_to_s(ru_after.ru_stime)
                          - timeval_to_s(ru_before.ru_stime);
        double cpu_run    = cpu_user_s + cpu_sys_s;
        sess.cpu_used_s  += cpu_run;

        print_separator("RELATÓRIO DE EXECUÇÃO");

        const char *rs = "Normal",    *rc = C_BGREEN;
        if      (kill_reason == 1) { rs = "TIMEOUT";        rc = C_BYELLOW; }
        else if (kill_reason == 2) { rs = "QUOTA CPU";      rc = C_BRED;    }
        else if (kill_reason == 3) { rs = "LIMITE MEMÓRIA"; rc = C_BRED;    }

        printf("  Término      : %s%s%s\n",  rc, rs, C_RESET);
        printf("  Tempo relógio: %.3fs\n",   wall_s);
        printf("  CPU usuário  : %.3fs\n",   cpu_user_s);
        printf("  CPU sistema  : %.3fs\n",   cpu_sys_s);
        printf("  CPU total    : %.3fs\n",   cpu_run);
        printf("  Mem. pico    : %ld KB (%.1f MB)\n",
               peak_mem_kb, peak_mem_kb / 1024.0);
        printf("  CPU sessão   : %.3fs / %.3fs\n\n",
               sess.cpu_used_s, sess.cpu_quota_s);

        printf("  Quota CPU  : ");
        bar(sess.cpu_used_s, sess.cpu_quota_s, 40, C_CYAN);
        printf("\n");
        if (sess.mem_limit_kb > 0) {
            printf("  Memória    : ");
            bar((double)peak_mem_kb, (double)sess.mem_limit_kb, 40, C_MAGENTA);
            printf("\n");
        }
        printf("\n");

        if (kill_reason == 2 || kill_reason == 3) {
            printf(C_BRED "[FMS] Limite crítico atingido. Encerrando FMS.\n" C_RESET);
            run = 0;
        } else if (sess.cpu_used_s >= sess.cpu_quota_s) {
            printf(C_BRED "[FMS] Quota de CPU esgotada. Encerrando FMS.\n" C_RESET);
            run = 0;
        }

        for (int i = 0; i < argc; i++) free(argv[i]);
    }

    print_separator("SESSÃO ENCERRADA");
    printf("  CPU total consumida: %.3fs\n\n", sess.cpu_used_s);
    return 0;
}

/* ═══════════════════════ THREAD MONITOR ═══════════════════════ */
static void *monitor_thread(void *arg)
{
    MonitorArgs *m = (MonitorArgs *)arg;
    struct timeval t_start;
    gettimeofday(&t_start, NULL);

    sleep_ms(200); /* primeira amostra rápida */

    while (1) {
        pthread_mutex_lock(m->mtx);
        int finished = *(m->done);
        pthread_mutex_unlock(m->mtx);
        if (finished) break;

        struct timeval now;
        gettimeofday(&now, NULL);
        double elapsed = (now.tv_sec  - t_start.tv_sec) +
                         (now.tv_usec - t_start.tv_usec) / 1e6;

        double tree_cpu = get_process_tree_cpu_s(m->child_pid);
        long   tree_mem = get_process_tree_mem_kb(m->child_pid);

        pthread_mutex_lock(m->mtx);
        if (tree_mem > *(m->peak_mem_out))
            *(m->peak_mem_out) = tree_mem;
        pthread_mutex_unlock(m->mtx);

        printf(C_DIM "\r  [Monitor] wall=%.1fs | cpu=%.3fs | mem=%ldKB (%.1fMB)    "
               C_RESET, elapsed, tree_cpu, tree_mem, tree_mem / 1024.0);
        fflush(stdout);

        /* timeout */
        if (m->timeout_s > 0 && elapsed >= m->timeout_s) {
            printf(C_BYELLOW "\n  [Monitor] TIMEOUT (%.1fs)! Matando...\n" C_RESET, elapsed);
            kill_tree(m->child_pid, SIGKILL);
            pthread_mutex_lock(m->mtx);
            *(m->kill_reason) = 1;
            pthread_mutex_unlock(m->mtx);
            break;
        }

        /* quota CPU */
        double total_cpu = m->cpu_session_before + tree_cpu;
        if (total_cpu >= m->cpu_quota_s) {
            printf(C_BRED "\n  [Monitor] QUOTA CPU esgotada (%.3fs)! Matando...\n"
                   C_RESET, total_cpu);
            kill_tree(m->child_pid, SIGKILL);
            pthread_mutex_lock(m->mtx);
            *(m->kill_reason) = 2;
            pthread_mutex_unlock(m->mtx);
            break;
        }

        /* limite memória */
        if (m->mem_limit_kb > 0 && tree_mem > m->mem_limit_kb) {
            printf(C_BRED "\n  [Monitor] MEM ultrapassada (%ldKB > %ldKB)! Matando...\n"
                   C_RESET, tree_mem, m->mem_limit_kb);
            kill_tree(m->child_pid, SIGKILL);
            pthread_mutex_lock(m->mtx);
            *(m->kill_reason) = 3;
            pthread_mutex_unlock(m->mtx);
            break;
        }

        sleep_ms(MONITOR_MS);
    }

    printf("\n");
    return NULL;
}

/* ═══════════════════════ /proc HELPERS ═══════════════════════ */

/*
 * collect_tree — BFS via /proc para encontrar toda a descendência.
 * USA strtol() para identificar entradas numéricas (PIDs).
 * NÃO usa d_type pois /proc não preenche esse campo de forma confiável.
 */
static int collect_tree(pid_t root, pid_t *out, int max)
{
    pid_t queue[MAX_PROCS];
    int   head = 0, tail = 0, count = 0;
    queue[tail++] = root;

    while (head < tail && count < max) {
        pid_t cur = queue[head++];
        out[count++] = cur;

        DIR *d = opendir("/proc");
        if (!d) break;

        struct dirent *de;
        while ((de = readdir(d)) != NULL && tail < MAX_PROCS) {
            char *endp;
            long p = strtol(de->d_name, &endp, 10);
            if (*endp != '\0' || p <= 0) continue; /* não é PID */

            char path[80];
            snprintf(path, sizeof(path), "/proc/%ld/status", p);
            FILE *f = fopen(path, "r");
            if (!f) continue;

            char  line[256];
            pid_t ppid = -1;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "PPid:", 5) == 0) {
                    ppid = (pid_t)atol(line + 5);
                    break;
                }
            }
            fclose(f);

            if (ppid == cur)
                queue[tail++] = (pid_t)p;
        }
        closedir(d);
    }
    return count;
}

/*
 * pid_cpu_s — CPU (user+sys) de um PID via /proc/PID/stat.
 *
 * O campo comm (campo 2) pode conter espaços e parênteses, quebrando
 * leitura com fscanf %s. Solução: encontrar o ÚLTIMO ')' na linha
 * e parsear os campos numéricos a partir daí.
 */
static double pid_cpu_s(pid_t pid)
{
    char path[80];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f) return 0.0;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0.0;
    buf[n] = '\0';

    /* Encontrar ÚLTIMO ')' — fim do campo comm */
    char *p = strrchr(buf, ')');
    if (!p) return 0.0;
    p++; /* aponta para o caractere após ')' */

    /*
     * A partir daqui os campos são:
     * state(3) ppid(4) pgrp(5) session(6) tty(7) tpgid(8) flags(9)
     * minflt(10) cminflt(11) majflt(12) cmajflt(13) utime(14) stime(15)
     */
    char state;
    int ppid, pgrp, session, tty, tpgid;
    unsigned flags;
    unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;

    int ret = sscanf(p,
        " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu",
        &state, &ppid, &pgrp, &session, &tty, &tpgid, &flags,
        &minflt, &cminflt, &majflt, &cmajflt,
        &utime, &stime);

    if (ret < 13) return 0.0;

    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) clk = 100;

    return (double)(utime + stime) / (double)clk;
}

/*
 * pid_rss_kb — RSS em KB via /proc/PID/statm.
 * Formato: size(pág) rss(pág) shared text lib data dt
 * Muito mais simples e robusto que parsear /proc/PID/status.
 */
static long pid_rss_kb(pid_t pid)
{
    char path[80];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    long size_pages = 0, rss_pages = 0;
    int ret = fscanf(f, "%ld %ld", &size_pages, &rss_pages);
    fclose(f);

    if (ret < 2) return 0;

    long page_kb = sysconf(_SC_PAGE_SIZE) / 1024;
    if (page_kb <= 0) page_kb = 4;

    return rss_pages * page_kb;
}

static double get_process_tree_cpu_s(pid_t root)
{
    pid_t pids[MAX_PROCS];
    int n = collect_tree(root, pids, MAX_PROCS);
    double total = 0.0;
    for (int i = 0; i < n; i++)
        total += pid_cpu_s(pids[i]);
    return total;
}

static long get_process_tree_mem_kb(pid_t root)
{
    pid_t pids[MAX_PROCS];
    int n = collect_tree(root, pids, MAX_PROCS);
    long total = 0;
    for (int i = 0; i < n; i++)
        total += pid_rss_kb(pids[i]);
    return total;
}

static void kill_tree(pid_t root, int sig)
{
    pid_t pids[MAX_PROCS];
    int n = collect_tree(root, pids, MAX_PROCS);
    for (int i = n - 1; i >= 0; i--)
        kill(pids[i], sig);
}

/* ═══════════════════════ UI HELPERS ═══════════════════════ */

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static double timeval_to_s(struct timeval tv)
{
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void bar(double used, double total, int width, const char *color)
{
    if (total <= 0) { printf("[N/A]"); return; }
    double pct = used / total;
    if (pct > 1.0) pct = 1.0;
    int filled = (int)(pct * width);
    printf("[%s", color);
    for (int i = 0; i < width; i++) putchar(i < filled ? '#' : '-');
    printf(C_RESET "] %.1f%%", pct * 100.0);
}

static void print_banner(void)
{
    printf(C_BCYAN
        "\n"
        "  ╔══════════════════════════════════════════╗\n"
        "  ║   FMS — Flexible Monitor System v%-6s  ║\n"
        "  ║   Controle de processos e recursos       ║\n"
        "  ╚══════════════════════════════════════════╝\n"
        C_RESET "\n", VERSION);
}

static void print_separator(const char *label)
{
    int pad = 38 - (int)strlen(label);
    if (pad < 0) pad = 0;
    printf(C_BLUE "  ── %s ", label);
    for (int i = 0; i < pad; i++) putchar('-');
    printf(C_RESET "\n");
}

static void print_status(Session *s)
{
    printf(C_DIM
        "\n  ┌─ Status da Sessão ─────────────────────┐\n"
        "  │ CPU usada   : %7.3fs / %-7.3fs        │\n"
        "  │ CPU restante: %-7.3fs                  │\n",
        s->cpu_used_s, s->cpu_quota_s,
        s->cpu_quota_s - s->cpu_used_s);
    if (s->mem_limit_kb > 0)
        printf("  │ Mem. limite : %-8ld KB             │\n", s->mem_limit_kb);
    else
        printf("  │ Mem. limite : sem limite               │\n");
    printf("  └────────────────────────────────────────┘\n" C_RESET "\n");
}

static int ask_session_config(Session *s)
{
    print_separator("CONFIGURAÇÃO DA SESSÃO");

    printf(C_CYAN "Quota total de CPU para a sessão (segundos): " C_RESET);
    fflush(stdout);
    if (scanf("%lf", &s->cpu_quota_s) != 1 || s->cpu_quota_s <= 0) {
        printf("\nValor inválido.\n");
        return 0;
    }
    getchar();

    printf(C_CYAN "Limite máximo de memória em KB (0 = sem limite): " C_RESET);
    fflush(stdout);
    if (scanf("%ld", &s->mem_limit_kb) != 1) s->mem_limit_kb = 0;
    getchar();
    if (s->mem_limit_kb < 0) s->mem_limit_kb = 0;

    int mode = 2;
    printf(C_CYAN
        "Modo de operação:\n"
        "  1 - Pré-pago  (créditos pré-definidos)\n"
        "  2 - Pós-pago  (pague pelo uso)\n"
        "Escolha: " C_RESET);
    fflush(stdout);
    if (scanf("%d", &mode) != 1) mode = 2;
    getchar();
    s->prepaid = (mode == 1);
    s->credits = s->cpu_quota_s;

    printf(C_BGREEN
        "\n[FMS] quota=%.3fs | mem=%ldKB | modo=%s\n\n" C_RESET,
        s->cpu_quota_s, s->mem_limit_kb,
        s->prepaid ? "pré-pago" : "pós-pago");
    return 1;
}

static int ask_program(char *binary, char **argv, int *argc)
{
    print_separator("NOVO PROGRAMA");
    printf(C_CYAN "Caminho do binário (ou 'sair' para encerrar): " C_RESET);
    fflush(stdout);

    char line[MAX_PATH + 256];
    if (!fgets(line, sizeof(line), stdin)) return 0;
    line[strcspn(line, "\n")] = '\0';

    if (strlen(line) == 0 ||
        strcasecmp(line, "sair") == 0 ||
        strcasecmp(line, "exit") == 0)
        return 0;

    char *token = strtok(line, " \t");
    if (!token) return 0;
    strncpy(binary, token, MAX_PATH - 1);
    binary[MAX_PATH - 1] = '\0';

    argv[0] = strdup(binary);
    *argc = 1;
    while ((token = strtok(NULL, " \t")) != NULL && *argc < MAX_ARGS - 1)
        argv[(*argc)++] = strdup(token);
    argv[*argc] = NULL;
    return 1;
}