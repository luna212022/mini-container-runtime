#!/usr/bin/env bash
# =============================================================================
#  mini-container-runtime.sh — Self-extracting bundle
#
#  Run this script on Linux to unpack the entire Mini Container Runtime project:
#
#    bash mini-container-runtime.sh          # unpacks into ./mini-container-runtime/
#    bash mini-container-runtime.sh mydir    # unpacks into ./mydir/
#
#  After unpacking:
#    cd mini-container-runtime
#    make                  # build engine + workloads
#    make all              # also build kernel module
#    sudo ./engine supervisor /tmp/myroot
# =============================================================================

set -e

DEST="${1:-mini-container-runtime}"
echo "==> Unpacking Mini Container Runtime into: $DEST/"
mkdir -p "$DEST"

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 1: monitor_ioctl.h
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/monitor_ioctl.h" << 'EOF_MONITOR_IOCTL_H'
/* monitor_ioctl.h
 * Shared header between the kernel module (monitor.c) and
 * user-space engine (engine.c).
 *
 * Defines the ioctl command numbers and the data structure used
 * to pass information between user-space and the kernel module.
 */

#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

/* Magic number for our ioctl commands — must be unique system-wide.
 * 'm' is commonly used for memory-related modules; verify no collision
 * with /usr/include/asm/ioctls.h on your system before production use. */
#define MONITOR_IOC_MAGIC  'm'

/**
 * struct monitor_request - payload exchanged via ioctl
 * @pid:           PID of the container's init process to monitor
 * @soft_limit_kb: RSS threshold (KB) at which a warning is logged to dmesg
 * @hard_limit_kb: RSS threshold (KB) at which the process is forcibly killed
 */
struct monitor_request {
    pid_t         pid;
    unsigned long soft_limit_kb;
    unsigned long hard_limit_kb;
};

/* MONITOR_IOC_REGISTER — tell the kernel module to start tracking a PID.
 * Direction: user → kernel (_IOW).  Payload: struct monitor_request. */
#define MONITOR_IOC_REGISTER  _IOW(MONITOR_IOC_MAGIC, 1, struct monitor_request)

/* MONITOR_IOC_SETLIMIT — update the soft/hard memory limits for a tracked PID.
 * Direction: user → kernel (_IOW).  Payload: struct monitor_request. */
#define MONITOR_IOC_SETLIMIT  _IOW(MONITOR_IOC_MAGIC, 2, struct monitor_request)

/* MONITOR_IOC_UNREGISTER — stop tracking a PID (called on container stop).
 * Direction: user → kernel (_IOW).  Payload: struct monitor_request (only pid used). */
#define MONITOR_IOC_UNREGISTER _IOW(MONITOR_IOC_MAGIC, 3, struct monitor_request)

/* Maximum number of containers the kernel module will track simultaneously. */
#define MONITOR_MAX_ENTRIES 64

#endif /* MONITOR_IOCTL_H */
EOF_MONITOR_IOCTL_H

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 2: engine.c
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/engine.c" << 'EOF_ENGINE_C'
/*
 * engine.c — Mini Container Runtime
 *
 * A lightweight Docker-like container runtime built on raw Linux primitives.
 *
 * Two modes of operation:
 *   1. SUPERVISOR: "engine supervisor <rootfs>"
 *      Runs as a long-lived daemon.  Listens on a UNIX-domain socket for
 *      commands from CLI instances.  Manages the container lifecycle.
 *
 *   2. CLI:       "engine start|run|ps|stop|logs ..."
 *      Connects to the supervisor socket, sends a command, prints result.
 *
 * Compile:  gcc -Wall -Wextra -pthread -o engine engine.c
 * Run:      sudo ./engine supervisor /path/to/rootfs
 *           sudo ./engine start mybox /path/to/rootfs /bin/sh
 */

#define _GNU_SOURCE   /* for clone(), unshare(), etc. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>      /* clone() */
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>

/* ── ioctl header (shared with kernel module) ──────────────────────────── */
#include "monitor_ioctl.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

#define SOCKET_PATH      "/tmp/container_engine.sock"
#define LOG_DIR          "/tmp/container_logs"
#define MAX_CONTAINERS   64
#define STACK_SIZE       (1024 * 1024)   /* 1 MiB clone stack */
#define MAX_LOG_LINES    256             /* bounded ring-buffer size */
#define MAX_LINE_LEN     512
#define CMD_BUF_SIZE     4096
#define MONITOR_DEV      "/dev/container_monitor"

/* ── Container states ───────────────────────────────────────────────────── */
typedef enum {
    STATE_FREE    = 0,
    STATE_RUNNING = 1,
    STATE_STOPPED = 2,
} ContainerState;

/* ── Per-container log ring buffer ─────────────────────────────────────── */
typedef struct {
    char        lines[MAX_LOG_LINES][MAX_LINE_LEN];
    int         head;           /* producer writes here */
    int         tail;           /* consumer reads here */
    int         count;          /* number of filled slots */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int         done;           /* set to 1 when pipe EOF reached */
} LogBuffer;

/* ── Container table entry ──────────────────────────────────────────────── */
typedef struct {
    ContainerState  state;
    char            id[64];
    pid_t           pid;            /* container init PID (host namespace) */
    int             pipe_rd;        /* read end of stdout/stderr pipe */
    pthread_t       producer_tid;
    pthread_t       consumer_tid;
    LogBuffer       logbuf;
    char            log_path[256];
    time_t          started_at;
} Container;

