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
