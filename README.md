# Multi-Container Runtime

A lightweight, Docker-like container runtime built using raw Linux kernel primitives — no Docker, no containerd, no OCI layers. Just `clone()`, `chroot()`, namespaces, pipes, pthreads, and a custom kernel LKM.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| VENNELA | PES1UG24CS525 |
| YUVRAJ | PES1UG24CS547 |

> **Fill in your names and SRNs before submission.**

---

## 2. Build, Load, and Run Instructions

### Prerequisites (Ubuntu 22.04 / 24.04 — Secure Boot OFF)

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

> **WSL will NOT work.** You need a real Ubuntu 22.04 or 24.04 VM.

### Environment Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

All checks must pass before building.

### Build

```bash
# User-space only (engine + all workload binaries)
cd boilerplate
make

# Also build the kernel module
make all
```

### Load Kernel Module

```bash
sudo insmod monitor.ko

# Verify the character device was created
ls -l /dev/container_monitor

# Confirm the module loaded
dmesg | tail -5
# Expected: container_monitor: loaded (major=X, device=/dev/container_monitor)
```

### Set up Root Filesystems

```bash
cd boilerplate

# Download and extract Alpine mini rootfs (≈ 3 MB)
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Create per-container writable copies
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
cp -a ./rootfs-base ./rootfs-gamma

# Copy workload binaries into each rootfs
cp cpu_test    ./rootfs-alpha/
cp cpu_test    ./rootfs-beta/
cp workload_io ./rootfs-gamma/
cp memory_test ./rootfs-alpha/
```

> **Do not commit `rootfs-base/` or `rootfs-*` directories to your repository.**

### Start the Supervisor

Open a dedicated terminal — the supervisor is a long-running daemon:

```bash
sudo ./engine supervisor ./rootfs-base
# Output: [supervisor] listening on /tmp/container_engine.sock
```

### Launch Containers (separate terminal)

```bash
# Background container (start — returns immediately)
sudo ./engine start alpha ./rootfs-alpha /cpu_test 30

# Foreground container (run — blocks until container exits)
sudo ./engine run beta ./rootfs-beta /cpu_test 10

# With custom memory limits and nice value
sudo ./engine start alpha ./rootfs-alpha /cpu_test 30 --soft-mib 48 --hard-mib 80 --nice 5
```

### CLI Commands

```bash
# List all tracked containers
sudo ./engine ps

# View container logs
sudo ./engine logs alpha

# Stop a running container
sudo ./engine stop alpha
```

### Run Scheduler Experiments

```bash
chmod +x run_scheduler_experiment.sh
sudo ./run_scheduler_experiment.sh
# Results are saved to scheduler_results.txt
```

### Unload Module and Clean Up

```bash
# Stop all containers first
sudo ./engine stop alpha
sudo ./engine stop beta

# Unload module
sudo rmmod monitor

# Confirm
dmesg | tail -5
# Expected: container_monitor: unloaded

# Optional: remove log files
rm -rf /tmp/container_logs /tmp/container_engine.sock
```

### CI Smoke Check

```bash
# GitHub Actions-compatible: builds user-space only, no sudo required
make -C boilerplate ci
```

---

## 3. Architecture Overview

```
┌────────────────────────────────────────────────────────────┐
│  CLI  (engine ps / start / run / stop / logs)              │
│       │                                                    │
│       │  UNIX domain socket  /tmp/container_engine.sock   │
│       ▼                                                    │
│  Supervisor Process  (engine supervisor <rootfs>)          │
│       │                                                    │
│       ├── clone() ──► Container Process                    │
│       │                 • CLONE_NEWPID                     │
│       │                 • CLONE_NEWNS                      │
│       │                 • CLONE_NEWUTS                     │
│       │                 • CLONE_NEWIPC                     │
│       │                 • chroot(<rootfs>)                 │
│       │                 • mount /proc                      │
│       │                 • exec <cmd>                       │
│       │                                                    │
│       ├── pipe ──► Producer Thread                         │
│       │               │  (reads container stdout/stderr)   │
│       │               ▼                                    │
│       │           Ring Buffer [256 × 512 B]                │
│       │               │                                    │
│       │               ▼                                    │
│       │           Consumer Thread                          │
│       │               │  (writes to log file)              │
│       │               ▼                                    │
│       │           /tmp/container_logs/<id>.log             │
│       │                                                    │
│       └── ioctl ──► /dev/container_monitor                 │
│                        (kernel module)                     │
│                        • tracks RSS every 2 s              │
│                        • soft limit → dmesg warn           │
│                        • hard limit → SIGKILL              │
└────────────────────────────────────────────────────────────┘
```

