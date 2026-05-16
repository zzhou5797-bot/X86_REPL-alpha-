// SPDX-License-Identifier: GPL-2.0-only
/*
 * asmrepl.c — kernel module: /dev/asmrepl
 *
 * Exposes CR0/CR2/CR3/CR4/CR8, DR0–DR7, and a whitelisted set of MSRs
 * to unprivileged processes via ioctl.
 *
 * MSR reads are protected by:
 *   1. A static whitelist — unknown addresses are rejected with EPERM.
 *   2. rdmsr_safe() — uses the kernel's exception fixup table so that
 *      a #GP fault does not crash the kernel (returns -EIO instead).
 *
 * Loading the module:
 *   sudo insmod asmrepl.ko
 *   ls -l /dev/asmrepl   # created by udev or manually by the Makefile
 *
 * Note: Reading CR/DR requires the caller to run on the *same* CPU that
 * executed the instruction of interest.  For a REPL this is acceptable
 * because ptrace already serializes execution.  For SMP accuracy a
 * smp_call_function_single() variant would be needed — not implemented
 * here to keep the module simple.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <asm/msr.h>           /* rdmsr_safe, native_read_cr* */
#include <asm/processor.h>

/* Include the shared ioctl header from userspace tree.
 * The kernel build system passes -I$(src)/../../include via the Makefile. */
#include "asmrepl_ioctl.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("asmrepl");
MODULE_DESCRIPTION("Expose CR/DR/MSR to userspace for assembly REPL");
MODULE_VERSION("1.0");

/* ── device bookkeeping ─────────────────────────────────────────── */

#define DEVICE_NAME "asmrepl"

static dev_t        g_devno;
static struct cdev  g_cdev;
static struct class *g_class;

/* Make /dev/asmrepl world-readable so asmrepl can open it without root */
static char *asmrepl_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;
    return NULL;
}

/* ── MSR whitelist ──────────────────────────────────────────────── */

static const u32 kMsrWhitelist[] = {
    MSR_EFER,           /* 0xC0000080 */
    MSR_STAR,           /* 0xC0000081 */
    MSR_LSTAR,          /* 0xC0000082 */
    MSR_CSTAR,          /* 0xC0000083 */
    MSR_SFMASK,         /* 0xC0000084 */
    MSR_FS_BASE,        /* 0xC0000100 */
    MSR_GS_BASE,        /* 0xC0000101 */
    MSR_KERNEL_GS,      /* 0xC0000102 */
    MSR_TSC,            /* 0x00000010 */
    MSR_APIC_BASE,      /* 0x0000001B */
    MSR_SYSENTER_CS,    /* 0x00000174 */
    MSR_SYSENTER_ESP,   /* 0x00000175 */
    MSR_SYSENTER_EIP,   /* 0x00000176 */
    0x000000FEu,        /* IA32_MTRRCAP */
};

static bool msr_is_whitelisted(u32 addr)
{
    for (size_t i = 0; i < ARRAY_SIZE(kMsrWhitelist); i++)
        if (kMsrWhitelist[i] == addr)
            return true;
    return false;
}

/* ── ioctl handlers ─────────────────────────────────────────────── */

static long do_get_cr(struct asmrepl_cr_state __user *ustate)
{
    struct asmrepl_cr_state s;

    s.cr0 = native_read_cr0();
    s.cr2 = native_read_cr2();
    s.cr3 = __native_read_cr3();
    s.cr4 = native_read_cr4();

    /* CR8 (TPR) — only meaningful on x2APIC systems.
     * native_read_cr8() was removed in Linux 6.x; use inline asm. */
#ifdef CONFIG_X86_64
    {
        unsigned long cr8;
        asm volatile("mov %%cr8, %0" : "=r"(cr8));
        s.cr8 = cr8;
    }
#else
    s.cr8 = 0;
#endif

    if (copy_to_user(ustate, &s, sizeof(s)))
        return -EFAULT;
    return 0;
}

