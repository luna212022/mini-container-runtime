/*
 * memory_test.c — Memory-hungry workload for Mini Container Runtime
 *
 * Allocates memory in 10 MB chunks, writes to every page with memset
 * so the allocations become resident (RSS increases), and then waits.
 * This is designed to trigger the kernel module's soft/hard memory limits.
 *
 * Compile: gcc -O0 -o memory_test memory_test.c
 * Usage:   ./memory_test [chunk_mb] [max_chunks]
 *
 * Defaults: 10 MB chunks, up to 100 chunks (1 GB total).
 *
 * Example — trigger a 256 MB soft limit and 512 MB hard limit:
 *   ./memory_test 10 60   # allocates up to 600 MB
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MB (1024UL * 1024UL)

int main(int argc, char *argv[])
{
    unsigned long chunk_mb   = 10;
    unsigned long max_chunks = 100;

    if (argc >= 2) chunk_mb   = strtoul(argv[1], NULL, 10);
    if (argc >= 3) max_chunks = strtoul(argv[2], NULL, 10);

    printf("[memory_test] PID=%d, allocating %lu x %lu MB chunks\n",
           getpid(), max_chunks, chunk_mb);
    fflush(stdout);

    unsigned long total_bytes = 0;

    for (unsigned long i = 0; i < max_chunks; i++) {
        unsigned long sz = chunk_mb * MB;
        void *p = malloc(sz);
        if (!p) {
            fprintf(stderr, "[memory_test] malloc failed at chunk %lu\n", i);
            break;
        }

        /* Touch every page so the OS actually maps the memory (increases RSS) */
        memset(p, (int)(i & 0xFF), sz);
        total_bytes += sz;

        printf("[memory_test] allocated chunk %lu, total=%lu MB (RSS should be ~%lu MB)\n",
               i + 1, total_bytes / MB, total_bytes / MB);
        fflush(stdout);

        sleep(1);   /* pause between allocations so the monitor has time to check */
    }

    printf("[memory_test] done allocating %lu MB; sleeping for 30s...\n",
           total_bytes / MB);
    fflush(stdout);

    sleep(30);    /* stay resident so the kernel module has time to act */

    printf("[memory_test] exiting\n");
    return 0;
}