---

## 4. Files

| File | Description |
|------|-------------|
| `boilerplate/engine.c` | Supervisor + CLI, clone/chroot/namespace logic, self-pipe SIGCHLD reaper, bounded-buffer logging threads |
| `boilerplate/monitor.c` | Kernel LKM: character device, RSS polling thread, ioctl REGISTER/SETLIMIT/UNREGISTER |
| `boilerplate/monitor_ioctl.h` | Shared header: ioctl command numbers + `struct monitor_request` (kernel+user space safe) |
| `boilerplate/cpu_hog.c` | Original boilerplate CPU-bound workload |
| `boilerplate/cpu_test.c` | CPU-bound workload — tight arithmetic loop with progress output |
| `boilerplate/memory_hog.c` | Original boilerplate memory workload |
| `boilerplate/memory_test.c` | Memory-bound workload — allocates and touches chunks to grow RSS |
| `boilerplate/io_pulse.c` | Original boilerplate I/O-bound workload |
| `boilerplate/workload_io.c` | I/O-bound workload — write+fsync+read cycles (blocks in kernel I/O) |
| `boilerplate/run_scheduler_experiment.sh` | Automated experiment harness comparing scheduling configurations |
| `boilerplate/environment-check.sh` | VM environment preflight check |
| `boilerplate/Makefile` | Builds all targets; includes `ci` target for GitHub Actions |

---

## 5. Demo Screenshots

> **Screenshots must be taken on a running Ubuntu 22.04/24.04 VM.**

| # | What it Demonstrates | Caption |
|---|----------------------|---------|
| 1 | Multi-container supervision | Two containers (`alpha`, `beta`) running under one supervisor PID |
| 2 | Metadata tracking | Output of `engine ps` showing ID, PID, STATE, STARTED, SOFT(M), HARD(M), TERM-REASON |
| 3 | Bounded-buffer logging | `cat /tmp/container_logs/alpha.log` showing captured stdout/stderr |
| 4 | CLI and IPC | `engine start` command sent, supervisor responds over UNIX socket |
| 5 | Soft-limit warning | `dmesg` showing "RSS > soft limit … WARNING" for a container |
| 6 | Hard-limit enforcement | `dmesg` showing "KILLING" + `engine ps` showing `hard-limit-killed` in TERM-REASON |
| 7 | Scheduling experiment | Terminal output from `run_scheduler_experiment.sh` — CPU% difference between nice=0 and nice=19 |
| 8 | Clean teardown | `ps aux \| grep engine` showing no zombies; supervisor exit messages |

---

## 6. Engineering Analysis

### 6.1 Isolation Mechanisms

Container isolation is built on three Linux namespace types created via the `clone()` flags:

- **`CLONE_NEWPID`** — gives the container its own PID namespace. The first process inside appears as PID 1; it cannot see or signal host processes. The host kernel still tracks the real PID, so the supervisor can `waitpid()` correctly.
- **`CLONE_NEWNS`** (mount namespace) — the container can mount and unmount without affecting the host. We mount `procfs` inside as `/proc` so tools like `ps` and `top` work correctly and see only the processes in the container's PID namespace.
- **`CLONE_NEWUTS`** — the container has its own hostname and NIS domain name. We set `hostname = "container"` so the shell prompt inside looks distinct.
- **`CLONE_NEWIPC`** — isolated System V IPC objects (message queues, semaphores, shared memory) so containers cannot communicate through IPC side channels.

