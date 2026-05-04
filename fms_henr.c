#include <stdio.h> // imput e output 
#include <stdlib.h> // Controle de memória
#include <unistd.h> // Comandos do SO 
#include <string.h>
#include <pthread.h> // Permite multiplas tarefas em um processo
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h> // Mede uso de cpu e memória
#include <fcntl.h>
#include <errno.h>

// ALUNOS: 
// Rafael Anselmo
// Davi Vitorino
// Henrique Ferreira

// Variáveis principais
int pid_filho;
int tempo_limite;
int volatile processo_terminou; // 'volatile' impede que o compilador otimize a variável, garantindo leitura atualizada entre threads.
int fd[2]; // Comunicação entre processos via Pipe

// Monitoramento de TEMPO de execução
void* monitor_timeout(void* arg) {
    int segundos_restantes = tempo_limite;
    
    while (segundos_restantes > 0 && !processo_terminou) {
        printf("\r[Timer] Tempo restante para o filho: %ds   ", segundos_restantes);
        fflush(stdout); // Exibe tempo restante no terminal
        sleep(1);
        segundos_restantes--;
    }

    // Verifica se atingiu o tempo limite
    if (!processo_terminou) {
        printf("\n[Monitor] Timeout! Matando o filho %d\n", pid_filho);
        kill(pid_filho, SIGKILL); // Sinal para encerrar o processo filho
    }
    return NULL;
}

int main() {
    char binario[100];
    float cpu_quota, cpu_usada_total = 0;
    float mem_usada = 0;
    long limite_memoria;
    struct rusage uso;

    // Imputs (Limites de uso CPU e Memória)
    printf("Quota total de CPU (segundos): ");
    if (scanf("%f", &cpu_quota) <= 0) return 1;
    printf("Limite de Memoria Acumulada (KB) (ex: 100MB -> 102400): ");
    if (scanf("%ld", &limite_memoria) <= 0) return 1;

    // Executa enquanto o limite de CPU e Memória não forem atingidos
    while (cpu_usada_total < cpu_quota && mem_usada < (float)limite_memoria) {
        printf("\n------------------------------------------------\n");
        printf("Status: CPU [%.2fs / %.2fs] | Mem [%.0fKB / %ldKB]\n", cpu_usada_total, cpu_quota, mem_usada, limite_memoria);
        printf("Digite o caminho do binário (ex: /bin/ls) ou 'sair': ");
        scanf("%s", binario);

        if (strcmp(binario, "sair") == 0) break;

        printf("Defina o timeout de relogio (segundos): ");
        scanf("%d", &tempo_limite);

        //Pipe para comunicação entre processos
        if (pipe(fd) == -1) {
            perror("Erro ao criar pipe");
            continue;
        }

        processo_terminou = 0;
        pid_filho = fork(); // Cria o novo processo

        if (pid_filho < 0) {
            perror("Erro no fork");
            continue;
        }

        if (pid_filho == 0) {
            close(fd[0]); // Fecha leitura do filho

            // Enviando sinal de pronto
            char msg[] = "Processo filho inicializado.";
            write(fd[1], msg, strlen(msg) + 1);
            close(fd[1]); // Fecha a escrita após enviar

            // Lançar execução de qualquer binário
            char *args[] = {binario, NULL};
            execvp(binario, args); // Transforma o provesso filho criado com 'fork' no binário definido pelo usuário

            // Se chegar aqui, houve erro no execvp
            fprintf(stderr, "Erro ao executar o binário '%s': %s\n", binario, strerror(errno));
            exit(EXIT_FAILURE);

        } else {        // Processo pai

            close(fd[1]); // Fecha escrita no pai

            // Recebe mensagem do filho
            char buffer[100];
            if (read(fd[0], buffer, sizeof(buffer)) > 0) {
                printf("\n[Pai] Mensagem do Filho via Pipe: %s\n", buffer);
            };
            close(fd[0]); // Fecha a leitura após receber


            // Thread adicional de monitoramento
            pthread_t tid;
            pthread_create(&tid, NULL, monitor_timeout, NULL); // Inicia o monitoramento de tempo

            wait4(pid_filho, NULL, 0, &uso); // Bloqueia o processo pai até terminar ação do binário (processo filho)
            processo_terminou = 1;           // Sinaliza para a thread de timeout parar
            pthread_join(tid, NULL);         // Sincroniza a thread

            float tempo_cpu_filho = (uso.ru_utime.tv_sec + (uso.ru_utime.tv_usec / 1000000.0)) + 
                                    (uso.ru_stime.tv_sec + (uso.ru_stime.tv_usec / 1000000.0));
            long mem_maxima_filho = uso.ru_maxrss; // Verifica uso de memória do filho atual

            printf("\n>>> Resultados do Processo:\n");
            printf("    Tempo CPU (User+Sys): %.4fs\n", tempo_cpu_filho);
            printf("    Memória Máxima (RSS): %ld KB\n", mem_maxima_filho);
            
            // Atualiza totais acumulados
            cpu_usada_total += tempo_cpu_filho;
            mem_usada += (float)mem_maxima_filho; // Ajuste: Soma cumulativa de memória
            
            // Verificar se extrapolou limites para encerrar o FMS.
            if (cpu_usada_total >= cpu_quota) {
                printf("\n[AVISO] Quota de CPU esgotada!\n");
                break;
            }
            if (mem_usada >= (float)limite_memoria) {
                printf("\n[AVISO] Limite de Memória acumulada atingido!\n");
                break;
            }
        }
    }
     printf("\n=== FMS Encerrado. Status Final: CPU %.2fs | Mem %.0fKB ===\n", cpu_usada_total, mem_usada);
    return 0;
}