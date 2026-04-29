#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>

// Rafael Anselmo
// Henrique Ferreira
// Davi Vitorino

// ---------------------------------------------------------------------------
// Estrutura compartilhada entre a thread principal e a thread de monitor.
// Todos os campos acessados por ambas as threads ficam aqui, protegidos
// por um mutex.
// ---------------------------------------------------------------------------
typedef struct {
    pid_t           pid_filho;
    int             tempo_limite;
    volatile int    processo_terminou;  // flag de encerramento
    pthread_mutex_t mutex;
} MonitorArgs;

// ---------------------------------------------------------------------------
// Thread de timeout: aguarda 'tempo_limite' segundos e mata o filho se ele
// ainda estiver rodando.
// ---------------------------------------------------------------------------
void* monitor_timeout(void* arg) {
    MonitorArgs* ma = (MonitorArgs*)arg;

    for (int s = 0; s < ma->tempo_limite; s++) {
        sleep(1);

        pthread_mutex_lock(&ma->mutex);
        int terminou = ma->processo_terminou;
        pthread_mutex_unlock(&ma->mutex);

        if (terminou) return NULL;
    }

    pthread_mutex_lock(&ma->mutex);
    if (!ma->processo_terminou) {
        printf("\n[Monitor] Timeout! Matando o filho %d\n", ma->pid_filho);
        kill(ma->pid_filho, SIGKILL);
    }
    pthread_mutex_unlock(&ma->mutex);

    return NULL;
}

// ---------------------------------------------------------------------------
// Lê uma linha inteira e extrai o primeiro token (sem risco de overflow).
// Retorna 0 se não leu nada útil.
// ---------------------------------------------------------------------------
static int ler_token(const char* prompt, char* dest, size_t dest_size) {
    printf("%s", prompt);
    fflush(stdout);

    char linha[512];
    if (!fgets(linha, sizeof(linha), stdin)) return 0;

    // Remove o '\n' residual
    linha[strcspn(linha, "\n")] = '\0';

    if (strlen(linha) == 0) return 0;

    strncpy(dest, linha, dest_size - 1);
    dest[dest_size - 1] = '\0';
    return 1;
}

// ---------------------------------------------------------------------------
// Lê um float do stdin com prompt.
// ---------------------------------------------------------------------------
static int ler_float(const char* prompt, float* dest) {
    char buf[64];
    if (!ler_token(prompt, buf, sizeof(buf))) return 0;
    char* end;
    *dest = strtof(buf, &end);
    return (end != buf);
}

// ---------------------------------------------------------------------------
// Lê um long do stdin com prompt.
// ---------------------------------------------------------------------------
static int ler_long(const char* prompt, long* dest) {
    char buf[64];
    if (!ler_token(prompt, buf, sizeof(buf))) return 0;
    char* end;
    *dest = strtol(buf, &end, 10);
    return (end != buf);
}

