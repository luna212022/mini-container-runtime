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
 *
 * Bug fixes applied (vs boilerplate version):
 *   1. SIGCHLD handler no longer calls pthread_mutex_lock (async-signal-unsafe).
 *      A self-pipe is used: handler writes 1 byte; main accept loop selects on
 *      the read end and calls reap_children() from safe context.
 *   2. CloneArgs allocated on heap (calloc), freed in parent after clone().
 *      Previous on-stack version was unsafe if the parent returned before the
 *      child consumed the struct.
 *   3. Stdout/stderr pipe uses pipe2(..., O_CLOEXEC) so grandchildren of the
 *      container exec do not inherit the write end, keeping pipes open.
 *   4. 'run' command now blocks until container exits and returns exit status.
 *   5. Container metadata tracks soft/hard limits, nice value, stop_requested
 *      flag, exit status, and termination reason for correct attribution.
 */

#define _GNU_SOURCE   /* for clone(), pipe2(), unshare(), etc. */

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
#include <sys/select.h>
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

/* Default memory limits (MiB → KB) */
#define DEFAULT_SOFT_MIB  40
#define DEFAULT_HARD_MIB  64

/* ── Container states ───────────────────────────────────────────────────── */
typedef enum {
    STATE_FREE    = 0,
    STATE_RUNNING = 1,
    STATE_STOPPED = 2,
} ContainerState;

/* ── Termination reason (for attribution in ps output) ─────────────────── */
typedef enum {
    TERM_NONE          = 0,
    TERM_NORMAL        = 1,   /* exited on its own */
    TERM_STOPPED       = 2,   /* killed by 'engine stop' */
    TERM_HARD_LIMIT    = 3,   /* killed by kernel module (SIGKILL, no stop_requested) */
} TermReason;

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

    /* Resource limits */
    unsigned long   soft_limit_mib;
    unsigned long   hard_limit_mib;
    int             nice_val;

    /* Termination tracking */
    int             stop_requested;  /* set before sending SIGTERM/SIGKILL from stop */
    int             exit_status;     /* raw waitpid status */
    TermReason      term_reason;
} Container;

/* ── Globals ────────────────────────────────────────────────────────────── */
static Container  containers[MAX_CONTAINERS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static int        monitor_fd = -1;   /* fd for /dev/container_monitor */

/*
 * Self-pipe for async-signal-safe SIGCHLD delivery.
 * The SIGCHLD handler writes 1 byte to g_sigpipe_w.
 * The main accept loop selects on g_sigpipe_r and calls reap_children().
 */
static int g_sigpipe_r = -1;
static int g_sigpipe_w = -1;

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
 *  Section 4: SIGCHLD self-pipe and reaper
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * The SIGCHLD signal handler ONLY writes one byte to the write end of the
 * self-pipe.  write(2) is async-signal-safe.  No mutex, no waitpid here.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    char byte = 1;
    /* Ignore the return value — if the pipe is full we'll still loop. */
    (void)write(g_sigpipe_w, &byte, 1);
}

/*
 * reap_children() — called from the main loop (NOT from a signal handler).
 * Safe to call pthread_mutex_lock here.
 */
static void reap_children(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&table_lock);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].pid == pid &&
                containers[i].state == STATE_RUNNING) {

                containers[i].state       = STATE_STOPPED;
                containers[i].exit_status = status;

                /* Attribution logic (Task 4 requirement):
                 *   - stop_requested set      → TERM_STOPPED
                 *   - SIGKILL without request  → TERM_HARD_LIMIT (from kernel module)
                 *   - otherwise               → TERM_NORMAL */
                if (containers[i].stop_requested) {
                    containers[i].term_reason = TERM_STOPPED;
                } else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
                    containers[i].term_reason = TERM_HARD_LIMIT;
                } else {
                    containers[i].term_reason = TERM_NORMAL;
                }

                /* Signal log threads that the pipe is done */
                logbuf_set_done(&containers[i].logbuf);
                break;
            }
        }
        pthread_mutex_unlock(&table_lock);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 5: Container process (run inside cloned namespace)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Arguments passed to the cloned child — allocated on the heap. */
typedef struct {
    char  rootfs[512];
    char  cmd[512];
    int   sync_pipe[2];   /* parent closes write end to signal child */
    int   out_pipe_w;     /* write end of stdout/stderr capture pipe */
    int   nice_val;
} ChildArgs;