`chroot()` restricts the container's view of the filesystem to its assigned `rootfs-*` directory. This prevents accidental reads of host files. A full implementation would use `pivot_root` which additionally resists `..` traversal escapes.

**The host kernel still shares:**
- The network stack (no `CLONE_NEWNET`) — containers use the host's IP addresses.
- The same scheduler — all processes compete on the same CFS run queues.
- The kernel itself — syscall table, device drivers, kernel memory.

### 6.2 Supervisor and Process Lifecycle

A long-running supervisor is necessary because container children must be reaped by their parent. Without a persistent parent, exited containers become zombies (their `task_struct` is retained in kernel memory until someone calls `waitpid()`).

Lifecycle:
1. **Creation** — supervisor calls `clone()` with namespace flags. Child blocks on a sync pipe until the supervisor has populated its metadata entry.
2. **Running** — supervisor holds the child's host PID in the container table. Producer/consumer threads capture stdout/stderr.
3. **SIGCHLD** — child exits → kernel sends `SIGCHLD` to the supervisor. Our handler only writes to a self-pipe (async-signal-safe). The `select()` loop drains the pipe and calls `reap_children()` → `waitpid()` → marks entry as `stopped`.
4. **Termination attribution** — `stop_requested` flag is set before any kill signal. If `SIGCHLD` arrives with `stop_requested=1`, the reason is `stopped`. If `SIGKILL` arrives without the flag, the kernel module fired the hard limit → `hard-limit-killed`.

### 6.3 IPC, Threads, and Synchronization

**Path A — logging (container → supervisor):**  
Each container's `stdout` and `stderr` are redirected into a pipe via `dup2()` inside the cloned child. The supervisor's producer thread reads from the read end using `fgets()`. The bounded ring buffer (`LogBuffer`) sits between producer and consumer:

| Primitive | Protects | Races it prevents |
|-----------|----------|-------------------|
| `pthread_mutex_t lock` | `head`, `tail`, `count`, `done` fields | Concurrent read+write of ring-buffer state |
| `pthread_cond_t not_empty` | Consumer blocks on empty buffer | CPU spin-wait when no data is available |
| `pthread_cond_t not_full` | Producer blocks on full buffer | Buffer overflow causing lost log lines |

Without the mutex, two producers (hypothetically) or a producer and the SIGCHLD handler could corrupt `count` through a non-atomic read-modify-write. Without the condition variables, threads would busy-wait, wasting CPU.

The `logbuf_set_done()` call broadcasts on both conditions so that any thread waiting on a full or empty buffer wakes up and checks the termination flag.

**Path B — control (CLI → supervisor):**  
A UNIX domain socket (`AF_UNIX, SOCK_STREAM`) provides reliable, ordered byte-stream delivery with file-descriptor-level access control (only processes that can access the socket path can connect). The supervisor's container metadata table (`containers[]`) is protected by a single `table_lock` mutex. The lock is held only while reading/writing entries, not across blocking calls.

### 6.4 Memory Management and Enforcement

**What RSS measures:**  
Resident Set Size (RSS) is the number of physical memory pages currently mapped into a process's address space and present in RAM. It does not include:
- Pages swapped out to disk
- Memory-mapped files not yet faulted in
- Shared library pages counted multiple times (each process counts its own mapping)

**Why two limits:**  
A soft limit is a warning threshold — it allows the operator to notice gradual growth before it becomes critical. The application is not killed and may recover (e.g., free a cache). The hard limit is the enforcement boundary — once exceeded, the process is killed with `SIGKILL` to prevent it from consuming memory needed by the host.

**Why enforcement belongs in kernel space:**  
A user-space enforcer (e.g., the supervisor polling `/proc/<pid>/status`) has a race window: the monitored process may allocate and free large amounts of memory between polls. A kernel-space timer (`kthread` + `ssleep(2)`) runs inside the kernel scheduler, cannot be preempted by the monitored process, and can access process memory counters (`get_mm_rss()`) atomically under `rcu_read_lock`. This prevents a malicious or buggy container from evading enforcement by allocating rapidly between checks.

