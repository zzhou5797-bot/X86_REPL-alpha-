/*
 * ptrace_exec.c — ptrace-based snippet execution engine
 *
 * Child process layout:
 *   0x40000000  — one anonymous page, RWX, filled with NOPs then INT3.
 *                 Parent writes each snippet here via PTRACE_POKETEXT.
 *   Stack       — the kernel allocates a default stack (no ELF loader needed).
 *
 * Execution protocol (per snippet):
 *   1. Parent writes code + INT3 to child page via PTRACE_POKETEXT words.
 *   2. Parent sets RIP = PTRACE_EXEC_CODE_VADDR via PTRACE_SETREGS.
 *   3. Parent issues PTRACE_CONT.
 *   4. Child runs until INT3 → SIGTRAP.
 *   5. Parent calls waitpid, then reads registers via PTRACE_GETREGS/GETFPREGS.
 *   6. Parent optionally reads CR/DR/MSR via ioctl.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

#include "ptrace_exec.h"
#include "hw_regs.h"

/* ── child trampoline ─────────────────────────────────────────────── */

/*
 * This function runs in the child.  It:
 *   1. Maps an RWX page at PTRACE_EXEC_CODE_VADDR.
 *   2. Fills the page with INT3 (0xCC) as a safety net.
 *   3. Notifies the parent it is ready via SIGSTOP.
 *   4. Loops forever — the parent drives it one snippet at a time.
 */
__attribute__((noreturn)) static void child_trampoline(void)
{
    /* Map the code page */
    void *page = mmap((void *)PTRACE_EXEC_CODE_VADDR,
                      4096,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                      -1, 0);
    if (page == MAP_FAILED) {
        perror("child: mmap code page");
        _exit(1);
    }
    /* Fill with INT3 */
    memset(page, 0xCC, 4096);

    /* Allow ptrace from parent */
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
        perror("child: PTRACE_TRACEME");
        _exit(1);
    }

    /* Signal readiness — parent's waitpid will see SIGSTOP */
    raise(SIGSTOP);

    /*
     * Parent will now repeatedly:
     *   - Write code to the page
     *   - Set RIP and PTRACE_CONT us
     * We will execute code and hit the trailing INT3, producing SIGTRAP.
     * The parent handles that SIGTRAP and sends PTRACE_CONT again.
     * We never reach code here; we live inside the ptrace loop.
     */
    for (;;)
        __asm__ volatile("int3");

    __builtin_unreachable();
}

/* ── write code into child via PTRACE_POKETEXT ───────────────────── */

static int poke_code(pid_t pid, uint64_t vaddr, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        uint64_t word = 0;
        size_t   chunk = (len - off < 8) ? (len - off) : 8;
        memcpy(&word, buf + off, chunk);
        errno = 0;
        if (ptrace(PTRACE_POKETEXT, pid, (void *)(vaddr + off), (void *)word) < 0) {
            if (errno) { perror("PTRACE_POKETEXT"); return -1; }
        }
        off += 8;
    }
    return 0;
}

/* ── public API ──────────────────────────────────────────────────── */

PtraceExec *ptrace_exec_create(int asmrepl_fd, int devmem_fd)
{
    PtraceExec *pe = calloc(1, sizeof(*pe));
    if (!pe) return NULL;

    pe->asmrepl_fd = asmrepl_fd;
    pe->devmem_fd  = devmem_fd;
    pe->have_prev  = 0;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free(pe);
        return NULL;
    }

    if (pid == 0) {
        /* child */
        child_trampoline();
        /* unreachable */
    }

    /* parent — wait for child's initial SIGSTOP */
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid (initial)");
        kill(pid, SIGKILL);
        free(pe);
        return NULL;
    }
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP) {
        fprintf(stderr, "ptrace_exec_create: unexpected child status 0x%x\n", status);
        kill(pid, SIGKILL);
        free(pe);
        return NULL;
    }

    pe->child_pid = pid;
    return pe;
}

void ptrace_exec_destroy(PtraceExec *pe)
{
    if (!pe) return;
    if (pe->child_pid > 0) {
        kill(pe->child_pid, SIGKILL);
        waitpid(pe->child_pid, NULL, 0);
    }
    free(pe);
}

