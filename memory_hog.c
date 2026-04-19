/*
 * memory_hog.c — Memory-consuming test workload for scheduler experiments.
 *
 * Usage:
 *   /memory_hog [mb] [seconds]
 *
 * Allocates <mb> MiB of memory (touching every page so it becomes resident),
 * then sleeps for <seconds> reporting current RSS.  This makes it easy to
 * trigger the kernel module's soft/hard memory limits.
 *
 * Default: 128 MiB, 60 seconds.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MB (1024UL * 1024UL)

int main(int argc, char *argv[])
{
    unsigned long mb      = (argc > 1) ? strtoul(argv[1], NULL, 10) : 128;
    unsigned long seconds = (argc > 2) ? strtoul(argv[2], NULL, 10) : 60;

    printf("memory_hog: allocating %lu MiB for %lu seconds\n", mb, seconds);
    fflush(stdout);

    char *buf = malloc(mb * MB);
    if (!buf) {
        perror("malloc");
        return 1;
    }

    /* Touch every page to force RSS to grow */
    memset(buf, 0xAB, mb * MB);

    printf("memory_hog: %lu MiB resident, sleeping %lu seconds\n", mb, seconds);
    fflush(stdout);

    time_t start = time(NULL);
    while ((unsigned long)(time(NULL) - start) < seconds) {
        sleep(1);
        printf("memory_hog: elapsed=%lds rss~=%lu MiB\n",
               (long)(time(NULL) - start), mb);
        fflush(stdout);
    }

    free(buf);
    printf("memory_hog: done\n");
    return 0;
}