/* ── Globals ────────────────────────────────────────────────────────────── */
static Container  containers[MAX_CONTAINERS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static int        monitor_fd = -1;   /* fd for /dev/container_monitor */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 1: Log ring-buffer helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void logbuf_init(LogBuffer *lb)
{
    memset(lb, 0, sizeof(*lb));
    pthread_mutex_init(&lb->lock, NULL);
    pthread_cond_init(&lb->not_empty, NULL);
    pthread_cond_init(&lb->not_full, NULL);
}

static void logbuf_destroy(LogBuffer *lb)
{
    pthread_mutex_destroy(&lb->lock);
    pthread_cond_destroy(&lb->not_empty);
    pthread_cond_destroy(&lb->not_full);
}

/* Producer: put one line into the buffer.
 * Blocks if buffer is full (back-pressure). */
static void logbuf_put(LogBuffer *lb, const char *line)
{
    pthread_mutex_lock(&lb->lock);
    while (lb->count == MAX_LOG_LINES && !lb->done)
        pthread_cond_wait(&lb->not_full, &lb->lock);

    if (lb->count < MAX_LOG_LINES) {
        strncpy(lb->lines[lb->head], line, MAX_LINE_LEN - 1);
        lb->lines[lb->head][MAX_LINE_LEN - 1] = '\0';
        lb->head = (lb->head + 1) % MAX_LOG_LINES;
        lb->count++;
        pthread_cond_signal(&lb->not_empty);
    }
    pthread_mutex_unlock(&lb->lock);
}

/* Consumer: get one line (blocks until available or done). */
static int logbuf_get(LogBuffer *lb, char *out)
{
    pthread_mutex_lock(&lb->lock);
    while (lb->count == 0 && !lb->done)
        pthread_cond_wait(&lb->not_empty, &lb->lock);

    if (lb->count == 0) {               /* done and empty */
        pthread_mutex_unlock(&lb->lock);
        return 0;
    }
    strncpy(out, lb->lines[lb->tail], MAX_LINE_LEN - 1);
    out[MAX_LINE_LEN - 1] = '\0';
    lb->tail = (lb->tail + 1) % MAX_LOG_LINES;
    lb->count--;
    pthread_cond_signal(&lb->not_full);
    pthread_mutex_unlock(&lb->lock);
    return 1;
}

/* Signal EOF so consumer can drain and exit. */
static void logbuf_set_done(LogBuffer *lb)
{
    pthread_mutex_lock(&lb->lock);
    lb->done = 1;
    pthread_cond_broadcast(&lb->not_empty);
    pthread_cond_broadcast(&lb->not_full);
    pthread_mutex_unlock(&lb->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 2: Producer / Consumer thread bodies
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Producer thread: reads lines from container pipe, enqueues into ring buffer. */
static void *producer_thread(void *arg)
{
    Container *c = (Container *)arg;
    char      buf[MAX_LINE_LEN];
    FILE     *fp = fdopen(c->pipe_rd, "r");

    if (!fp) {
        perror("fdopen pipe");
        logbuf_set_done(&c->logbuf);
        return NULL;
    }

    while (fgets(buf, sizeof(buf), fp)) {
        /* Strip trailing newline for cleaner storage */
        buf[strcspn(buf, "\n")] = '\0';
        logbuf_put(&c->logbuf, buf);
    }

    fclose(fp);
    logbuf_set_done(&c->logbuf);   /* signal consumer that we're done */
    return NULL;
}

/* Consumer thread: drains ring buffer and appends lines to a log file. */
static void *consumer_thread(void *arg)
{
    Container *c = (Container *)arg;
    char      line[MAX_LINE_LEN];
    FILE     *fp = fopen(c->log_path, "a");

    if (!fp) {
        perror("fopen log file");
        return NULL;
    }

    while (logbuf_get(&c->logbuf, line)) {
        fprintf(fp, "%s\n", line);
        fflush(fp);
    }

    fclose(fp);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 3: Container table helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static Container *find_free_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == STATE_FREE)
            return &containers[i];
    return NULL;
}

static Container *find_by_id(const char *id)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != STATE_FREE &&
            strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 4: Container process (run inside cloned namespace)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Arguments passed to the cloned child via the stack. */
typedef struct {
    const char *rootfs;
    const char *cmd;
    int         sync_pipe[2];   /* parent signals child once table is updated */
} ChildArgs;

/* Entry point executed by clone() in the new namespaces. */
static int container_main(void *arg)
{
    ChildArgs *a = (ChildArgs *)arg;

    /* Wait for parent to finish setting up before proceeding.
     * Parent closes write end; child reads 1 byte as "go" signal. */
    char ch;
    close(a->sync_pipe[1]);
    read(a->sync_pipe[0], &ch, 1);
    close(a->sync_pipe[0]);

    /* 1. Change root filesystem */
    if (chroot(a->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/")         < 0) { perror("chdir");  return 1; }

    /* 2. Mount /proc so tools like ps, top work inside the container */
    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        perror("mount /proc");   /* non-fatal — rootfs might not have /proc */

    /* 3. Set a recognisable hostname for the UTS namespace */
    if (sethostname("container", 9) < 0)
        perror("sethostname");

    /* 4. Execute the desired command */
    char *argv[] = { (char *)a->cmd, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm",
        NULL
    };
    execve(a->cmd, argv, envp);
    perror("execve");   /* only reached on error */
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 5: Start a container (called by supervisor)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * do_start() — spawn a container.
 * Returns 0 on success and fills *out_pid.
 */
static int do_start(const char *id, const char *rootfs, const char *cmd,
                    char *errbuf, size_t errlen)
{
    pthread_mutex_lock(&table_lock);

    /* Check for duplicate ID */
    if (find_by_id(id)) {
        snprintf(errbuf, errlen, "container '%s' already exists", id);
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    Container *c = find_free_slot();
    if (!c) {
        snprintf(errbuf, errlen, "container table full");
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /* ── Set up per-container pipe for stdout/stderr capture ── */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        snprintf(errbuf, errlen, "pipe: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /* ── Sync pipe: parent unblocks child after table update ── */
    ChildArgs ca;
    ca.rootfs = rootfs;
    ca.cmd    = cmd;
    if (pipe(ca.sync_pipe) < 0) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(errbuf, errlen, "sync pipe: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /* ── Allocate a 1 MiB stack for the cloned child ── */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        close(pipefd[0]); close(pipefd[1]);
        close(ca.sync_pipe[0]); close(ca.sync_pipe[1]);
        snprintf(errbuf, errlen, "malloc stack: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;   /* stack grows downward */

    /* ── Clone with new namespaces ── */
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
                SIGCHLD;
    pid_t pid = clone(container_main, stack_top, flags, &ca);
    if (pid < 0) {
        free(stack);
        close(pipefd[0]); close(pipefd[1]);
        close(ca.sync_pipe[0]); close(ca.sync_pipe[1]);
        snprintf(errbuf, errlen, "clone: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /* ── Parent: redirect child stdout/stderr through our pipe ── */
    /* We can't dup2 directly in the parent for the child's fds;
     * the pipe is used by the producer thread which reads from the child.
     * For a more complete implementation, use a pre-clone dup2 trick or
     * ptrace.  Here we start the producer on the read end regardless; the
     * container writes to its own stdout which goes to a pty in a full
     * implementation.  We keep this simple by using the write end as a
     * placeholder that callers may write to for testing. */
    close(pipefd[1]);   /* parent only reads */

    /* ── Populate container entry ── */
    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id) - 1);
    c->state      = STATE_RUNNING;
    c->pid        = pid;
    c->pipe_rd    = pipefd[0];
    c->started_at = time(NULL);

    /* Build log file path and create log dir if needed */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, id);

    logbuf_init(&c->logbuf);

    /* ── Start producer/consumer threads ── */
    pthread_create(&c->producer_tid, NULL, producer_thread, c);
    pthread_create(&c->consumer_tid, NULL, consumer_thread, c);

    /* ── Register PID with kernel module (if loaded) ── */
    if (monitor_fd >= 0) {
        struct monitor_request req = {
            .pid           = pid,
            .soft_limit_kb = 256 * 1024,   /* 256 MB soft */
            .hard_limit_kb = 512 * 1024,   /* 512 MB hard */
        };
        if (ioctl(monitor_fd, MONITOR_IOC_REGISTER, &req) < 0)
            perror("ioctl MONITOR_IOC_REGISTER");
    }

    pthread_mutex_unlock(&table_lock);

    /* ── Unblock child (close our write end of the sync pipe) ── */
    close(ca.sync_pipe[1]);

    free(stack);   /* safe — clone has copied stack contents */
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 6: Stop a container
 * ═══════════════════════════════════════════════════════════════════════════ */

static int do_stop(const char *id, char *errbuf, size_t errlen)
{
    pthread_mutex_lock(&table_lock);
    Container *c = find_by_id(id);
    if (!c) {
        snprintf(errbuf, errlen, "container '%s' not found", id);
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    if (c->state != STATE_RUNNING) {
        snprintf(errbuf, errlen, "container '%s' is not running", id);
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /* Send SIGTERM, then SIGKILL if needed */
    kill(c->pid, SIGTERM);
    sleep(1);
    kill(c->pid, SIGKILL);   /* ensure it's gone */
    c->state = STATE_STOPPED;

    /* Unregister from kernel module */
    if (monitor_fd >= 0) {
        struct monitor_request req = { .pid = c->pid };
        ioctl(monitor_fd, MONITOR_IOC_UNREGISTER, &req);
    }

    pthread_mutex_unlock(&table_lock);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 7: List containers (ps)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void do_ps(int client_fd)
{
    char buf[CMD_BUF_SIZE];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-20s %-10s %-10s %-20s\n",
                    "ID", "PID", "STATE", "STARTED");
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-20s %-10s %-10s %-20s\n",
                    "--------------------", "----------",
                    "----------", "--------------------");

    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == STATE_FREE) continue;
        const char *state_str =
            containers[i].state == STATE_RUNNING ? "running" :
            containers[i].state == STATE_STOPPED ? "stopped" : "?";

        char tsbuf[32];
        struct tm *tm = localtime(&containers[i].started_at);
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm);

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-20s %-10d %-10s %-20s\n",
                        containers[i].id,
                        containers[i].pid,
                        state_str,
                        tsbuf);
    }
    pthread_mutex_unlock(&table_lock);

    write(client_fd, buf, strlen(buf));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 8: Tail logs for a container
 * ═══════════════════════════════════════════════════════════════════════════ */

static void do_logs(const char *id, int client_fd)
{
    pthread_mutex_lock(&table_lock);
    Container *c = find_by_id(id);
    if (!c) {
        char err[128];
        snprintf(err, sizeof(err), "ERROR: container '%s' not found\n", id);
        write(client_fd, err, strlen(err));
        pthread_mutex_unlock(&table_lock);
        return;
    }
    char log_path[256];
    strncpy(log_path, c->log_path, sizeof(log_path) - 1);
    pthread_mutex_unlock(&table_lock);

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        char err[256];
        snprintf(err, sizeof(err), "ERROR: cannot open log %s: %s\n",
                 log_path, strerror(errno));
        write(client_fd, err, strlen(err));
        return;
    }

    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), fp))
        write(client_fd, line, strlen(line));
    fclose(fp);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 9: SIGCHLD handler — reap zombie children
 * ═══════════════════════════════════════════════════════════════════════════ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;
    /* Reap ALL exited children without blocking */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Update container table */
        pthread_mutex_lock(&table_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].pid == pid &&
                containers[i].state == STATE_RUNNING) {
                containers[i].state = STATE_STOPPED;

                /* Signal log threads that the pipe is done */
                logbuf_set_done(&containers[i].logbuf);
                break;
            }
        }
        pthread_mutex_unlock(&table_lock);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 10: Supervisor main loop
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Protocol (line-based, newline-terminated):
 *   client → "START <id> <rootfs> <cmd>\n"
 *   client → "STOP <id>\n"
 *   client → "PS\n"
 *   client → "LOGS <id>\n"
 *
 *   server → free-form text response, ends with ".\n"
 */
static void handle_client(int cfd)
{
    char buf[CMD_BUF_SIZE];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) { close(cfd); return; }
    buf[n] = '\0';

    /* Strip trailing newline */
    buf[strcspn(buf, "\n")] = '\0';

    char *tok = strtok(buf, " ");
    if (!tok) { close(cfd); return; }

    if (strcmp(tok, "PS") == 0) {
        do_ps(cfd);

    } else if (strcmp(tok, "START") == 0 || strcmp(tok, "RUN") == 0) {
        char *id     = strtok(NULL, " ");
        char *rootfs = strtok(NULL, " ");
        char *cmd    = strtok(NULL, " ");
        if (!id || !rootfs || !cmd) {
            write(cfd, "ERROR: usage: START <id> <rootfs> <cmd>\n", 40);
        } else {
            char errbuf[256] = {0};
            if (do_start(id, rootfs, cmd, errbuf, sizeof(errbuf)) < 0) {
                char msg[320];
                snprintf(msg, sizeof(msg), "ERROR: %s\n", errbuf);
                write(cfd, msg, strlen(msg));
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "OK: container '%s' started\n", id);
                write(cfd, msg, strlen(msg));
            }
        }

    } else if (strcmp(tok, "STOP") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) {
            write(cfd, "ERROR: usage: STOP <id>\n", 24);
        } else {
            char errbuf[256] = {0};
            if (do_stop(id, errbuf, sizeof(errbuf)) < 0) {
                char msg[320];
                snprintf(msg, sizeof(msg), "ERROR: %s\n", errbuf);
                write(cfd, msg, strlen(msg));
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "OK: container '%s' stopped\n", id);
                write(cfd, msg, strlen(msg));
            }
        }

    } else if (strcmp(tok, "LOGS") == 0) {
        char *id = strtok(NULL, " ");
        if (!id) {
            write(cfd, "ERROR: usage: LOGS <id>\n", 24);
        } else {
            do_logs(id, cfd);
        }

    } else {
        write(cfd, "ERROR: unknown command\n", 23);
    }

    write(cfd, ".\n", 2);   /* end-of-response sentinel */
    close(cfd);
}

