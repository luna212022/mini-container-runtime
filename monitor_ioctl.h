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
