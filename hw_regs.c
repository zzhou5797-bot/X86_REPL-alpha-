/*
 * hw_regs.c — hardware register capture (three paths)
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/user.h>

#include "hw_regs.h"

/* ── ANSI colors ─────────────────────────────────────────────────── */
#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_RED     "\033[31m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_DIM     "\033[2m"

/* APIC base physical address (standard, except for x2APIC mode) */
#define APIC_PHYS_BASE  0xFEE00000UL
#define APIC_MMIO_SIZE  0x1000

/* ── Well-known MSRs to capture by default ───────────────────────── */
static const struct { uint32_t addr; const char *name; size_t offset; } kDefaultMsrs[] = {
#define OFF(f) offsetof(HwMsrState, f)
    { MSR_EFER,      "EFER",      OFF(efer)      },
    { MSR_LSTAR,     "LSTAR",     OFF(lstar)     },
    { MSR_FS_BASE,   "FS.BASE",   OFF(fs_base)   },
    { MSR_GS_BASE,   "GS.BASE",   OFF(gs_base)   },
    { MSR_KERNEL_GS, "KERNEL_GS", OFF(kernel_gs) },
    { MSR_TSC,       "TSC",       OFF(tsc)       },
    { MSR_APIC_BASE, "APIC_BASE", OFF(apic_base) },
#undef OFF
};

/* ════════════════════════════════════════════════════════════════════
 * Initialization / teardown
 * ════════════════════════════════════════════════════════════════════ */

int hw_regs_init(int *devmem_fd_out)
{
    int asmrepl_fd = open("/dev/asmrepl", O_RDONLY | O_CLOEXEC);
    /* Not fatal — kmod is optional */
    if (asmrepl_fd < 0 && errno != ENOENT && errno != ENXIO)
        perror("hw_regs_init: /dev/asmrepl");

    if (devmem_fd_out) {
        int dfd = open("/dev/mem", O_RDONLY | O_CLOEXEC);
        *devmem_fd_out = dfd;   /* -1 if unavailable (no root / CONFIG_STRICT_DEVMEM) */
    }

    return asmrepl_fd;
}

void hw_regs_fini(int asmrepl_fd, int devmem_fd)
{
    if (asmrepl_fd >= 0) close(asmrepl_fd);
    if (devmem_fd   >= 0) close(devmem_fd);
}

/* ════════════════════════════════════════════════════════════════════
 * Path 1 — ptrace
 * ════════════════════════════════════════════════════════════════════ */