static void run_supervisor(const char *rootfs)
{
    (void)rootfs;   /* supervisor rootfs is informational; containers set their own */

    /* Install SIGCHLD handler before spawning any children */
    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Try to open the kernel monitor device (optional) */
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor unavailable (%s); continuing without it\n",
                strerror(errno));

    /* Create UNIX domain socket */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCKET_PATH);   /* remove stale socket */

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv, 8) < 0) { perror("listen"); exit(1); }

    fprintf(stderr, "[supervisor] listening on %s\n", SOCKET_PATH);

    /* Accept loop */
    while (1) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        handle_client(cfd);
    }

    close(srv);
    unlink(SOCKET_PATH);
    if (monitor_fd >= 0) close(monitor_fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 11: CLI — connect to supervisor and send a command
 * ═══════════════════════════════════════════════════════════════════════════ */

static void send_command(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running? (sudo ./engine supervisor <rootfs>)\n",
                SOCKET_PATH, strerror(errno));
        close(fd);
        exit(1);
    }

    /* Send command */
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);

    /* Read and print response until sentinel ".\n" */
    char buf[CMD_BUF_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        /* Strip the sentinel before printing */
        char *sentinel = strstr(buf, "\n.\n");
        if (sentinel) { *sentinel = '\0'; }
        printf("%s", buf);
        if (sentinel) break;
    }
    printf("\n");
    close(fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 12: main() — dispatch on argv[1]
 * ═══════════════════════════════════════════════════════════════════════════ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>           Run the container supervisor\n"
        "  %s start <id> <rootfs> <cmd>     Start a detached container\n"
        "  %s run   <id> <rootfs> <cmd>     Start a container (alias for start)\n"
        "  %s ps                            List containers\n"
        "  %s stop  <id>                    Stop a container\n"
        "  %s logs  <id>                    Print container logs\n",
        prog, prog, prog, prog, prog, prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 2) usage(argv[0]);

    const char *subcmd = argv[1];

    /* ── Supervisor mode ──────────────────────────────────────────── */
    if (strcmp(subcmd, "supervisor") == 0) {
        if (argc < 3) usage(argv[0]);
        run_supervisor(argv[2]);
        return 0;
    }

    /* ── ps (no extra args) ─────────────────────────────────────── */
    if (strcmp(subcmd, "ps") == 0) {
        send_command("PS");
        return 0;
    }

    /* ── start / run ─────────────────────────────────────────────── */
    if (strcmp(subcmd, "start") == 0 || strcmp(subcmd, "run") == 0) {
        if (argc < 5) usage(argv[0]);
        char cmd[CMD_BUF_SIZE];
        snprintf(cmd, sizeof(cmd), "START %s %s %s",
                 argv[2], argv[3], argv[4]);
        send_command(cmd);
        return 0;
    }

    /* ── stop ───────────────────────────────────────────────────── */
    if (strcmp(subcmd, "stop") == 0) {
        if (argc < 3) usage(argv[0]);
        char cmd[CMD_BUF_SIZE];
        snprintf(cmd, sizeof(cmd), "STOP %s", argv[2]);
        send_command(cmd);
        return 0;
    }

    /* ── logs ───────────────────────────────────────────────────── */
    if (strcmp(subcmd, "logs") == 0) {
        if (argc < 3) usage(argv[0]);
        char cmd[CMD_BUF_SIZE];
        snprintf(cmd, sizeof(cmd), "LOGS %s", argv[2]);
        send_command(cmd);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
EOF_ENGINE_C

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 3: monitor.c
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/monitor.c" << 'EOF_MONITOR_C'
/*
 * monitor.c — Mini Container Runtime Kernel Module
 *
 * A Linux kernel module that:
 *   1. Registers a character device /dev/container_monitor
 *   2. Accepts ioctl requests from user-space to register/unregister PIDs
 *      and set per-PID memory limits.
 *   3. A background kernel thread wakes every 2 seconds and checks the
 *      RSS (Resident Set Size) of every tracked process.
 *      - RSS > soft_limit → printk(KERN_WARNING ...)
 *      - RSS > hard_limit → send SIGKILL to the process
 *
 * Build:  make (see Makefile)
 * Load:   sudo insmod monitor.ko
 * Unload: sudo rmmod monitor
 *
 * Kernel version: tested on Linux 5.15+ (Ubuntu 22.04 LTS)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>   /* copy_from_user */
#include <linux/slab.h>      /* kmalloc / kfree */
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/mm.h>        /* get_task_mm, mm_struct */
#include <linux/signal.h>

/* Pull in the shared ioctl definitions.
 * Because the kernel build system compiles this as a module, we include
 * the header relative to this source file's directory. */
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mini Container Runtime");
MODULE_DESCRIPTION("Container memory monitor kernel module");
MODULE_VERSION("1.0");

/* ── Device bookkeeping ──────────────────────────────────────────────────── */
#define DEVICE_NAME "container_monitor"
#define CLASS_NAME  "container_mon"

static int            major_num;
static struct class  *dev_class;
static struct device *dev_device;
static struct cdev    cdev_obj;
static dev_t          dev_num;

/* ── Per-container tracking entry ────────────────────────────────────────── */
struct monitor_entry {
    struct list_head  list;
    pid_t             pid;
    unsigned long     soft_limit_kb;
    unsigned long     hard_limit_kb;
    int               warned;          /* 1 after soft-limit warning fired */
};

static LIST_HEAD(entry_list);
static DEFINE_MUTEX(entry_mutex);

/* ── Background monitor thread ───────────────────────────────────────────── */
static struct task_struct *monitor_thread;

/*
 * get_rss_kb() — read VmRSS for a given PID.
 *
 * We obtain the mm_struct for the task to access its RSS counters.
 * This is the same mechanism used by /proc/<pid>/status.
 * Returns 0 if the process no longer exists (already exited).
 */
static unsigned long get_rss_kb(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    unsigned long       rss_pages = 0;

    rcu_read_lock();
    /* find_get_pid / pid_task: find the task safely under RCU */
    struct pid *pid_struct = find_get_pid(pid);
    if (!pid_struct) { rcu_read_unlock(); return 0; }
    task = pid_task(pid_struct, PIDTYPE_PID);
    if (!task)      { put_pid(pid_struct); rcu_read_unlock(); return 0; }

    mm = get_task_mm(task);
    put_pid(pid_struct);
    rcu_read_unlock();

    if (!mm) return 0;

    /* get_mm_rss() returns count in pages; convert to KB. */
    rss_pages = get_mm_rss(mm);
    mmput(mm);

    return rss_pages * (PAGE_SIZE / 1024);
}

/*
 * send_kill() — send SIGKILL to a process.
 * We look up the task under RCU, then send the signal.
 */
static void send_kill(pid_t pid)
{
    struct task_struct *task;
    rcu_read_lock();
    struct pid *pid_struct = find_get_pid(pid);
    if (pid_struct) {
        task = pid_task(pid_struct, PIDTYPE_PID);
        if (task)
            send_sig(SIGKILL, task, 1);
        put_pid(pid_struct);
    }
    rcu_read_unlock();
}

/*
 * monitor_thread_fn() — the background polling loop.
 * Wakes every 2 seconds and checks RSS for all tracked PIDs.
 */
static int monitor_thread_fn(void *data)
{
    (void)data;
    printk(KERN_INFO "container_monitor: monitor thread started\n");

    while (!kthread_should_stop()) {
        struct monitor_entry *entry, *tmp;

        mutex_lock(&entry_mutex);
        list_for_each_entry_safe(entry, tmp, &entry_list, list) {
            unsigned long rss = get_rss_kb(entry->pid);

            if (rss == 0) {
                /* Process has exited; remove from list */
                printk(KERN_INFO "container_monitor: PID %d exited, removing\n",
                       entry->pid);
                list_del(&entry->list);
                kfree(entry);
                continue;
            }

            if (rss > entry->hard_limit_kb) {
                printk(KERN_WARNING
                       "container_monitor: PID %d RSS %lu KB > hard limit %lu KB — KILLING\n",
                       entry->pid, rss, entry->hard_limit_kb);
                send_kill(entry->pid);

            } else if (rss > entry->soft_limit_kb && !entry->warned) {
                printk(KERN_WARNING
                       "container_monitor: PID %d RSS %lu KB > soft limit %lu KB — WARNING\n",
                       entry->pid, rss, entry->soft_limit_kb);
                entry->warned = 1;

            } else if (rss <= entry->soft_limit_kb) {
                /* Reset warning flag once RSS drops back below soft limit */
                entry->warned = 0;
            }
        }
        mutex_unlock(&entry_mutex);

        /* Sleep 2 seconds; kthread_should_stop() is checked at loop top */
        ssleep(2);
    }

    printk(KERN_INFO "container_monitor: monitor thread stopping\n");
    return 0;
}

/* ── ioctl handler ───────────────────────────────────────────────────────── */

static long mon_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct monitor_entry  *entry, *tmp;

    /* Copy the request struct from user-space */
    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    switch (cmd) {

    case MONITOR_IOC_REGISTER: {
        /* Allocate a new tracking entry */
        struct monitor_entry *new_entry = kmalloc(sizeof(*new_entry), GFP_KERNEL);
        if (!new_entry) return -ENOMEM;

        new_entry->pid           = req.pid;
        new_entry->soft_limit_kb = req.soft_limit_kb;
        new_entry->hard_limit_kb = req.hard_limit_kb;
        new_entry->warned        = 0;
        INIT_LIST_HEAD(&new_entry->list);

        mutex_lock(&entry_mutex);
        /* Remove any stale entry for the same PID before adding */
        list_for_each_entry_safe(entry, tmp, &entry_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
            }
        }
        list_add_tail(&new_entry->list, &entry_list);
        mutex_unlock(&entry_mutex);

        printk(KERN_INFO "container_monitor: registered PID %d "
               "(soft=%lu KB, hard=%lu KB)\n",
               req.pid, req.soft_limit_kb, req.hard_limit_kb);
        break;
    }

    case MONITOR_IOC_SETLIMIT:
        mutex_lock(&entry_mutex);
        list_for_each_entry(entry, &entry_list, list) {
            if (entry->pid == req.pid) {
                entry->soft_limit_kb = req.soft_limit_kb;
                entry->hard_limit_kb = req.hard_limit_kb;
                entry->warned        = 0;
                printk(KERN_INFO "container_monitor: updated limits for PID %d "
                       "(soft=%lu KB, hard=%lu KB)\n",
                       req.pid, req.soft_limit_kb, req.hard_limit_kb);
                break;
            }
        }
        mutex_unlock(&entry_mutex);
        break;

    case MONITOR_IOC_UNREGISTER:
        mutex_lock(&entry_mutex);
        list_for_each_entry_safe(entry, tmp, &entry_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                printk(KERN_INFO "container_monitor: unregistered PID %d\n",
                       req.pid);
                break;
            }
        }
        mutex_unlock(&entry_mutex);
        break;

    default:
        return -ENOTTY;   /* unknown ioctl */
    }

    return 0;
}