/* Entry point executed by clone() in the new namespaces. */
static int container_main(void *arg)
{
    ChildArgs *a = (ChildArgs *)arg;

    /* Redirect stdout and stderr into the supervisor's capture pipe.
     * We do this inside the child namespace, which is safe. */
    dup2(a->out_pipe_w, STDOUT_FILENO);
    dup2(a->out_pipe_w, STDERR_FILENO);
    /* Close the original write end FD (already duplicated above) */
    close(a->out_pipe_w);

    /* Wait for parent to finish setting up before proceeding.
     * Parent closes its write end; child reads EOF as "go" signal. */
    close(a->sync_pipe[1]);
    char ch;
    read(a->sync_pipe[0], &ch, 1);
    close(a->sync_pipe[0]);

    /* Apply nice value if requested */
    if (a->nice_val != 0)
        nice(a->nice_val);

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
    char *argv[] = { a->cmd, NULL };
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
 *  Section 6: Start a container (called by supervisor)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * do_start() — spawn a container.
 * Returns 0 on success.  On error writes message into errbuf and returns -1.
 */
static int do_start(const char *id, const char *rootfs, const char *cmd,
                    unsigned long soft_mib, unsigned long hard_mib, int nice_val,
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

    /*
     * Bug fix #3: use pipe2 with O_CLOEXEC so that after the child's exec,
     * any grandchildren do NOT inherit the write end.  We clear O_CLOEXEC on
     * the write end only before clone() so the direct child process inherits it.
     */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        snprintf(errbuf, errlen, "pipe2: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    /* Clear O_CLOEXEC on write end so the cloned child inherits it */
    fcntl(pipefd[1], F_SETFD, 0);

    /* Sync pipe: parent unblocks child after table update */
    int sync_pipe[2];
    if (pipe(sync_pipe) < 0) {
        close(pipefd[0]); close(pipefd[1]);
        snprintf(errbuf, errlen, "sync pipe: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /*
     * Bug fix #2: allocate ChildArgs on the HEAP so it outlives this stack
     * frame.  The parent frees it after clone() returns.  The child has its
     * own copy of the address space (clone without CLONE_VM acts like fork).
     */
    ChildArgs *ca = calloc(1, sizeof(ChildArgs));
    if (!ca) {
        close(pipefd[0]); close(pipefd[1]);
        close(sync_pipe[0]); close(sync_pipe[1]);
        snprintf(errbuf, errlen, "calloc ChildArgs: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    strncpy(ca->rootfs, rootfs, sizeof(ca->rootfs) - 1);
    strncpy(ca->cmd,    cmd,    sizeof(ca->cmd)    - 1);
    ca->sync_pipe[0] = sync_pipe[0];
    ca->sync_pipe[1] = sync_pipe[1];
    ca->out_pipe_w   = pipefd[1];
    ca->nice_val     = nice_val;

    /* Allocate a 1 MiB stack for the cloned child */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(ca);
        close(pipefd[0]); close(pipefd[1]);
        close(sync_pipe[0]); close(sync_pipe[1]);
        snprintf(errbuf, errlen, "malloc stack: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }
    char *stack_top = stack + STACK_SIZE;   /* stack grows downward */

    /* Clone with new namespaces */
    int flags = CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
                SIGCHLD;
    pid_t pid = clone(container_main, stack_top, flags, ca);

    /* Parent can free its heap copy of ChildArgs and the stack immediately.
     * clone() without CLONE_VM duplicates the address space (like fork), so
     * the child has its own copy of ca.  The parent's pointer is now stale
     * but safe to free — child is independent. */
    free(stack);
    free(ca);

    /* Close write end of the capture pipe in the parent */
    close(pipefd[1]);
    /* Close child's end of sync pipe in the parent */
    close(sync_pipe[0]);

    if (pid < 0) {
        close(pipefd[0]);
        close(sync_pipe[1]);
        snprintf(errbuf, errlen, "clone: %s", strerror(errno));
        pthread_mutex_unlock(&table_lock);
        return -1;
    }

    /* Populate container entry */
    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id) - 1);
    c->state          = STATE_RUNNING;
    c->pid            = pid;
    c->pipe_rd        = pipefd[0];
    c->started_at     = time(NULL);
    c->soft_limit_mib = soft_mib;
    c->hard_limit_mib = hard_mib;
    c->nice_val       = nice_val;
    c->stop_requested = 0;
    c->term_reason    = TERM_NONE;

    /* Build log file path and create log dir if needed */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, id);

    logbuf_init(&c->logbuf);

    /* Start producer/consumer threads */
    pthread_create(&c->producer_tid, NULL, producer_thread, c);
    pthread_create(&c->consumer_tid, NULL, consumer_thread, c);

    /* Register PID with kernel module (if loaded) */
    if (monitor_fd >= 0) {
        struct monitor_request req = {
            .pid           = pid,
            .soft_limit_kb = soft_mib * 1024UL,
            .hard_limit_kb = hard_mib * 1024UL,
        };
        if (ioctl(monitor_fd, MONITOR_IOC_REGISTER, &req) < 0)
            perror("ioctl MONITOR_IOC_REGISTER");
    }

    pthread_mutex_unlock(&table_lock);

    /* Unblock child (close our write end of the sync pipe) */
    close(sync_pipe[1]);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 7: Stop a container
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

    /*
     * Task 4 attribution requirement:
     * Set stop_requested BEFORE sending any signal so that reap_children()
     * classifies this termination as TERM_STOPPED, not TERM_HARD_LIMIT.
     */
    c->stop_requested = 1;

    pid_t pid = c->pid;
    pthread_mutex_unlock(&table_lock);

    /* Send SIGTERM, wait briefly, then SIGKILL if still alive */
    kill(pid, SIGTERM);
    usleep(500000);   /* 500 ms grace period */
    kill(pid, SIGKILL);

    /* Unregister from kernel module */
    if (monitor_fd >= 0) {
        struct monitor_request req = { .pid = pid };
        ioctl(monitor_fd, MONITOR_IOC_UNREGISTER, &req);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 8: List containers (ps)
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *term_reason_str(TermReason r)
{
    switch (r) {
    case TERM_NONE:       return "-";
    case TERM_NORMAL:     return "exited";
    case TERM_STOPPED:    return "stopped";
    case TERM_HARD_LIMIT: return "hard-limit-killed";
    default:              return "?";
    }
}

static void do_ps(int client_fd)
{
    char buf[CMD_BUF_SIZE * 4];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-16s %-8s %-10s %-20s %-8s %-8s %-18s\n",
                    "ID", "PID", "STATE", "STARTED",
                    "SOFT(M)", "HARD(M)", "TERM-REASON");
    off += snprintf(buf + off, sizeof(buf) - off,
                    "%-16s %-8s %-10s %-20s %-8s %-8s %-18s\n",
                    "----------------", "--------",
                    "----------", "--------------------",
                    "--------", "--------", "------------------");

    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == STATE_FREE) continue;
        const char *state_str =
            containers[i].state == STATE_RUNNING ? "running" :
            containers[i].state == STATE_STOPPED ? "stopped" : "?";

        char tsbuf[32];
        struct tm *tm = localtime(&containers[i].started_at);
        strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm);

        char soft_str[16], hard_str[16];
        snprintf(soft_str, sizeof(soft_str), "%lu", containers[i].soft_limit_mib);
        snprintf(hard_str, sizeof(hard_str), "%lu", containers[i].hard_limit_mib);

        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8d %-10s %-20s %-8s %-8s %-18s\n",
                        containers[i].id,
                        containers[i].pid,
                        state_str,
                        tsbuf,
                        soft_str,
                        hard_str,
                        term_reason_str(containers[i].term_reason));
    }
    pthread_mutex_unlock(&table_lock);

    write(client_fd, buf, strlen(buf));
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 9: Tail logs for a container
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
 *  Section 10: RUN command — wait for container exit
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * do_run() — start a container and block on the client connection until it
 * exits.  Returns the container's exit status to the client.
 *
 * Protocol: supervisor keeps the connection open, polling the container state
 * every 200ms.  When the container stops, it sends the final status and closes.
 */
static void do_run(const char *id, const char *rootfs, const char *cmd,
                   unsigned long soft_mib, unsigned long hard_mib, int nice_val,
                   int client_fd)
{
    char errbuf[256] = {0};

    if (do_start(id, rootfs, cmd, soft_mib, hard_mib, nice_val,
                 errbuf, sizeof(errbuf)) < 0) {
        char msg[320];
        snprintf(msg, sizeof(msg), "ERROR: %s\n", errbuf);
        write(client_fd, msg, strlen(msg));
        return;
    }

    {
        char msg[128];
        snprintf(msg, sizeof(msg), "OK: container '%s' started (blocking)\n", id);
        write(client_fd, msg, strlen(msg));
    }

    /* Poll until the container exits.  We use a short sleep so that the
     * supervisor's accept loop is not completely blocked — in a production
     * system you would use a dedicated "waiter" thread. */
    while (1) {
        usleep(200000);   /* 200 ms */

        pthread_mutex_lock(&table_lock);
        Container *c = find_by_id(id);
        if (!c || c->state != STATE_RUNNING) {
            int exit_st  = c ? c->exit_status : -1;
            TermReason tr = c ? c->term_reason : TERM_NONE;
            pthread_mutex_unlock(&table_lock);

            char msg[256];
            if (exit_st >= 0 && WIFEXITED(exit_st)) {
                snprintf(msg, sizeof(msg),
                         "EXITED: exit_code=%d reason=%s\n",
                         WEXITSTATUS(exit_st), term_reason_str(tr));
            } else if (exit_st >= 0 && WIFSIGNALED(exit_st)) {
                snprintf(msg, sizeof(msg),
                         "EXITED: signal=%d reason=%s\n",
                         WTERMSIG(exit_st), term_reason_str(tr));
            } else {
                snprintf(msg, sizeof(msg), "EXITED: reason=%s\n",
                         term_reason_str(tr));
            }
            write(client_fd, msg, strlen(msg));
            return;
        }
        pthread_mutex_unlock(&table_lock);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 11: Parse optional flags from a token stream
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * parse_flags() — scan remaining strtok tokens for --soft-mib, --hard-mib,
 * --nice.  Sets the corresponding output pointers.
 */
static void parse_flags(unsigned long *soft_mib, unsigned long *hard_mib, int *nice_val)
{
    *soft_mib = DEFAULT_SOFT_MIB;
    *hard_mib = DEFAULT_HARD_MIB;
    *nice_val = 0;

    char *tok;
    while ((tok = strtok(NULL, " ")) != NULL) {
        if (strcmp(tok, "--soft-mib") == 0) {
            char *val = strtok(NULL, " ");
            if (val) *soft_mib = (unsigned long)atol(val);
        } else if (strcmp(tok, "--hard-mib") == 0) {
            char *val = strtok(NULL, " ");
            if (val) *hard_mib = (unsigned long)atol(val);
        } else if (strcmp(tok, "--nice") == 0) {
            char *val = strtok(NULL, " ");
            if (val) *nice_val = atoi(val);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 12: Supervisor client handler
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Protocol (line-based, newline-terminated):
 *   client → "START <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
 *   client → "RUN   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
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

    } else if (strcmp(tok, "START") == 0) {
        char *id     = strtok(NULL, " ");
        char *rootfs = strtok(NULL, " ");
        char *cmd    = strtok(NULL, " ");
        if (!id || !rootfs || !cmd) {
            write(cfd, "ERROR: usage: START <id> <rootfs> <cmd> [flags]\n", 48);
        } else {
            unsigned long soft_mib, hard_mib;
            int nice_val;
            parse_flags(&soft_mib, &hard_mib, &nice_val);

            char errbuf[256] = {0};
            if (do_start(id, rootfs, cmd, soft_mib, hard_mib, nice_val,
                         errbuf, sizeof(errbuf)) < 0) {
                char msg[320];
                snprintf(msg, sizeof(msg), "ERROR: %s\n", errbuf);
                write(cfd, msg, strlen(msg));
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "OK: container '%s' started\n", id);
                write(cfd, msg, strlen(msg));
            }
        }

    } else if (strcmp(tok, "RUN") == 0) {
        char *id     = strtok(NULL, " ");
        char *rootfs = strtok(NULL, " ");
        char *cmd    = strtok(NULL, " ");
        if (!id || !rootfs || !cmd) {
            write(cfd, "ERROR: usage: RUN <id> <rootfs> <cmd> [flags]\n", 46);
        } else {
            unsigned long soft_mib, hard_mib;
            int nice_val;
            parse_flags(&soft_mib, &hard_mib, &nice_val);
            /* do_run keeps the connection open until container exits */
            do_run(id, rootfs, cmd, soft_mib, hard_mib, nice_val, cfd);
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

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 13: Supervisor main loop (with self-pipe select)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void run_supervisor(const char *rootfs)
{
    (void)rootfs;   /* supervisor rootfs is informational; containers set their own */

    /*
     * Bug fix #1: Set up self-pipe BEFORE installing SIGCHLD handler.
     * The handler writes to g_sigpipe_w; we select() on g_sigpipe_r
     * in the accept loop and call reap_children() from safe context.
     */
    int sp[2];
    if (pipe2(sp, O_CLOEXEC | O_NONBLOCK) < 0) {
        perror("pipe2 self-pipe");
        exit(1);
    }
    g_sigpipe_r = sp[0];
    g_sigpipe_w = sp[1];

    /* Install SIGCHLD handler — only writes to pipe, never locks */
    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    /* Try to open the kernel monitor device (optional) */
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] kernel monitor unavailable (%s); continuing without it\n",
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

    /* Accept loop using select() so we can also watch the self-pipe */
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv,          &rfds);
        FD_SET(g_sigpipe_r,  &rfds);
        int nfds = (srv > g_sigpipe_r ? srv : g_sigpipe_r) + 1;

        int ret = select(nfds, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* Self-pipe readable → child(ren) exited */
        if (FD_ISSET(g_sigpipe_r, &rfds)) {
            char drain[64];
            /* Drain all bytes (there may be multiple SIGCHLDs queued) */
            while (read(g_sigpipe_r, drain, sizeof(drain)) > 0)
                ;
            reap_children();
        }

        /* Incoming client connection */
        if (FD_ISSET(srv, &rfds)) {
            int cfd = accept(srv, NULL, NULL);
            if (cfd < 0) {
                if (errno == EINTR) continue;
                perror("accept");
                break;
            }
            handle_client(cfd);
        }
    }

    close(srv);
    unlink(SOCKET_PATH);
    if (monitor_fd >= 0) close(monitor_fd);
    close(g_sigpipe_r);
    close(g_sigpipe_w);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 14: CLI — connect to supervisor and send a command
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

/*
 * send_run_command() — like send_command but keeps the connection open until
 * the supervisor sends the EXITED line.  Also installs SIGINT/SIGTERM handlers
 * that forward a STOP request to the supervisor (spec requirement for 'run').
 */
static volatile sig_atomic_t g_run_interrupted = 0;
static char g_run_id[64] = {0};

static void run_sighandler(int sig)
{
    (void)sig;
    g_run_interrupted = 1;
}

static void send_run_command(const char *cmd, const char *id)
{
    strncpy(g_run_id, id, sizeof(g_run_id) - 1);

    struct sigaction sa = {0};
    sa.sa_handler = run_sighandler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n",
                SOCKET_PATH, strerror(errno));
        close(fd);
        exit(1);
    }

    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);

    char buf[CMD_BUF_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';

        /* Check for SIGINT/SIGTERM — forward STOP to supervisor */
        if (g_run_interrupted) {
            g_run_interrupted = 0;
            fprintf(stderr, "\n[run] interrupted — stopping container '%s'\n", g_run_id);
            /* Send stop via a new socket connection */
            char stopcmd[128];
            snprintf(stopcmd, sizeof(stopcmd), "STOP %s", g_run_id);
            send_command(stopcmd);
        }

        char *sentinel = strstr(buf, "\n.\n");
        if (sentinel) { *sentinel = '\0'; }
        printf("%s", buf);
        if (sentinel || strncmp(buf, "EXITED:", 7) == 0) break;
    }
    printf("\n");
    close(fd);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Section 15: main() — dispatch on argv[1]
 * ═══════════════════════════════════════════════════════════════════════════ */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <rootfs>                               Run the supervisor\n"
        "  %s start <id> <rootfs> <cmd> [--soft-mib N]\n"
        "                               [--hard-mib N] [--nice N]  Start (background)\n"
        "  %s run   <id> <rootfs> <cmd> [--soft-mib N]\n"
        "                               [--hard-mib N] [--nice N]  Start (foreground)\n"
        "  %s ps                                                List containers\n"
        "  %s stop  <id>                                        Stop a container\n"
        "  %s logs  <id>                                        Print container logs\n",
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

    /* ── start ────────────────────────────────────────────────────── */
    if (strcmp(subcmd, "start") == 0) {
        if (argc < 5) usage(argv[0]);
        /* Build command string including optional flags */
        char cmd[CMD_BUF_SIZE];
        int off = snprintf(cmd, sizeof(cmd), "START %s %s %s",
                           argv[2], argv[3], argv[4]);
        for (int i = 5; i < argc && off < (int)sizeof(cmd) - 2; i++)
            off += snprintf(cmd + off, sizeof(cmd) - off, " %s", argv[i]);
        send_command(cmd);
        return 0;
    }

    /* ── run (foreground — blocks until container exits) ─────────── */
    if (strcmp(subcmd, "run") == 0) {
        if (argc < 5) usage(argv[0]);
        char cmd[CMD_BUF_SIZE];
        int off = snprintf(cmd, sizeof(cmd), "RUN %s %s %s",
                           argv[2], argv[3], argv[4]);
        for (int i = 5; i < argc && off < (int)sizeof(cmd) - 2; i++)
            off += snprintf(cmd + off, sizeof(cmd) - off, " %s", argv[i]);
        send_run_command(cmd, argv[2]);
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
