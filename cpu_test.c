/*
 * cpu_test.c — CPU-intensive workload for Mini Container Runtime
 *
 * Runs a tight arithmetic loop for approximately 60 seconds (or the
 * duration passed as argv[1]), keeping one CPU core fully utilized.
 * Useful for testing that the container isolates CPU-bound processes
 * and that the engine correctly tracks and can stop running containers.
 *
 * Compile: gcc -O2 -o cpu_test cpu_test.c
 * Usage:   ./cpu_test [duration_seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int duration = 60;   /* default: 60 seconds */
    if (argc >= 2)
        duration = atoi(argv[1]);

    printf("[cpu_test] PID=%d, running for %d seconds...\n",
           getpid(), duration);
    fflush(stdout);

    time_t start = time(NULL);
    volatile unsigned long counter = 0;

    while (1) {
        time_t now = time(NULL);
        if ((int)(now - start) >= duration)
            break;

        /* Tight arithmetic loop — keeps the core busy */
        for (unsigned long i = 0; i < 10000000UL; i++)
            counter += i * 3 + 7;

        /* Print progress every ~5 seconds */
        if ((int)(now - start) % 5 == 0) {
            printf("[cpu_test] elapsed=%lds counter=%lu\n",
                   (long)(now - start), counter);
            fflush(stdout);
        }
    }

    printf("[cpu_test] done after %d seconds, counter=%lu\n",
           duration, counter);
    return 0;
}
