/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * asmrepl_ioctl.h — shared between kernel module and userspace
 *
 * Defines ioctl commands and data structures for /dev/asmrepl.
 * This header must be included both by kmod/asmrepl.c and hw_regs.c.
 */
#ifndef ASMREPL_IOCTL_H
#define ASMREPL_IOCTL_H

#ifdef __KERNEL__
# include <linux/types.h>
# include <linux/ioctl.h>
#else
# include <stdint.h>
# include <sys/ioctl.h>
#endif

#define ASMREPL_IOC_MAGIC   'A'

/* ── CR register snapshot ────────────────────────────────────────── */
struct asmrepl_cr_state {
    uint64_t cr0;
    uint64_t cr2;
    uint64_t cr3;
    uint64_t cr4;
    uint64_t cr8;   /* Task Priority Register (TPR shadow) */
};

/* ── Debug register snapshot ─────────────────────────────────────── */
struct asmrepl_dr_state {
    uint64_t dr[8]; /* DR0–DR7 (DR4/DR5 alias DR6/DR7 when CR4.DE=0) */
};

/*
 * ── MSR read request ─────────────────────────────────────────────
 *
 * Fill msr_addr, then issue ASMREPL_GET_MSR.
 * The kernel reads the MSR inside a fixup-table guarded region:
 *   • If the MSR is not in the whitelist → error = -EPERM
 *   • If rdmsr_safe() faults (#GP)       → error = -EIO
 *   • On success                         → error = 0, value valid
 */
struct asmrepl_msr_req {
    uint32_t msr_addr;
    int32_t  error;     /* out: 0 = ok, negative errno on failure  */
    uint64_t value;     /* out: MSR value (valid only when error=0) */
};

/* ioctl commands */
#define ASMREPL_GET_CR   _IOR ('A', 0, struct asmrepl_cr_state)
#define ASMREPL_GET_DR   _IOR ('A', 1, struct asmrepl_dr_state)
#define ASMREPL_GET_MSR  _IOWR('A', 2, struct asmrepl_msr_req)

/* ── Well-known MSR addresses ────────────────────────────────────── */
/* Guard against kernel headers that already define these */
#ifndef MSR_EFER
# define MSR_EFER       0xC0000080u
#endif
#ifndef MSR_STAR
# define MSR_STAR       0xC0000081u
#endif
#ifndef MSR_LSTAR
# define MSR_LSTAR      0xC0000082u
#endif
#ifndef MSR_CSTAR
# define MSR_CSTAR      0xC0000083u
#endif
#ifndef MSR_SFMASK
# define MSR_SFMASK     0xC0000084u
#endif
#ifndef MSR_FS_BASE
# define MSR_FS_BASE    0xC0000100u
#endif
#ifndef MSR_GS_BASE
# define MSR_GS_BASE    0xC0000101u
#endif
#ifndef MSR_KERNEL_GS
# define MSR_KERNEL_GS  0xC0000102u
#endif
#ifndef MSR_TSC
# define MSR_TSC        0x00000010u
#endif
#ifndef MSR_APIC_BASE
# define MSR_APIC_BASE  0x0000001Bu
#endif
#ifndef MSR_MTRRCAP
# define MSR_MTRRCAP    0x000000FEu
#endif
#ifndef MSR_SYSENTER_CS
# define MSR_SYSENTER_CS  0x00000174u
#endif
#ifndef MSR_SYSENTER_ESP
# define MSR_SYSENTER_ESP 0x00000175u
#endif
#ifndef MSR_SYSENTER_EIP
# define MSR_SYSENTER_EIP 0x00000176u
#endif

#endif /* ASMREPL_IOCTL_H */
