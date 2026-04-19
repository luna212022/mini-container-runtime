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
 * Kernel version: tested on Linux 5.15+ (Ubuntu 22.04) and 6.x (Ubuntu 24.04)
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
#include <linux/version.h>  /* LINUX_VERSION_CODE, KERNEL_VERSION */

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

    /* Create a device class so udev creates /dev/container_monitor.
     * class_create() API changed in Linux 6.4: THIS_MODULE arg was removed. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    dev_class = class_create(CLASS_NAME);
#else
    dev_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
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
