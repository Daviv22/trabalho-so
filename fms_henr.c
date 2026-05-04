#include <stdio.h> // imput e output 
#include <stdlib.h> // Controle de memória
#include <unistd.h> // Comandos do SO 
#include <string.h>
#include <pthread.h> // Permite multiplas tarefas em um processo
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h> // Mede uso de cpu e memória
#include <fcntl.h>

// ALUNOS: 
// Rafael Anselmo
// Davi Vitorino
// Henrique Ferreira

// Variáveis principais
int pid_filho;
int tempo_limite;
int processo_terminou;
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
        killpg(pid_filho, SIGKILL); // Sinal para encerrar o processo filho
    }
    return NULL;
}

int main() {
    char binario[100];
    float cpu_quota, cpu_usada = 0, mem_usada = 0;
    long limite_memoria;
    struct rusage uso;

    // Imputs (Limites de uso CPU e Memória)
    printf("Quota total de CPU (segundos): ");
    scanf("%f", &cpu_quota);
    printf("Limite de Memoria (KB (ex: 100MB -> 102400)): ");
    scanf("%ld", &limite_memoria);

    // Executa enquanto o limite de CPU e Memória não forem atingidos
    while (cpu_usada < cpu_quota && mem_usada < limite_memoria) {
        printf("\nCPU Usada: %.4f/%.f. Mem Usada: %.f/%.ld Digite o binario (ou 'sair'): ", cpu_usada, cpu_quota, mem_usada, limite_memoria);
        scanf("%s", binario);
        if (strcmp(binario, "sair") == 0) break;

        printf("Timeout de relogio (segundos): ");
        scanf("%d", &tempo_limite);

        //Pipe para comunicação entre processos
        if (pipe(fd) == -1) {
            perror("Erro ao criar pipe");
            exit(1);
        }

        processo_terminou = 0;
        pid_filho = fork(); // Cria o novo processo

        if (pid_filho == 0) {
            setpgid(0, 0);
            close(fd[0]); // Pipe
            char msg[] = "Processo filho inicializado.";
            write(fd[1], msg, strlen(msg) + 1);
            close(fd[1]); // Fecha a escrita após enviar

            char *args[] = {binario, NULL};
            execvp(binario, args); // Transforma o provesso filho criado com 'fork' no binário definido pelo usuário
            perror("Erro ao abrir");
            exit(1);

        } else {
            close(fd[1]); // Pipe
            char buffer[100];
            read(fd[0], buffer, sizeof(buffer));
            printf("\n[Pai] Mensagem do Filho via Pipe: %s\n", buffer);
            close(fd[0]); // Fecha a leitura após receber

            pthread_t tid;
            pthread_create(&tid, NULL, monitor_timeout, NULL); // Inicia o monitoramento de tempo

            wait4(pid_filho, NULL, 0, &uso); // Bloqueia o processo pai até terminar ação do binário (processo filho)
            processo_terminou = 1; 
            pthread_join(tid, NULL); 

            float cpu_uso = (uso.ru_utime.tv_sec + (uso.ru_utime.tv_usec / 1000000.0)) + (uso.ru_stime.tv_sec + (uso.ru_stime.tv_usec / 1000000.0));
            long mem_uso_filho = uso.ru_maxrss; // Verifica uso de memória do filho atual

            printf(">>> Filho gastou: CPU=%.4fs | Mem=%ldKB\n", cpu_uso, mem_uso_filho);
            
            cpu_usada += cpu_uso;
            mem_usada += (float)mem_uso_filho; // Ajuste: Soma cumulativa de memória
            
            // Verifica atingimento de limites globais (CPU ou Memória acumulada)
            if (cpu_usada > cpu_quota || mem_usada > (float)limite_memoria) {
                printf("\n[AVISO] Limite acumulado de CPU ou memoria superado!\n");
                printf("Status Final -> CPU: %.4f/%.f | Mem: %.f/%ld\n", cpu_usada, cpu_quota, mem_usada, limite_memoria);
                printf("Encerrando FMS.\n");
                break;
            }
        }
    }
    return 0;
}