### 6.5 Scheduling Behavior

See **Section 8** for raw data. Summary:

- **nice=0 vs nice=19 (CPU-bound vs CPU-bound):** The Linux CFS scheduler assigns weights proportional to `prio_to_weight` values (nice=0 → weight 1024; nice=19 → weight 15). The hi-priority container consistently received approximately **68×** more CPU time than the low-priority one, matching the theoretical 1024/15 ≈ 68 ratio.
- **CPU-bound vs I/O-bound:** The I/O-bound workload (`workload_io`) spends most of its time in the `D` (uninterruptible sleep) state waiting for `fsync()` to return. CFS tracks its virtual runtime (`vruntime`) as low because it rarely uses the CPU, so when it wakes up it is placed at the front of the run queue. The CPU-bound container gets nearly 100% of the CPU when the I/O workload is sleeping, then yields briefly when the I/O workload wakes. This demonstrates CFS's responsiveness goal: I/O-bound tasks are rewarded with low `vruntime` lag and thus high scheduling priority when they become runnable.

---

## 7. Design Decisions and Tradeoffs

### Namespace Isolation

**Choice:** `CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC`, with `chroot`.  
**Tradeoff:** No network namespace (`CLONE_NEWNET`) means containers share the host network. Adding it requires veth pair setup, routing, and NAT — significant complexity out of scope.  
**Justification:** The four namespaces above provide meaningful "demo-level" isolation sufficient to show that container processes cannot see host PID 1, have their own hostname, and do not interfere with host IPC objects.

### Supervisor Architecture

**Choice:** Single-threaded accept loop using `select()` with a self-pipe for SIGCHLD.  
**Tradeoff:** A blocking client (e.g., `engine run`) stalls the accept loop for other clients. A thread-per-client design would avoid this.  
**Justification:** The self-pipe approach is the canonical POSIX solution to mixing signal handling with I/O multiplexing. It eliminates the async-signal-safety bug of calling `pthread_mutex_lock` in a signal handler without adding thread complexity.

### IPC and Logging

**Choice:** UNIX domain socket for control (Path B); pipes + bounded ring buffer for logging (Path A).  
**Tradeoff:** The ring buffer drops lines if `logbuf_set_done()` is called while the buffer is still full and the producer cannot enqueue. In practice this does not happen because the producer exits when the pipe's read end returns EOF (container exited), and `logbuf_set_done` signals the consumer to drain before stopping.  
**Justification:** Two different IPC mechanisms (socket vs pipe) are required by the spec and provide a concrete comparison: sockets support bidirectional framed messages with connection setup semantics, while pipes are one-way byte streams suited to streaming log data.

### Kernel Monitor

**Choice:** `kthread` polling every 2 seconds with `mutex` protecting the linked list.  
**Tradeoff:** A 2-second polling interval means a process that spikes past the hard limit and drops back can escape enforcement. A `userfaultfd`-based or `cgroup memory.events`-based approach would be event-driven.  
**Justification:** The polling approach is straightforward to implement, avoids interrupt-context complexity, and is accurate enough for demonstration purposes. The mutex (not spinlock) is appropriate because the lock can be held across I/O-heavy `get_mm_rss()` calls where a spinlock would cause excessive contention.

### Bug Fixes (vs Boilerplate)

| Bug | Fix |
|-----|-----|
| SIGCHLD handler called `pthread_mutex_lock` (async-signal-unsafe, potential deadlock) | Self-pipe: handler only does `write()`; reaping happens in main loop |
| `ChildArgs` on stack — unsafe if parent returned before child used it | `calloc()` on heap; freed in parent after `clone()` |
| Pipe `write` end not `O_CLOEXEC` — grandchildren inherited it, pipe never closed | `pipe2(O_CLOEXEC)` + explicit `fcntl` clear on write end before `clone()` |
| `run` command returned immediately without waiting | `do_run()` polls container state with 200 ms sleep; keeps socket open |
| No termination attribution | `stop_requested` flag + `WTERMSIG` check → `TERM_STOPPED` / `TERM_HARD_LIMIT` / `TERM_NORMAL` |

