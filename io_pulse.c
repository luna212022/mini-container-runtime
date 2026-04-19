/*
 * io_pulse.c — I/O-bound test workload for scheduler experiments.
 *
 * Usage:
 *   /io_pulse [seconds] [block_size_bytes]
 *
 * Performs repeated write → fsync → read cycles, spending most of its
 * time in the kernel waiting for disk I/O to complete.  This is useful
 * for comparing CPU-bound vs I/O-bound container behaviour under CFS.
 *
 * Default: 60 seconds, 4096 bytes per block.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define TMP_FILE "/tmp/io_pulse_scratch.dat"

int main(int argc, char *argv[])
{
    int    duration   = (argc > 1) ? atoi(argv[1]) : 60;
    size_t block_size = (argc > 2) ? (size_t)atol(argv[2]) : 4096;

    printf("io_pulse: PID=%d  duration=%ds  block=%zu bytes\n",
           getpid(), duration, block_size);
    fflush(stdout);

    char *wbuf = malloc(block_size);
    char *rbuf = malloc(block_size);
    if (!wbuf || !rbuf) { perror("malloc"); return 1; }
    memset(wbuf, 'A', block_size);

    int fd = open(TMP_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { perror("open"); free(wbuf); free(rbuf); return 1; }

    time_t    start      = time(NULL);
    long long io_ops     = 0;
    long long bytes_sent = 0;

    while (1) {
        time_t now = time(NULL);
        if ((int)(now - start) >= duration)
            break;

        ssize_t wr = write(fd, wbuf, block_size);
        if (wr < 0) { if (errno == EINTR) continue; perror("write"); break; }
        bytes_sent += wr;

        if (fsync(fd) < 0) { perror("fsync"); break; }

        if (lseek(fd, -(off_t)block_size, SEEK_CUR) < 0) { perror("lseek"); break; }
        ssize_t rd = read(fd, rbuf, block_size);
        if (rd < 0) { if (errno == EINTR) continue; perror("read"); break; }

        if (lseek(fd, 0, SEEK_END) < 0) { perror("lseek end"); break; }

        io_ops++;

        if ((int)(now - start) % 5 == 0 && io_ops % 50 == 0) {
            printf("io_pulse: elapsed=%lds  io_ops=%lld  bytes=%lld\n",
                   (long)(now - start), io_ops, bytes_sent);
            fflush(stdout);
        }
    }

    close(fd);
    unlink(TMP_FILE);
    free(wbuf);
    free(rbuf);

    printf("io_pulse: done  io_ops=%lld  bytes=%lld\n", io_ops, bytes_sent);
    return 0;
}
