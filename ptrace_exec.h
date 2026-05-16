/*
 * ptrace_exec.h — ptrace-based snippet execution engine
 *
 * Manages a long-lived child process into which machine code snippets are
 * injected and executed one at a time via ptrace.  After each snippet the
 * child hits a software breakpoint (INT3) and the parent reads back all
 * register state through hw_regs_capture_ptrace().
 *
 * Lifecycle:
 *   pe = ptrace_exec_create()
 *   while (user keeps typing):
 *       bytes = asm_encode(line)
 *       ptrace_exec_run(pe, bytes, len, &state_out)
 *       hw_print_gpr(&state_out, &prev)
 *   ptrace_exec_destroy(pe)
 */
#ifndef PTRACE_EXEC_H
#define PTRACE_EXEC_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "hw_regs.h"

/* Maximum bytes for a single injected snippet (including trailing INT3) */
#define PTRACE_EXEC_MAX_CODE  256

/* Address of the executable trampoline page in the child process */
#define PTRACE_EXEC_CODE_VADDR  0x40000000UL

typedef struct {
    pid_t    child_pid;
    int      asmrepl_fd;    /* /dev/asmrepl fd (may be -1) */
    int      devmem_fd;     /* /dev/mem fd (may be -1) */

    /* Saved register state before each snippet for delta display */
    HwRegState prev;
    int        have_prev;
} PtraceExec;

/*
 * ptrace_exec_create — fork the trampoline child, set up shared code page.
 * asmrepl_fd and devmem_fd are passed through for path-2/3 capture;
 * pass -1 for either if not available.
 * Returns NULL on failure.
 */
PtraceExec *ptrace_exec_create(int asmrepl_fd, int devmem_fd);

/*
 * ptrace_exec_destroy — kill the child and free the handle.
 */
void ptrace_exec_destroy(PtraceExec *pe);

/*
 * ptrace_exec_run — inject and execute one machine-code snippet.
 *
 * code     : raw x86-64 bytes (do NOT include terminating INT3; added here)
 * code_len : number of bytes (must be <= PTRACE_EXEC_MAX_CODE - 1)
 * out      : filled with full HwRegState after execution
 *
 * Returns:
 *   0   — snippet executed, *out valid
 *  -1   — execution error (child may be in bad state; caller should destroy/recreate)
 */
int ptrace_exec_run(PtraceExec *pe, const uint8_t *code, size_t code_len,
                    HwRegState *out);

/*
 * ptrace_exec_reset — re-zero all GPRs in the child (except RSP/RIP) and
 * clear xmm registers.  Useful for "reset" REPL command.
 */
int ptrace_exec_reset(PtraceExec *pe);

#endif /* PTRACE_EXEC_H */
