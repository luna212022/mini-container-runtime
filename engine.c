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
