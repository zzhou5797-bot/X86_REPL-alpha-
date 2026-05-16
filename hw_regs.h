/*
 * hw_regs.h — unified hardware register snapshot
 *
 * HwRegState aggregates all three access paths:
 *   Path 1: ptrace PTRACE_GETREGS / PTRACE_GETFPREGS  (GPR, XMM, x87, MXCSR)
 *   Path 2: ioctl /dev/asmrepl                        (CR, DR, MSR whitelist)
 *   Path 3: /dev/mem mmap                             (APIC, HPET — optional)
 */
#ifndef HW_REGS_H
#define HW_REGS_H

#include <stdint.h>
#include <sys/user.h>       /* user_regs_struct, user_fpregs_struct */
#include "include/asmrepl_ioctl.h"

/* ── Subset of interesting MSRs captured by default ─────────────── */
typedef struct {
    uint64_t efer;          /* MSR_EFER      0xC0000080 */
    uint64_t lstar;         /* MSR_LSTAR     0xC0000082 */
    uint64_t fs_base;       /* MSR_FS_BASE   0xC0000100 */
    uint64_t gs_base;       /* MSR_GS_BASE   0xC0000101 */
    uint64_t kernel_gs;     /* MSR_KERNEL_GS 0xC0000102 */
    uint64_t tsc;           /* MSR_TSC       0x00000010 */
    uint64_t apic_base;     /* MSR_APIC_BASE 0x0000001B */
} HwMsrState;

/* ── Optional MMIO device state (path 3) ────────────────────────── */
typedef struct {
    uint32_t apic_id;       /* APIC ID register (offset 0x20) */
    uint32_t apic_ver;      /* APIC Version register (offset 0x30) */
    uint32_t apic_tpr;      /* Task Priority Register (offset 0x80) */
    int      available;     /* 0 if /dev/mem not accessible */
} HwMemMappedState;

/* ── Full hardware register snapshot ────────────────────────────── */
typedef struct {
    /* Path 1a — ptrace PTRACE_GETREGS */
    struct user_regs_struct   gpr;  /* rax-r15, rip, rflags, cs/ds/es/fs/gs/ss,
                                       orig_rax, fs_base, gs_base */

    /* Path 1b — ptrace PTRACE_GETFPREGS */
    struct user_fpregs_struct fpr;  /* xmm0-15 (128b each), st(0)-st(7),
                                       mxcsr, mxcr_mask, fcw, fsw, ftw */

    /* Path 2 — ioctl /dev/asmrepl (requires kmod) */
    struct asmrepl_cr_state   cr;
    struct asmrepl_dr_state   dr;
    HwMsrState                msr;
    int                       kmod_available; /* 1 if /dev/asmrepl opened ok */

    /* Path 3 — /dev/mem mmap (optional, requires root) */
    HwMemMappedState          mmio;
} HwRegState;

/* ── Capture functions ───────────────────────────────────────────── */

/*
 * hw_regs_init — open /dev/asmrepl (path 2) and /dev/mem (path 3) if available.
 * Call once at startup.  Returns fd for /dev/asmrepl (-1 if unavailable).
 */
int  hw_regs_init(int *devmem_fd_out);

/*
 * hw_regs_capture_ptrace — fill gpr + fpr from a stopped ptrace child.
 * child_pid must be in STOPPED state (e.g., after PTRACE_CONT + waitpid).
 * Returns 0 on success, -1 on error (sets errno).
 */
int  hw_regs_capture_ptrace(HwRegState *s, pid_t child_pid);

/*
 * hw_regs_capture_kmod — fill cr, dr, msr via ioctl on asmrepl_fd.
 * If asmrepl_fd < 0, marks s->kmod_available = 0 and returns immediately.
 */
void hw_regs_capture_kmod(HwRegState *s, int asmrepl_fd);

/*
 * hw_regs_capture_mmio — fill mmio fields via /dev/mem mmap.
 * devmem_fd must be a valid fd from open("/dev/mem", O_RDONLY).
 * Silently skips if devmem_fd < 0.
 */
void hw_regs_capture_mmio(HwRegState *s, int devmem_fd);

/* hw_regs_fini — close fds opened by hw_regs_init. */
void hw_regs_fini(int asmrepl_fd, int devmem_fd);

/* ── Pretty-print helpers ────────────────────────────────────────── */

/* Print GPR + rflags + segment bases.  prev may be NULL (no highlighting). */
void hw_print_gpr(const HwRegState *s, const HwRegState *prev);

/* Print XMM registers (xmm0–xmm15) as packed uint64 pairs. */
void hw_print_xmm(const HwRegState *s, const HwRegState *prev);

/* Print x87 ST(0)–ST(7) + MXCSR + FCW/FSW. */
void hw_print_fpu(const HwRegState *s, const HwRegState *prev);

/* Print CR0/CR2/CR3/CR4/CR8 with flag decode. */
void hw_print_cr(const HwRegState *s, const HwRegState *prev);

/* Print DR0–DR7 with DR6 condition decode, DR7 enable decode. */
void hw_print_dr(const HwRegState *s, const HwRegState *prev);

/* Print MSR snapshot. */
void hw_print_msr(const HwRegState *s, const HwRegState *prev);

#endif /* HW_REGS_H */
