/*
 * workload_io.c — I/O-bound workload for Mini Container Runtime
 *
 * Performs repeated write → fsync → read cycles against a temporary file,
 * spending most of its time blocked in the kernel on disk I/O.  This is
 * classic I/O-bound behaviour and produces a useful counterpoint to the
 * CPU-bound workload in cpu_test.c when running scheduler experiments.
 *
 * The process intentionally yields the CPU on every I/O call, so the Linux
 * CFS scheduler sees it as well-behaved and gives it high I/O priority while
 * leaving CPU slices for CPU-bound peers.
 *
 * Compile: gcc -O2 -o workload_io workload_io.c
 * Usage:   ./workload_io [duration_seconds] [block_size_bytes]
 *
 * Default: 60 seconds, 4096-byte blocks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define DEFAULT_DURATION   60
#define DEFAULT_BLOCK_SIZE 4096
#define TMP_FILE           "/tmp/workload_io_scratch.dat"

int main(int argc, char *argv[])
{
    int    duration   = (argc >= 2) ? atoi(argv[1]) : DEFAULT_DURATION;
    size_t block_size = (argc >= 3) ? (size_t)atol(argv[2]) : DEFAULT_BLOCK_SIZE;

    printf("[workload_io] PID=%d  duration=%ds  block=%zu bytes\n",
           getpid(), duration, block_size);
    fflush(stdout);

    /* Allocate one write buffer (all 'A's) and one read buffer */
    char *wbuf = malloc(block_size);
    char *rbuf = malloc(block_size);
    if (!wbuf || !rbuf) {
        perror("malloc");
        return 1;
    }
    memset(wbuf, 'A', block_size);

    /* Open an O_SYNC file so every write forces a kernel flush */
    int fd = open(TMP_FILE, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        free(wbuf); free(rbuf);
        return 1;
    }

    time_t    start      = time(NULL);
    long long io_ops     = 0;
    long long bytes_sent = 0;

    while (1) {
        time_t now = time(NULL);
        if ((int)(now - start) >= duration)
            break;

        /* ── Write phase ── */
        ssize_t wr = write(fd, wbuf, block_size);
        if (wr < 0) {
            if (errno == EINTR) continue;
            perror("write");
            break;
        }
        bytes_sent += wr;

        /* ── Sync phase: blocks until kernel flushes to storage ── */
        if (fsync(fd) < 0) {
            perror("fsync");
            break;
        }

        /* ── Seek back and read ── */
        if (lseek(fd, -(off_t)block_size, SEEK_CUR) < 0) {
            perror("lseek");
            break;
        }
        ssize_t rd = read(fd, rbuf, block_size);
        if (rd < 0) {
            if (errno == EINTR) continue;
            perror("read");
            break;
        }

        /* Move file pointer forward so next write appends */
        if (lseek(fd, 0, SEEK_END) < 0) {
            perror("lseek end");
            break;
        }

        io_ops++;

        /* Print progress every ~5 seconds */
        if ((int)(now - start) % 5 == 0 && io_ops % 50 == 0) {
            printf("[workload_io] elapsed=%lds  io_ops=%lld  bytes=%lld\n",
                   (long)(now - start), io_ops, bytes_sent);
            fflush(stdout);
        }
    }

    close(fd);
    unlink(TMP_FILE);
    free(wbuf);
    free(rbuf);

    printf("[workload_io] done  io_ops=%lld  bytes=%lld\n", io_ops, bytes_sent);
    fflush(stdout);
    return 0;
}