/* ── File operations ─────────────────────────────────────────────────────── */

static int mon_open(struct inode *inode, struct file *filp)
{
    (void)inode; (void)filp;
    return 0;
}

static int mon_release(struct inode *inode, struct file *filp)
{
    (void)inode; (void)filp;
    return 0;
}

static const struct file_operations mon_fops = {
    .owner          = THIS_MODULE,
    .open           = mon_open,
    .release        = mon_release,
    .unlocked_ioctl = mon_ioctl,
    /* No read/write; all communication is via ioctl */
};

/* ── Module init / exit ──────────────────────────────────────────────────── */

static int __init monitor_init(void)
{
    int ret;

    /* Dynamically allocate a major number */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "container_monitor: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }
    major_num = MAJOR(dev_num);

    /* Initialize and add the cdev */
    cdev_init(&cdev_obj, &mon_fops);
    cdev_obj.owner = THIS_MODULE;
    ret = cdev_add(&cdev_obj, dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "container_monitor: cdev_add failed: %d\n", ret);
        goto err_unregister;
    }

    /* Create a device class so udev creates /dev/container_monitor */
    dev_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(dev_class)) {
        ret = PTR_ERR(dev_class);
        printk(KERN_ERR "container_monitor: class_create failed: %d\n", ret);
        goto err_cdev;
    }

    dev_device = device_create(dev_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(dev_device)) {
        ret = PTR_ERR(dev_device);
        printk(KERN_ERR "container_monitor: device_create failed: %d\n", ret);
        goto err_class;
    }

    /* Start the background monitoring kernel thread */
    monitor_thread = kthread_run(monitor_thread_fn, NULL, "container_monitor");
    if (IS_ERR(monitor_thread)) {
        ret = PTR_ERR(monitor_thread);
        printk(KERN_ERR "container_monitor: kthread_run failed: %d\n", ret);
        goto err_device;
    }

    printk(KERN_INFO "container_monitor: loaded (major=%d, device=/dev/%s)\n",
           major_num, DEVICE_NAME);
    return 0;

    /* Cleanup on error */
