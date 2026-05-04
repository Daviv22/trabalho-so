#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>

// Estrutura para passar dados para a thread de monitoramento
typedef struct {
    pid_t child_pid;
    int timeout;
    int finished;
} monitor_data;

// Thread que monitora o tempo de relógio (Wall Time)
void* watch_timeout(void* arg) {
    monitor_data* data = (monitor_data*)arg;
    int elapsed = 0;

    while (elapsed < data->timeout) {
        sleep(1);
        elapsed++;
        if (data->finished) return NULL;
    }

    if (!data->finished) {
        printf("\n[FMS] Tempo limite de relógio atingido! Matando processo %d...\n", data->child_pid);
        kill(data->child_pid, SIGKILL);
    }
    return NULL;
}

int main() {
    char binary[256];
    float cpu_quota, total_cpu_used = 0;
    int wall_timeout;
    long mem_limit; // em KB

    printf("--- FMS: Monitor de Processos ---\n");
    printf("Informe a quota total de CPU (segundos): ");
    scanf("%f", &cpu_quota);
    printf("Informe o limite de memoria (KB): ");
    scanf("%ld", &mem_limit);

    // Loop principal do FMS 
    while (total_cpu_used < cpu_quota) {
        printf("\nQuota restante: %.2fs\n", cpu_quota - total_cpu_used);
        printf("Digite o nome do binario para executar (ou 'sair'): ");
        scanf("%s", binary);

        if (strcmp(binary, "sair") == 0) break;

        printf("Tempo limite de execucao (timeout em seg): ");
        scanf("%d", &wall_timeout);

        pid_t pid = fork();

        if (pid < 0) {
            perror("Erro no fork");
            continue;
        } 

        if (pid == 0) {
            // Processo Filho
            execlp(binary, binary, NULL);
            // Se chegar aqui, o execlp falhou
            perror("Erro ao carregar binario");
            exit(1);
        } else {
            // Processo Pai (FMS)
            pthread_t tid;
            monitor_data m_data = {pid, wall_timeout, 0};
            struct rusage usage;
            int status;

            // Cria thread de monitoramento [cite: 124]
            pthread_create(&tid, NULL, watch_timeout, &m_data);

            // Espera o filho terminar e coleta recursos 
            wait4(pid, &status, 0, &usage);
            m_data.finished = 1; // Avisa a thread que o processo acabou
            pthread_join(tid, NULL);

            // Cálculos de tempo de CPU (Usuario + Sistema)
            float user_cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1000000.0;
            float sys_cpu = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1000000.0;
            float task_cpu = user_cpu + sys_cpu;
            long task_mem = usage.ru_maxrss; // No Linux, maxrss costuma ser em KB

            total_cpu_used += task_cpu;

            printf("\n--- Relatorio da Tarefa ---\n");
            printf("Tempo de CPU: %.4fs (User: %.4fs, Sys: %.4fs)\n", task_cpu, user_cpu, sys_cpu);
            printf("Memoria maxima: %ld KB\n", task_mem);

            // Verifica limites fatais para o FMS 
            if (total_cpu_used > cpu_quota) {
                printf("[FMS] Quota de CPU excedida! Encerrando...\n");
                break;
            }
            if (task_mem > mem_limit) {
                printf("[FMS] Limite de memoria excedido! Encerrando...\n");
                break;
            }
        }
    }

    printf("\nFMS finalizado. Total de CPU consumido: %.4fs\n", total_cpu_used);
    return 0;
}