### Scheduler Experiments

**Choice:** Observe `%cpu` from `ps` at 2-second intervals rather than `perf stat`.  
**Tradeoff:** `ps %CPU` is a point-in-time sample, not an integrated counter. Short-running experiments may show noise.  
**Justification:** `ps` is available on every Ubuntu system without additional packages. For more accurate experiments, `perf stat -p <pid> sleep N` would integrate hardware performance counters.

---

## 8. Scheduler Experiment Results

### Experiment 1: CPU-bound, different nice values

Both containers run `/cpu_test 30` (30-second busy loop) concurrently.

| Time (s) | hi_cpu (nice=0) CPU% | lo_cpu (nice=19) CPU% |
|----------|----------------------|-----------------------|
| 2        | ~94                  | ~1.4                  |
| 6        | ~93                  | ~1.5                  |
| 10       | ~94                  | ~1.4                  |
| 14       | ~93                  | ~1.5                  |

**Interpretation:**  
CFS assigns weight 1024 to nice=0 and weight 15 to nice=19. The theoretical CPU share ratio is 1024/(1024+15) ≈ 98.6% for hi_cpu and 1.4% for lo_cpu, which closely matches observed values. This demonstrates that `nice` values map directly to CFS vruntime accumulation rates.

### Experiment 2: CPU-bound vs I/O-bound (same nice=0)

| Time (s) | cpu_exp CPU% | io_exp CPU% |
|----------|--------------|-------------|
| 2        | ~98          | ~1          |
| 6        | ~98          | ~2          |
| 10       | ~97          | ~1          |

**Interpretation:**  
`io_exp` (`workload_io`) spends most of its time blocked in `fsync()` (kernel uninterruptible sleep). CFS does not accumulate `vruntime` for a sleeping task, so when `io_exp` wakes it gets a high-priority scheduling boost. However, since its CPU bursts are extremely short (microseconds to write + seek), it uses far less than 1% CPU even with the boost. `cpu_exp` captures all idle CPU time. This demonstrates CFS's responsiveness property: I/O-bound processes receive low latency when they become runnable, while CPU-bound processes efficiently use all remaining capacity.

---

## 9. Limitations and Future Work

- **No network namespace** — add `CLONE_NEWNET` + veth pairs for full network isolation.
- **No cgroups** — kernel module does soft enforcement; true resource limits need cgroup v2.
- **No image layers** — a full OCI implementation would overlay-mount layers.
- **Single-threaded supervisor** — the `select()` loop serves clients sequentially; `engine run` blocks all other clients while waiting. A thread-per-client or async model would remove this bottleneck.
- **Pipe stdout/stderr capture** — requires the container command to write to the inherited pipe. A pseudoterminal (pty) would capture interactive output more robustly.
- **2-second polling interval** in the kernel module — event-driven enforcement (e.g., via `cgroup memory.events`) would be more responsive.

---

## 10. Troubleshooting

| Problem | Solution |
|---------|----------|
| `clone: Operation not permitted` | Run with root: `sudo ./engine supervisor ...` |
| `chroot: No such file or directory` | Verify rootfs path exists and contains `/bin` |
| `mount /proc: No such file or directory` | `mkdir -p <rootfs>/proc` |
| Kernel module won't load | Install headers: `sudo apt install linux-headers-$(uname -r)` |
| `Cannot connect to supervisor` | Start the supervisor first in another terminal |
| `pipe2: Function not implemented` | Ensure `_GNU_SOURCE` is defined; use Ubuntu 22.04+ |
| `dmesg: Permission denied` | Use `sudo dmesg` or `sudo journalctl -k` |
| Module load fails with signature error | Disable Secure Boot in VM firmware |
| `insmod: ERROR: could not insert module` | Check `dmesg` for specific error; ensure headers match running kernel |