int hw_regs_capture_ptrace(HwRegState *s, pid_t child_pid)
{
    if (ptrace(PTRACE_GETREGS, child_pid, NULL, &s->gpr) < 0) {
        perror("PTRACE_GETREGS");
        return -1;
    }
    if (ptrace(PTRACE_GETFPREGS, child_pid, NULL, &s->fpr) < 0) {
        perror("PTRACE_GETFPREGS");
        return -1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Path 2 — ioctl /dev/asmrepl
 * ════════════════════════════════════════════════════════════════════ */

void hw_regs_capture_kmod(HwRegState *s, int asmrepl_fd)
{
    if (asmrepl_fd < 0) {
        s->kmod_available = 0;
        return;
    }
    s->kmod_available = 1;

    /* CR registers */
    if (ioctl(asmrepl_fd, ASMREPL_GET_CR, &s->cr) < 0)
        perror("ioctl ASMREPL_GET_CR");

    /* Debug registers */
    if (ioctl(asmrepl_fd, ASMREPL_GET_DR, &s->dr) < 0)
        perror("ioctl ASMREPL_GET_DR");

    /* MSRs — iterate whitelist, tolerate individual failures */
    uint8_t *msr_base = (uint8_t *)&s->msr;
    for (size_t i = 0; i < sizeof(kDefaultMsrs) / sizeof(kDefaultMsrs[0]); i++) {
        struct asmrepl_msr_req req = { .msr_addr = kDefaultMsrs[i].addr };
        if (ioctl(asmrepl_fd, ASMREPL_GET_MSR, &req) < 0 || req.error != 0) {
            /* Leave the field zero; not fatal */
            continue;
        }
        uint64_t *dst = (uint64_t *)(msr_base + kDefaultMsrs[i].offset);
        *dst = req.value;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Path 3 — /dev/mem mmap (APIC)
 * ════════════════════════════════════════════════════════════════════ */

void hw_regs_capture_mmio(HwRegState *s, int devmem_fd)
{
    s->mmio.available = 0;
    if (devmem_fd < 0)
        return;

    void *apic = mmap(NULL, APIC_MMIO_SIZE, PROT_READ, MAP_SHARED,
                      devmem_fd, (off_t)APIC_PHYS_BASE);
    if (apic == MAP_FAILED)
        return; /* CONFIG_STRICT_DEVMEM or not root */

    /* APIC registers are at 4KB-spaced 32-bit offsets */
    const volatile uint32_t *regs = apic;
    s->mmio.apic_id  = regs[0x20 / 4];
    s->mmio.apic_ver = regs[0x30 / 4];
    s->mmio.apic_tpr = regs[0x80 / 4];
    s->mmio.available = 1;

    munmap(apic, APIC_MMIO_SIZE);
}

/* ════════════════════════════════════════════════════════════════════
 * Pretty-print helpers
 * ════════════════════════════════════════════════════════════════════ */

static void print_rflags(uint64_t rf)
{
    printf("  %s= 0x%016llx  [%s%s%s%s%s%s%s%s%s]%s\n",
        ANSI_DIM, (unsigned long long)rf,
        (rf >> 11) & 1 ? "OF " : "",
        (rf >> 10) & 1 ? "DF " : "",
        (rf >>  9) & 1 ? "IF " : "",
        (rf >>  8) & 1 ? "TF " : "",
        (rf >>  7) & 1 ? "SF " : "",
        (rf >>  6) & 1 ? "ZF " : "",
        (rf >>  4) & 1 ? "AF " : "",
        (rf >>  2) & 1 ? "PF " : "",
        (rf >>  0) & 1 ? "CF " : "",
        ANSI_RESET);
}

void hw_print_gpr(const HwRegState *s, const HwRegState *prev)
{
    const struct user_regs_struct *g  = &s->gpr;
    const struct user_regs_struct *gp = prev ? &prev->gpr : NULL;

#define GPR_ROW(label, cur, prev_field) do { \
    uint64_t _c = (cur), _p = (uint64_t)(gp ? (prev_field) : _c); \
    int _ch = (_c != _p); \
    printf("  %s%-7s%s 0x%016llx%s\n", \
           _ch ? ANSI_YELLOW ANSI_BOLD : "", label, \
           _ch ? ANSI_RESET : "", (unsigned long long)_c, \
           _ch ? ANSI_DIM " <--" ANSI_RESET : ""); \
} while(0)

    printf(ANSI_BOLD ANSI_CYAN "── GPR ──────────────────────────────────────────────────────\n" ANSI_RESET);
    GPR_ROW("rax",    g->rax,    gp->rax);
    GPR_ROW("rbx",    g->rbx,    gp->rbx);
    GPR_ROW("rcx",    g->rcx,    gp->rcx);
    GPR_ROW("rdx",    g->rdx,    gp->rdx);
    GPR_ROW("rsi",    g->rsi,    gp->rsi);
    GPR_ROW("rdi",    g->rdi,    gp->rdi);
    GPR_ROW("rbp",    g->rbp,    gp->rbp);
    GPR_ROW("rsp",    g->rsp,    gp->rsp);
    GPR_ROW("r8",     g->r8,     gp->r8);
    GPR_ROW("r9",     g->r9,     gp->r9);
    GPR_ROW("r10",    g->r10,    gp->r10);
    GPR_ROW("r11",    g->r11,    gp->r11);
    GPR_ROW("r12",    g->r12,    gp->r12);
    GPR_ROW("r13",    g->r13,    gp->r13);
    GPR_ROW("r14",    g->r14,    gp->r14);
    GPR_ROW("r15",    g->r15,    gp->r15);
    GPR_ROW("rip",    g->rip,    gp->rip);

    printf("  %-7s", "rflags");
    print_rflags(g->eflags);

    printf("  " ANSI_DIM "fs_base=0x%llx  gs_base=0x%llx\n" ANSI_RESET,
           (unsigned long long)g->fs_base, (unsigned long long)g->gs_base);
#undef GPR_ROW
}

void hw_print_xmm(const HwRegState *s, const HwRegState *prev)
{
    printf(ANSI_BOLD ANSI_CYAN "── XMM ──────────────────────────────────────────────────────\n" ANSI_RESET);
    for (int i = 0; i < 16; i++) {
        /* xmm_space[32]: each XMM is 4 × uint32_t (128 bits) */
        const uint32_t *cur = (const uint32_t *)&s->fpr.xmm_space[i * 4];
        const uint32_t *prv = prev ? (const uint32_t *)&prev->fpr.xmm_space[i * 4] : cur;
        int changed = (prev && memcmp(cur, prv, 16) != 0);
        printf("  %sxmm%-2d%s  %08x_%08x_%08x_%08x%s\n",
               changed ? ANSI_YELLOW ANSI_BOLD : "",
               i,
               changed ? ANSI_RESET : "",
               cur[3], cur[2], cur[1], cur[0],
               changed ? ANSI_DIM " <--" ANSI_RESET : "");
    }
    printf("  " ANSI_DIM "mxcsr=0x%08x  mxcr_mask=0x%08x\n" ANSI_RESET,
           s->fpr.mxcsr, s->fpr.mxcr_mask);
}

void hw_print_fpu(const HwRegState *s, const HwRegState *prev)
{
    (void)prev; /* delta display not yet implemented for x87 */
    printf(ANSI_BOLD ANSI_CYAN "── x87 FPU ──────────────────────────────────────────────────\n" ANSI_RESET);
    /* st_space: 8 registers × 4 uint32_t = 32 entries (80-bit extended in 128b) */
    for (int i = 0; i < 8; i++) {
        const uint32_t *st = (const uint32_t *)&s->fpr.st_space[i * 4];
        /* 80-bit: [0..1] mantissa low, [2] mantissa high (16b) | exponent+sign */
        uint64_t mant = ((uint64_t)st[1] << 32) | st[0];
        uint16_t exp  = (uint16_t)st[2];
        printf("  ST(%d)   mant=0x%016llx  exp_sign=0x%04x\n",
               i, (unsigned long long)mant, exp);
    }
    printf("  fcw=0x%04x  fsw=0x%04x  ftw=0x%04x\n",
           s->fpr.cwd, s->fpr.swd, (uint16_t)s->fpr.ftw);
    printf("  mxcsr=0x%08x\n", s->fpr.mxcsr);
}

/* CR0 flag decode */
static void decode_cr0(uint64_t cr0)
{
    printf("  [%s%s%s%s%s%s%s%s]\n",
        (cr0 >> 31) & 1 ? "PG "  : "",
        (cr0 >> 30) & 1 ? "CD "  : "",
        (cr0 >> 29) & 1 ? "NW "  : "",
        (cr0 >> 16) & 1 ? "WP "  : "",
        (cr0 >>  5) & 1 ? "NE "  : "",
        (cr0 >>  4) & 1 ? "ET "  : "",
        (cr0 >>  1) & 1 ? "MP "  : "",
        (cr0 >>  0) & 1 ? "PE "  : "");
}

static void decode_cr4(uint64_t cr4)
{
    printf("  [%s%s%s%s%s%s%s%s%s%s%s]\n",
        (cr4 >> 21) & 1 ? "SMAP " : "",
        (cr4 >> 20) & 1 ? "SMEP " : "",
        (cr4 >> 18) & 1 ? "OSXSAVE " : "",
        (cr4 >> 17) & 1 ? "PCIDE " : "",
        (cr4 >> 13) & 1 ? "VME " : "",
        (cr4 >> 10) & 1 ? "OSXMMEXCPT " : "",
        (cr4 >>  9) & 1 ? "OSFXSR " : "",
        (cr4 >>  8) & 1 ? "PCE " : "",
        (cr4 >>  5) & 1 ? "PAE " : "",
        (cr4 >>  4) & 1 ? "PSE " : "",
        (cr4 >>  0) & 1 ? "VME " : "");
}

void hw_print_cr(const HwRegState *s, const HwRegState *prev)
{
    printf(ANSI_BOLD ANSI_CYAN "── Control Registers ────────────────────────────────────────\n" ANSI_RESET);
    if (!s->kmod_available) {
        printf("  " ANSI_DIM "(kmod /dev/asmrepl not loaded)" ANSI_RESET "\n");
        return;
    }
#define CR_ROW(name, cur, prev_field) do { \
    uint64_t _c = (cur), _p = prev ? (prev_field) : _c; \
    int _ch = (_c != _p); \
    printf("  %s%-5s%s  0x%016llx%s\n", \
           _ch ? ANSI_YELLOW ANSI_BOLD : "", name, \
           _ch ? ANSI_RESET : "", (unsigned long long)_c, \
           _ch ? ANSI_DIM " <--" ANSI_RESET : ""); \
} while(0)
    CR_ROW("CR0", s->cr.cr0, prev->cr.cr0); decode_cr0(s->cr.cr0);
    CR_ROW("CR2", s->cr.cr2, prev->cr.cr2);
    CR_ROW("CR3", s->cr.cr3, prev->cr.cr3);
    CR_ROW("CR4", s->cr.cr4, prev->cr.cr4); decode_cr4(s->cr.cr4);
    CR_ROW("CR8", s->cr.cr8, prev->cr.cr8);
#undef CR_ROW
}

void hw_print_dr(const HwRegState *s, const HwRegState *prev)
{
    printf(ANSI_BOLD ANSI_CYAN "── Debug Registers ──────────────────────────────────────────\n" ANSI_RESET);
    if (!s->kmod_available) {
        printf("  " ANSI_DIM "(kmod /dev/asmrepl not loaded)" ANSI_RESET "\n");
        return;
    }
    for (int i = 0; i < 8; i++) {
        uint64_t cur = s->dr.dr[i];
        uint64_t prv = prev ? prev->dr.dr[i] : cur;
        int changed = (cur != prv);
        printf("  %sDR%d%s    0x%016llx%s\n",
               changed ? ANSI_YELLOW ANSI_BOLD : "",
               i,
               changed ? ANSI_RESET : "",
               (unsigned long long)cur,
               changed ? ANSI_DIM " <--" ANSI_RESET : "");
    }
    /* DR6 condition bits */
    uint64_t dr6 = s->dr.dr[6];
    printf("  " ANSI_DIM "DR6: [%s%s%s%s%s]\n" ANSI_RESET,
           (dr6 >> 13) & 1 ? "BD " : "",
           (dr6 >> 14) & 1 ? "BS " : "",
           (dr6 >>  0) & 1 ? "B0 " : "",
           (dr6 >>  1) & 1 ? "B1 " : "",
           (dr6 >>  2) & 1 ? "B2 " : "");
}

void hw_print_msr(const HwRegState *s, const HwRegState *prev)
{
    printf(ANSI_BOLD ANSI_CYAN "── MSRs ─────────────────────────────────────────────────────\n" ANSI_RESET);
    if (!s->kmod_available) {
        printf("  " ANSI_DIM "(kmod /dev/asmrepl not loaded)" ANSI_RESET "\n");
        return;
    }
    const HwMsrState *m  = &s->msr;
    const HwMsrState *mp = prev ? &prev->msr : m;

#define MSR_ROW(name, cur, pval) do { \
    int _ch = ((cur) != (pval)); \
    printf("  %s%-12s%s  0x%016llx%s\n", \
           _ch ? ANSI_YELLOW ANSI_BOLD : "", name, \
           _ch ? ANSI_RESET : "", (unsigned long long)(cur), \
           _ch ? ANSI_DIM " <--" ANSI_RESET : ""); \
} while(0)
    MSR_ROW("EFER",      m->efer,      mp->efer);
    MSR_ROW("LSTAR",     m->lstar,     mp->lstar);
    MSR_ROW("FS.BASE",   m->fs_base,   mp->fs_base);
    MSR_ROW("GS.BASE",   m->gs_base,   mp->gs_base);
    MSR_ROW("KERNEL_GS", m->kernel_gs, mp->kernel_gs);
    MSR_ROW("TSC",       m->tsc,       mp->tsc);
    MSR_ROW("APIC_BASE", m->apic_base, mp->apic_base);
#undef MSR_ROW
}
