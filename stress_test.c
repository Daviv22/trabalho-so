/*
 * stress_test.c — programa auxiliar para testar o FMS.
 * Aceita argumentos:
 *   ./stress_test <cpu_seconds> <mem_mb>
 * Consome CPU por <cpu_seconds> segundos e aloca <mem_mb> MB.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

int main(int argc, char *argv[])
{
    double cpu_s = 3.0;
    int    mem_mb = 10;

    if (argc > 1) cpu_s  = atof(argv[1]);
    if (argc > 2) mem_mb = atoi(argv[2]);

    printf("[stress] Alocando %d MB...\n", mem_mb);
    size_t sz = (size_t)mem_mb * 1024 * 1024;
    char *buf = malloc(sz);
    if (buf) memset(buf, 0xAB, sz);

    printf("[stress] Queimando %.1f s de CPU...\n", cpu_s);
    struct timespec t0, t1;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t0);
    double elapsed = 0.0;
    volatile double x = 1.23456789;
    while (elapsed < cpu_s) {
        for (int i = 0; i < 1000000; i++) x = sqrt(x + 1.1);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t1);
        elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    }

    printf("[stress] Finalizado. x=%.4f\n", x);
    free(buf);
    return 0;
}