// ---------------------------------------------------------------------------
// Lê um int do stdin com prompt.
// ---------------------------------------------------------------------------
static int ler_int(const char* prompt, int* dest) {
    long tmp;
    if (!ler_long(prompt, &tmp)) return 0;
    *dest = (int)tmp;
    return 1;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    // Desativa buffer do stdout para que prompts apareçam imediatamente,
    // mesmo quando a saída do filho se intercala com a do pai.
    setvbuf(stdout, NULL, _IONBF, 0);

    float cpu_quota, cpu_usada = 0.0f;
    long  limite_memoria;

    if (!ler_float("Quota total de CPU (segundos): ", &cpu_quota) || cpu_quota <= 0) {
        fprintf(stderr, "Quota de CPU invalida.\n");
        return 1;
    }
    if (!ler_long("Limite de Memoria (KB): ", &limite_memoria) || limite_memoria <= 0) {
        fprintf(stderr, "Limite de memoria invalido.\n");
        return 1;
    }

    // Uso acumulado ANTES do processo atual, para isolar o gasto de cada filho.
    float cpu_acumulada_anterior = 0.0f;
    long  mem_acumulada_anterior = 0L;

    while (cpu_usada < cpu_quota) {
        printf("\n--- CPU Usada: %.2f / %.2f s | Mem Usada (ultimo): %.2ld / %ld KB ---\n",
               cpu_usada, cpu_quota, mem_acumulada_anterior, limite_memoria);

        // Lê o caminho do binário (até 255 caracteres, sem overflow)
        char binario[256];
        if (!ler_token("Digite o binario (ou 'sair'): ", binario, sizeof(binario))) continue;
        if (strcmp(binario, "sair") == 0) break;

        int tempo_limite;
        if (!ler_int("Timeout de relogio (segundos): ", &tempo_limite) || tempo_limite <= 0) {
            printf("Timeout invalido, tente novamente.\n");
            continue;
        }

        // Inicializa a estrutura compartilhada com a thread de monitor
        MonitorArgs ma;
        ma.tempo_limite       = tempo_limite;
        ma.processo_terminou  = 0;
        pthread_mutex_init(&ma.mutex, NULL);

        // Fork — com tratamento de erro
        ma.pid_filho = fork();

        if (ma.pid_filho < 0) {
            perror("fork falhou");
            pthread_mutex_destroy(&ma.mutex);
            continue;
        }

        if (ma.pid_filho == 0) {
            // --- Processo filho ---
            // Permite que o usuário passe argumentos separados por espaço.
            // Por simplicidade, executamos só o binário sem argumentos extras;
            // para suportar argumentos, bastaria tokenizar 'binario' com strtok.
            char* args[] = { binario, NULL };
            execvp(binario, args);
            perror("Erro ao executar");
            _exit(127);  // _exit evita flushes indevidos de buffers do pai
        }

        // --- Processo pai ---
        pthread_t tid;
        pthread_create(&tid, NULL, monitor_timeout, &ma);

        // Aguarda o filho terminar
        int status;
        waitpid(ma.pid_filho, &status, 0);

        // Sinaliza à thread de monitor que o filho já encerrou
        pthread_mutex_lock(&ma.mutex);
        ma.processo_terminou = 1;
        pthread_mutex_unlock(&ma.mutex);

        // Espera a thread de monitor terminar de verdade antes de continuar
        pthread_join(tid, NULL);
        pthread_mutex_destroy(&ma.mutex);

        // Informa como o filho terminou
        if (WIFEXITED(status)) {
            printf(">>> Filho encerrou normalmente com codigo %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf(">>> Filho morto por sinal %d%s\n",
                   WTERMSIG(status),
                   WTERMSIG(status) == SIGKILL ? " (Timeout ou limite de memoria)" : "");
        }

        // getrusage(RUSAGE_CHILDREN) retorna o uso ACUMULADO de todos os filhos
        // já encerrados. Para obter o gasto somente deste filho, subtraímos o
        // acumulado anterior.
        struct rusage uso;
        getrusage(RUSAGE_CHILDREN, &uso);

        float cpu_total_agora = (float)(uso.ru_utime.tv_sec  + uso.ru_stime.tv_sec)
                              + (float)(uso.ru_utime.tv_usec + uso.ru_stime.tv_usec) / 1e6f;
        long  mem_total_agora = uso.ru_maxrss;

        float cpu_este_filho = cpu_total_agora - cpu_acumulada_anterior;
        long  mem_este_filho = mem_total_agora - mem_acumulada_anterior;

        // Garante valores não negativos (caso o SO não suporte ru_maxrss, etc.)
        if (cpu_este_filho < 0) cpu_este_filho = 0;
        if (mem_este_filho < 0) mem_este_filho = 0;

        printf(">>> Filho gastou: CPU=%.3fs | Mem=%ldKB\n", cpu_este_filho, mem_este_filho);

        // Atualiza os acumuladores
        cpu_acumulada_anterior = cpu_total_agora;
        mem_acumulada_anterior = mem_total_agora;
        cpu_usada += cpu_este_filho;

        // Verifica limites DEPOIS de contabilizar o uso
        if (cpu_usada >= cpu_quota) {
            printf("[FMS] Quota de CPU esgotada (%.2f / %.2f s). Encerrando.\n",
                   cpu_usada, cpu_quota);
            break;
        }
        if (mem_este_filho > limite_memoria) {
            printf("[FMS] Limite de memoria superado (%ldKB > %ldKB). Encerrando.\n",
                   mem_este_filho, limite_memoria);
            break;
        }
    }

    printf("[FMS] Sessao encerrada. CPU total usada: %.3f / %.2f s\n", cpu_usada, cpu_quota);
    return 0;
}