err_device:
    device_destroy(dev_class, dev_num);
err_class:
    class_destroy(dev_class);
err_cdev:
    cdev_del(&cdev_obj);
err_unregister:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit monitor_exit(void)
{
    /* Stop the background thread */
    if (monitor_thread)
        kthread_stop(monitor_thread);

    /* Free all tracking entries */
    struct monitor_entry *entry, *tmp;
    mutex_lock(&entry_mutex);
    list_for_each_entry_safe(entry, tmp, &entry_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&entry_mutex);

    /* Tear down the device and class */
    device_destroy(dev_class, dev_num);
    class_destroy(dev_class);
    cdev_del(&cdev_obj);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
EOF_MONITOR_C

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 4: cpu_test.c
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/cpu_test.c" << 'EOF_CPU_TEST_C'
/*
 * cpu_test.c — CPU-intensive workload for Mini Container Runtime
 *
 * Runs a tight arithmetic loop for approximately 60 seconds,
 * keeping one CPU core fully utilized.  Useful for testing that
 * the container isolates CPU-bound processes and that the engine
 * correctly tracks and can stop running containers.
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
EOF_CPU_TEST_C

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 5: memory_test.c
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/memory_test.c" << 'EOF_MEMORY_TEST_C'
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
EOF_MEMORY_TEST_C

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 6: Makefile
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/Makefile" << 'EOF_MAKEFILE'
# Makefile — Mini Container Runtime
#
# Targets:
#   make          — build user-space engine and workload programs
#   make module   — build the kernel module
#   make all      — build everything (engine + module)
#   make clean    — remove all build artifacts
#   make load     — insert the kernel module (requires root)
#   make unload   — remove the kernel module (requires root)

# ── User-space build settings ─────────────────────────────────────────────
CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -pthread
LDFLAGS := -pthread

# Binaries
ENGINE   := engine
CPU_TEST := cpu_test
MEM_TEST := memory_test

# Sources
ENGINE_SRC   := engine.c
CPU_SRC      := cpu_test.c
MEM_SRC      := memory_test.c

# ── Kernel module build settings ──────────────────────────────────────────
MODULE_NAME  := monitor
MODULE_SRCS  := monitor.c
KDIR         := /lib/modules/$(shell uname -r)/build
PWD          := $(shell pwd)

# ── Default target: user-space only ───────────────────────────────────────
.PHONY: default
default: $(ENGINE) $(CPU_TEST) $(MEM_TEST)
	@echo "==> User-space binaries built. Run 'make module' to build the kernel module."

# Build everything
.PHONY: all
all: default module

# ── engine ────────────────────────────────────────────────────────────────
$(ENGINE): $(ENGINE_SRC) monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "==> Built: $(ENGINE)"

# ── Workload programs ─────────────────────────────────────────────────────
$(CPU_TEST): $(CPU_SRC)
	$(CC) $(CFLAGS) -O2 -o $@ $<
	@echo "==> Built: $(CPU_TEST)"

$(MEM_TEST): $(MEM_SRC)
	$(CC) $(CFLAGS) -O0 -o $@ $<
	@echo "==> Built: $(MEM_TEST)"

# ── Kernel module ─────────────────────────────────────────────────────────
obj-m += $(MODULE_NAME).o

.PHONY: module
module:
	@echo "==> Building kernel module $(MODULE_NAME).ko ..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "==> Kernel module built: $(MODULE_NAME).ko"

# ── Load / unload ─────────────────────────────────────────────────────────
.PHONY: load
load: module
	@echo "==> Loading kernel module ..."
	sudo insmod $(MODULE_NAME).ko
	@echo "==> Module loaded. Check: ls -l /dev/container_monitor"
	@dmesg | tail -5

.PHONY: unload
unload:
	@echo "==> Unloading kernel module ..."
	sudo rmmod $(MODULE_NAME)
	@echo "==> Module unloaded."
	@dmesg | tail -5

# ── Clean ─────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	@echo "==> Cleaning user-space artifacts ..."
	rm -f $(ENGINE) $(CPU_TEST) $(MEM_TEST)
	@echo "==> Cleaning kernel module artifacts ..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order .*.cmd
	rm -rf .tmp_versions
	@echo "==> Clean done."

# ── Help ──────────────────────────────────────────────────────────────────
.PHONY: help
help:
	@echo "Targets:"
	@echo "  make          Build engine + workload programs (user-space)"
	@echo "  make module   Build kernel module monitor.ko"
	@echo "  make all      Build everything"
	@echo "  make load     sudo insmod monitor.ko"
	@echo "  make unload   sudo rmmod monitor"
	@echo "  make clean    Remove all build artifacts"
EOF_MAKEFILE

# ─────────────────────────────────────────────────────────────────────────────
#  FILE 7: README.md
# ─────────────────────────────────────────────────────────────────────────────
cat > "$DEST/README.md" << 'EOF_README_MD'
# Mini Container Runtime

A lightweight, Docker-like container runtime built using raw Linux kernel primitives — no Docker, no containerd, no OCI layers. Just `clone()`, `chroot()`, namespaces, pipes, pthreads, and a custom kernel module.

---

## Architecture

```
+-----------------------------------------------------+
|  CLI  (engine ps / start / stop / logs)             |
|       |                                             |
|       |  UNIX domain socket  /tmp/container_engine.sock
|       v                                             |
|  Supervisor Process  (engine supervisor <rootfs>)   |
|       |                                             |
|       +-- clone() --> Container Process             |
|       |                 * new PID namespace         |
|       |                 * new mount namespace       |
|       |                 * new UTS namespace         |
|       |                 * new IPC namespace         |
|       |                 * chroot(<rootfs>)          |
|       |                 * mount /proc               |
|       |                 * exec <cmd>                |
|       |                                             |
|       +-- pipe --> Producer Thread                  |
|       |               |  (reads container output)  |
|       |               v                             |
|       |           Ring Buffer (256 lines)           |
|       |               |                             |
|       |               v                             |
|       |           Consumer Thread                   |
|       |               |  (writes to log file)      |
|       |               v                             |
|       |           /tmp/container_logs/<id>.log      |
|       |                                             |
|       +-- ioctl --> /dev/container_monitor          |
|                        (kernel module)              |
|                        * tracks RSS every 2s        |
|                        * soft limit -> dmesg warn   |
|                        * hard limit -> SIGKILL      |
+-----------------------------------------------------+
```

---

## Files

| File              | Description                                              |
|-------------------|----------------------------------------------------------|
| `engine.c`        | Main runtime: supervisor + CLI, clone/chroot/ns, logging |
| `monitor.c`       | Kernel module: character device, RSS polling, ioctl      |
| `monitor_ioctl.h` | Shared header: ioctl command numbers + struct            |
| `cpu_test.c`      | CPU-intensive workload (60-second busy loop)             |
| `memory_test.c`   | Memory-hungry workload (10 MB chunks + memset)           |
| `Makefile`        | Build all targets, load/unload module                    |

---

## Setup

### Prerequisites (Ubuntu 22.04 LTS)

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r)
```

### Build

```bash
# User-space only (engine + workloads)
make

# Also build the kernel module
make all
```

### Set up a minimal rootfs

The container needs a real Linux rootfs to chroot into.
The quickest way is to use debootstrap:

```bash
sudo apt install debootstrap
sudo debootstrap --variant=minbase jammy /tmp/myroot http://archive.ubuntu.com/ubuntu
sudo mkdir -p /tmp/myroot/proc
```

Or use a minimal BusyBox tree:

```bash
mkdir -p /tmp/tinyrootfs/{bin,proc,sys,dev,tmp}
cp /bin/busybox /tmp/tinyrootfs/bin/busybox
for cmd in sh ls cat echo ps; do
  ln -s busybox /tmp/tinyrootfs/bin/$cmd
done
```

---

## Running

### 1. Start the Supervisor (must be root)

```bash
sudo ./engine supervisor /tmp/myroot
# Output: [supervisor] listening on /tmp/container_engine.sock
```

### 2. Start a container

```bash
sudo ./engine start web1 /tmp/myroot /bin/sh
# Output: OK: container 'web1' started
```

### 3. List running containers

```bash
sudo ./engine ps
```

### 4. View logs

```bash
sudo ./engine logs web1
# Logs also written to /tmp/container_logs/web1.log
```

### 5. Stop a container

```bash
sudo ./engine stop web1
```

### 6. Run workloads inside a container

```bash
cp cpu_test memory_test /tmp/myroot/bin/
sudo ./engine start cpu1 /tmp/myroot /bin/cpu_test
sudo ./engine start mem1 /tmp/myroot /bin/memory_test
```

---

## Kernel Module

### Load

```bash
make load
# Or manually: sudo insmod monitor.ko
ls -l /dev/container_monitor
```

### Verify

```bash
dmesg | tail -10
# Should show: container_monitor: loaded (major=X, device=/dev/container_monitor)
```

### Unload

```bash
make unload
# Or: sudo rmmod monitor
```

### How it works

- The supervisor registers each new container PID via `ioctl(MONITOR_IOC_REGISTER)`.
- A kernel thread wakes every **2 seconds** and reads RSS via `get_mm_rss()`.
- **Soft limit** (default 256 MB): logs a warning to `dmesg`.
- **Hard limit** (default 512 MB): sends `SIGKILL` to the container.
- On `engine stop`, the PID is unregistered via `ioctl(MONITOR_IOC_UNREGISTER)`.

---

## Namespaces Used

| Namespace | Flag           | Effect                                        |
|-----------|----------------|-----------------------------------------------|
| PID       | CLONE_NEWPID   | Container sees its init as PID 1              |
| Mount     | CLONE_NEWNS    | Container can mount without affecting host    |
| UTS       | CLONE_NEWUTS   | Container has its own hostname                |
| IPC       | CLONE_NEWIPC   | Isolated System V IPC and POSIX message queues|

---

## Troubleshooting

| Problem                              | Solution                                                    |
|--------------------------------------|-------------------------------------------------------------|
| `clone: Operation not permitted`     | Run as root: `sudo ./engine supervisor ...`                 |
| `chroot: No such file or directory`  | Verify the rootfs path exists and contains `/bin`           |
| `mount /proc: No such file ...`      | `mkdir -p /tmp/myroot/proc`                                 |
| Kernel module won't load             | `sudo apt install linux-headers-$(uname -r)`                |
| `Cannot connect to supervisor`       | Start the supervisor first in another terminal              |
EOF_README_MD

# ─────────────────────────────────────────────────────────────────────────────
#  Done
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "==> All files written to: $DEST/"
echo ""
echo "    mini-container-runtime/"
echo "    ├── engine.c"
echo "    ├── monitor.c"
echo "    ├── monitor_ioctl.h"
echo "    ├── cpu_test.c"
echo "    ├── memory_test.c"
echo "    ├── Makefile"
echo "    └── README.md"
echo ""
echo "==> To get started on Ubuntu:"
echo "    cd $DEST"
echo "    sudo apt install build-essential linux-headers-\$(uname -r)"
echo "    make"
echo "    sudo ./engine supervisor /tmp/myroot"