static long do_get_dr(struct asmrepl_dr_state __user *ustate)
{
    struct asmrepl_dr_state s;

    /* get_debugreg(val, N) reads DRN via the CPU instruction */
    get_debugreg(s.dr[0], 0);
    get_debugreg(s.dr[1], 1);
    get_debugreg(s.dr[2], 2);
    get_debugreg(s.dr[3], 3);
    /* DR4/DR5 alias DR6/DR7 when CR4.DE=0; read them as DR6/DR7 */
    get_debugreg(s.dr[4], 6);   /* DR4 → same as DR6 */
    get_debugreg(s.dr[5], 7);   /* DR5 → same as DR7 */
    get_debugreg(s.dr[6], 6);
    get_debugreg(s.dr[7], 7);

    if (copy_to_user(ustate, &s, sizeof(s)))
        return -EFAULT;
    return 0;
}

static long do_get_msr(struct asmrepl_msr_req __user *ureq)
{
    struct asmrepl_msr_req req;

    if (copy_from_user(&req, ureq, sizeof(req)))
        return -EFAULT;

    if (!msr_is_whitelisted(req.msr_addr)) {
        req.error = -EPERM;
        req.value = 0;
        return copy_to_user(ureq, &req, sizeof(req)) ? -EFAULT : 0;
    }

    u32 lo, hi;
    int err = rdmsr_safe(req.msr_addr, &lo, &hi);
    if (err) {
        req.error = -EIO;   /* #GP caught by fixup table */
        req.value = 0;
    } else {
        req.error = 0;
        req.value = ((u64)hi << 32) | lo;
    }

    return copy_to_user(ureq, &req, sizeof(req)) ? -EFAULT : 0;
}

/* ── file operations ────────────────────────────────────────────── */

static long asmrepl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    void __user *uarg = (void __user *)arg;

    switch (cmd) {
    case ASMREPL_GET_CR:
        return do_get_cr(uarg);
    case ASMREPL_GET_DR:
        return do_get_dr(uarg);
    case ASMREPL_GET_MSR:
        return do_get_msr(uarg);
    default:
        return -ENOTTY;
    }
}

static int asmrepl_open(struct inode *inode, struct file *filp)
{
    return 0;   /* no per-fd state */
}

static int asmrepl_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations g_fops = {
    .owner          = THIS_MODULE,
    .open           = asmrepl_open,
    .release        = asmrepl_release,
    .unlocked_ioctl = asmrepl_ioctl,
    .compat_ioctl   = asmrepl_ioctl,
};

/* ── module init / exit ─────────────────────────────────────────── */

static int __init asmrepl_init(void)
{
    int rc;

    rc = alloc_chrdev_region(&g_devno, 0, 1, DEVICE_NAME);
    if (rc < 0) {
        pr_err("asmrepl: alloc_chrdev_region failed: %d\n", rc);
        return rc;
    }

    cdev_init(&g_cdev, &g_fops);
    g_cdev.owner = THIS_MODULE;
    rc = cdev_add(&g_cdev, g_devno, 1);
    if (rc < 0) {
        pr_err("asmrepl: cdev_add failed: %d\n", rc);
        unregister_chrdev_region(g_devno, 1);
        return rc;
    }

    /* class_create() dropped the THIS_MODULE argument in Linux 6.4 */
    g_class = class_create(DEVICE_NAME);
    if (IS_ERR(g_class)) {
        rc = PTR_ERR(g_class);
        pr_err("asmrepl: class_create failed: %d\n", rc);
        cdev_del(&g_cdev);
        unregister_chrdev_region(g_devno, 1);
        return rc;
    }
    g_class->devnode = asmrepl_devnode;

    struct device *dev = device_create(g_class, NULL, g_devno, NULL, DEVICE_NAME);
    if (IS_ERR(dev)) {
        rc = PTR_ERR(dev);
        pr_err("asmrepl: device_create failed: %d\n", rc);
        class_destroy(g_class);
        cdev_del(&g_cdev);
        unregister_chrdev_region(g_devno, 1);
        return rc;
    }

    pr_info("asmrepl: /dev/%s created (major=%d)\n",
            DEVICE_NAME, MAJOR(g_devno));
    return 0;
}

static void __exit asmrepl_exit(void)
{
    device_destroy(g_class, g_devno);
    class_destroy(g_class);
    cdev_del(&g_cdev);
    unregister_chrdev_region(g_devno, 1);
    pr_info("asmrepl: unloaded\n");
}

module_init(asmrepl_init);
module_exit(asmrepl_exit);