int ptrace_exec_run(PtraceExec *pe, const uint8_t *code, size_t code_len,
                    HwRegState *out)
{
    if (code_len >= PTRACE_EXEC_MAX_CODE) {
        fprintf(stderr, "ptrace_exec_run: snippet too large (%zu)\n", code_len);
        return -1;
    }

    /* Build buffer: code + INT3 terminator */
    uint8_t buf[PTRACE_EXEC_MAX_CODE + 1];
    memcpy(buf, code, code_len);
    buf[code_len] = 0xCC; /* INT3 */

    /* Write snippet to child's code page */
    if (poke_code(pe->child_pid, PTRACE_EXEC_CODE_VADDR, buf, code_len + 1) < 0)
        return -1;

    /* Preserve all registers except RIP — let GPRs accumulate across snippets */
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pe->child_pid, NULL, &regs) < 0) {
        perror("PTRACE_GETREGS (before run)");
        return -1;
    }
    regs.rip = PTRACE_EXEC_CODE_VADDR;
    if (ptrace(PTRACE_SETREGS, pe->child_pid, NULL, &regs) < 0) {
        perror("PTRACE_SETREGS");
        return -1;
    }

    /* Run child */
    if (ptrace(PTRACE_CONT, pe->child_pid, NULL, NULL) < 0) {
        perror("PTRACE_CONT");
        return -1;
    }

    /* Wait for INT3 trap */
    int status;
    if (waitpid(pe->child_pid, &status, 0) < 0) {
        perror("waitpid (after run)");
        return -1;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "ptrace_exec_run: child exited unexpectedly (code %d)\n",
                WEXITSTATUS(status));
        pe->child_pid = -1;
        return -1;
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "ptrace_exec_run: child not stopped (status 0x%x)\n", status);
        return -1;
    }
    if (WSTOPSIG(status) != SIGTRAP) {
        fprintf(stderr, "ptrace_exec_run: unexpected signal %d\n", WSTOPSIG(status));
        /* Deliver the signal and try to recover */
        ptrace(PTRACE_CONT, pe->child_pid, NULL, (void *)(uintptr_t)WSTOPSIG(status));
        return -1;
    }

    /* Capture all paths */
    memset(out, 0, sizeof(*out));
    if (hw_regs_capture_ptrace(out, pe->child_pid) < 0)
        return -1;
    hw_regs_capture_kmod(out, pe->asmrepl_fd);
    hw_regs_capture_mmio(out, pe->devmem_fd);

    /* Rewind RIP by 1 to account for the INT3 the CPU has already advanced past.
     * After INT3, RIP points to the byte AFTER the 0xCC.  We don't need to
     * actually fix it because we overwrite RIP before every PTRACE_CONT, but
     * record the real post-execution RIP (before INT3) for display. */
    out->gpr.rip = PTRACE_EXEC_CODE_VADDR + code_len;

    /* Save for next delta */
    pe->prev      = *out;
    pe->have_prev = 1;

    return 0;
}

int ptrace_exec_reset(PtraceExec *pe)
{
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pe->child_pid, NULL, &regs) < 0) {
        perror("PTRACE_GETREGS (reset)");
        return -1;
    }

    uint64_t saved_rsp = regs.rsp;
    memset(&regs, 0, sizeof(regs));
    regs.rsp    = saved_rsp;
    regs.rip    = PTRACE_EXEC_CODE_VADDR;
    /* Segment registers must be valid — restore to canonical user-mode values */
    regs.cs     = 0x33;
    regs.ss     = 0x2b;
    regs.ds     = 0x00;
    regs.es     = 0x00;
    regs.fs     = 0x00;
    regs.gs     = 0x00;

    if (ptrace(PTRACE_SETREGS, pe->child_pid, NULL, &regs) < 0) {
        perror("PTRACE_SETREGS (reset)");
        return -1;
    }

    /* Zero XMM/x87 state */
    struct user_fpregs_struct fpregs;
    memset(&fpregs, 0, sizeof(fpregs));
    fpregs.cwd = 0x037F; /* default x87 control word */
    fpregs.mxcsr = 0x1F80; /* default MXCSR: all exceptions masked */
    if (ptrace(PTRACE_SETFPREGS, pe->child_pid, NULL, &fpregs) < 0) {
        perror("PTRACE_SETFPREGS (reset)");
        return -1;
    }

    pe->have_prev = 0;
    return 0;
}
