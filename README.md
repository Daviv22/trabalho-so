# Trabalho de Sistemas Operacionais

<img width="569" height="320" alt="FURG-logo" src="https://github.com/user-attachments/assets/9231df80-1ca1-4c7f-9fa2-5e6b34b6d98f" />

## Tarefa:

Implemente um programa (FMS) capaz de lançar e controlar a execução de um outro programa conforme os requisitos descritos

## Requisítos mínimos:

1. Criar e controlar a execução de um novo processo
2. Criar uma thread adicional para monitorar o novo processo
3. Implementar comunicação entre threads e entre processos
    * Thread principal com thread de monitoramento do timeout
    * Processo original com o novo processo
4. Lançar a execução de qualquer programa executável cujo binário seja fornecido pelo usuário
    * Deve existir uma interface (textual ou gráfica) para que o nome do binário seja informado pelo(a) usuário(a) sem que seja necessário editar o código fonte
5. Consultar a(o) usuária(o) sobre
    * A quota de tempo de CPU para execução do binário fornecido
    * Eventual tempo limite (timeout) para execução do binário fornecido
    * Um eventual montante máximo de memória para execução do binário fornecido
6. Monitorar a execução do processo criado para executar o binário fornecido
    * Controlar o tempo de relógio e matar o processo caso o timeout expire
    * Identificar quando o programa terminou
    * Quantificar o tempo de CPU (usuário e sistema) utilizado
    * Quantificar o máximo de memória utilizado
7. O programa FMS deve funcionar em laço, solicitando um novo binário sempre que ainda houver quota de tempo de CPU disponível
    * Caso a quota de CPU ou o consumo máximo de memória seja ultrapassada(o), o programa deve reportar tal situação
    * O FMS deve encerrar caso algum dos limites (CPU ou memória) seja extrapolado
    * Porém a expiração do timeout não encerra o FMS, apenas o programa monitorado

## Requisítos recomendados
1. Monitorar constantemente (e.g., 1x por segundo) o consumo de quota de CPU e o uso máximo de memória
    * Matando imediatamente o programa caso ao menos um dos limites seja extrapolado
2. Reportar dinamicamente ao usuário o progresso do consumo de quota de CPU e de memória
3. Não descontar da quota, execuções que falhem em lançar o binário fornecido
4. Monitorar a árvore de processos criados
    *  Útil para programas que criam muitos processos
5. Implementar "operação pré-paga" com base em créditos
6. Implementar "operação pós-paga" "pague pelo uso


### Atenção
- O tempo de CPU é diferente do tempo de relógio (walltime)
- O tempo de relógio é o tempo real transcorrido entre o início e o fim da execução
- O tempo de CPU é o tempo em que a CPU ficou ocupada de fato executando o código do processo diretamente ou por intermédio do sistema
- A função timeout não necessariamente evita que a quota de CPU seja superada
