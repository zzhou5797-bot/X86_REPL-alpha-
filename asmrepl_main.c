/*
 * asmrepl_main.c — interactive x86-64 assembly REPL
 *
 * Usage:
 *   asmrepl [--att]               Intel syntax by default
 *   asmrepl --hex                 hex-bytes input mode
 *
 * Built-in commands (prefix with '.'):
 *   .gpr          show GPR + rflags + segment bases
 *   .xmm          show XMM0–XMM15
 *   .fpu          show x87 ST(0-7) + FCW/FSW/MXCSR
 *   .cr           show CR0/CR2/CR3/CR4/CR8  (requires kmod)
 *   .dr           show DR0–DR7              (requires kmod)
 *   .msr          show MSR whitelist        (requires kmod)
 *   .msr <addr>   read arbitrary MSR by hex address (e.g. .msr c0000080)
 *   .all          show all register groups
 *   .reset        zero GPR/XMM state in child process
 *   .syntax [intel|att|hex]  switch input syntax
 *   .help         show this help
 *   .quit / .exit / Ctrl-D
 *
 * Architecture:
 *   - libedit (BSD editline) for readline-compatible input + history
 *   - asm_encode: delegates assembly to GNU 'as' + objcopy
 *   - ptrace_exec: long-lived child process, injected one snippet per line
 *   - hw_regs: three-path register capture
 *   - kmod: /dev/asmrepl for CR/DR/MSR (optional)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>

#include <editline/readline.h>

#include "asm_encode.h"
#include "ptrace_exec.h"
#include "hw_regs.h"
#include "include/asmrepl_ioctl.h"

/* ── globals ─────────────────────────────────────────────────────── */

typedef enum { SYNTAX_INTEL, SYNTAX_ATT, SYNTAX_HEX } Syntax;

static Syntax       g_syntax     = SYNTAX_INTEL;
static PtraceExec  *g_pe         = NULL;
static int          g_asmrepl_fd = -1;
static int          g_devmem_fd  = -1;
static int          g_show_delta = 1;  /* highlight changed registers */

/* Last captured state for delta display */
static HwRegState   g_prev;
static int          g_have_prev  = 0;

/* ── ANSI helpers ─────────────────────────────────────────────────── */
#define ANSI_RESET  "\033[0m"
#define ANSI_BOLD   "\033[1m"
#define ANSI_GREEN  "\033[32m"
#define ANSI_RED    "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN   "\033[36m"
#define ANSI_DIM    "\033[2m"

/* ── helpers ─────────────────────────────────────────────────────── */

static void print_help(void)
{
    printf(ANSI_BOLD "asmrepl — x86-64 assembly REPL\n" ANSI_RESET);
    printf("Type an instruction (or hex bytes) and press Enter.\n");
    printf("Registers that change are highlighted in " ANSI_YELLOW "yellow" ANSI_RESET ".\n\n");
    printf(ANSI_BOLD "Built-in commands:\n" ANSI_RESET);
    printf("  .gpr              GPR + rflags + segment bases\n");
    printf("  .xmm              XMM0–XMM15\n");
    printf("  .fpu              x87 ST(0-7) + FCW/FSW/MXCSR\n");
    printf("  .cr               CR0/CR2/CR3/CR4/CR8  [needs kmod]\n");
    printf("  .dr               DR0–DR7              [needs kmod]\n");
    printf("  .msr              MSR whitelist         [needs kmod]\n");
    printf("  .msr <hex_addr>   read single MSR\n");
    printf("  .all              all register groups\n");
    printf("  .reset            zero GPR/XMM in child\n");
    printf("  .syntax intel|att|hex\n");
    printf("  .help             this message\n");
    printf("  .quit / .exit / Ctrl-D\n\n");
    printf(ANSI_BOLD "Syntax modes:\n" ANSI_RESET);
    printf("  intel  mov rax, 42          (default)\n");
    printf("  att    movq $42, %%rax\n");
    printf("  hex    48 c7 c0 2a 00 00 00\n");
}

static const char *syntax_name(Syntax s)
{
    switch (s) {
    case SYNTAX_INTEL: return "intel";
    case SYNTAX_ATT:   return "att";
    case SYNTAX_HEX:   return "hex";
    }
    return "?";
}

/* Print hex dump of machine bytes */
static void print_bytes(const uint8_t *b, size_t n)
{
    printf(ANSI_DIM "  bytes: ");
    for (size_t i = 0; i < n; i++) printf("%02x ", b[i]);
    printf(ANSI_RESET "\n");
}

/* Strip leading/trailing whitespace in-place; returns pointer to first non-space */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

/* ── command dispatch ─────────────────────────────────────────────── */

/* Capture current state without executing anything */
static void cmd_show(const char *which)
{
    if (!g_have_prev) {
        printf(ANSI_DIM "  (no state yet — execute an instruction first)\n" ANSI_RESET);
        return;
    }
    const HwRegState *prev = g_show_delta ? &g_prev : NULL;

    if (strcmp(which, "gpr") == 0)
        hw_print_gpr(&g_prev, NULL);
    else if (strcmp(which, "xmm") == 0)
        hw_print_xmm(&g_prev, NULL);
    else if (strcmp(which, "fpu") == 0)
        hw_print_fpu(&g_prev, NULL);
    else if (strcmp(which, "cr") == 0)
        hw_print_cr(&g_prev, NULL);
    else if (strcmp(which, "dr") == 0)
        hw_print_dr(&g_prev, NULL);
    else if (strcmp(which, "msr") == 0)
        hw_print_msr(&g_prev, NULL);
    else if (strcmp(which, "all") == 0) {
        hw_print_gpr(&g_prev, NULL);
        hw_print_xmm(&g_prev, NULL);
        hw_print_fpu(&g_prev, NULL);
        hw_print_cr(&g_prev, NULL);
        hw_print_dr(&g_prev, NULL);
        hw_print_msr(&g_prev, NULL);
    }
    (void)prev;
}

static void cmd_read_msr(const char *addr_str)
{
    if (g_asmrepl_fd < 0) {
        printf(ANSI_RED "  kmod /dev/asmrepl not available\n" ANSI_RESET);
        return;
    }
    char *end;
    unsigned long addr = strtoul(addr_str, &end, 16);
    if (end == addr_str || *end != '\0') {
        printf(ANSI_RED "  invalid MSR address: %s\n" ANSI_RESET, addr_str);
        return;
    }

    struct asmrepl_msr_req req = { .msr_addr = (uint32_t)addr };
    if (ioctl(g_asmrepl_fd, ASMREPL_GET_MSR, &req) < 0) {
        perror("  ioctl ASMREPL_GET_MSR");
        return;
    }
    if (req.error != 0) {
        printf(ANSI_RED "  MSR 0x%08lx: read error (%s)\n" ANSI_RESET,
               addr, req.error == -EIO ? "#GP fault" : "EPERM (not in whitelist)");
        return;
    }
    printf("  MSR 0x%08lx = 0x%016llx\n", addr, (unsigned long long)req.value);
}

static void cmd_syntax(const char *arg)
{
    if (!arg || *arg == '\0') {
        printf("  current syntax: %s\n", syntax_name(g_syntax));
        return;
    }
    if (strcmp(arg, "intel") == 0) { g_syntax = SYNTAX_INTEL; }
    else if (strcmp(arg, "att") == 0)   { g_syntax = SYNTAX_ATT; }
    else if (strcmp(arg, "hex") == 0)   { g_syntax = SYNTAX_HEX; }
    else {
        printf(ANSI_RED "  unknown syntax '%s'. Use: intel att hex\n" ANSI_RESET, arg);
        return;
    }
    printf("  syntax → %s\n", syntax_name(g_syntax));
}

/* Returns 1 if the line was a command, 0 if it should be assembled, -1 to quit */
static int handle_command(char *line)
{
    /* All commands start with '.' */
    if (line[0] != '.') return 0;

    char *cmd = line + 1;
    char *arg = cmd;
    while (*arg && !isspace((unsigned char)*arg)) arg++;
    if (*arg) { *arg++ = '\0'; arg = trim(arg); }

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        return -1;
    } else if (strcmp(cmd, "help") == 0) {
        print_help();
    } else if (strcmp(cmd, "gpr") == 0 || strcmp(cmd, "xmm") == 0 ||
               strcmp(cmd, "fpu") == 0 || strcmp(cmd, "cr")  == 0 ||
               strcmp(cmd, "dr")  == 0 || strcmp(cmd, "all") == 0) {
        cmd_show(cmd);
    } else if (strcmp(cmd, "msr") == 0) {
        if (*arg)
            cmd_read_msr(arg);
        else
            cmd_show("msr");
    } else if (strcmp(cmd, "reset") == 0) {
        if (ptrace_exec_reset(g_pe) == 0) {
            g_have_prev = 0;
            printf("  registers zeroed\n");
        }
    } else if (strcmp(cmd, "syntax") == 0) {
        cmd_syntax(*arg ? arg : NULL);
    } else {
        printf(ANSI_RED "  unknown command '.%s'. Try .help\n" ANSI_RESET, cmd);
    }

    return 1;
}

/* ── SIGINT handler — don't kill the REPL, just print a newline ──── */
static volatile sig_atomic_t g_interrupted = 0;
static void sigint_handler(int sig) { (void)sig; g_interrupted = 1; }

/* ── main REPL loop ──────────────────────────────────────────────── */

static void repl_loop(void)
{
    char prompt[64];
    char errbuf[512];

    for (;;) {
        snprintf(prompt, sizeof(prompt),
                 ANSI_BOLD ANSI_GREEN "asm(%s)> " ANSI_RESET,
                 syntax_name(g_syntax));

        g_interrupted = 0;
        char *line = readline(prompt);

        if (!line) {
            /* EOF (Ctrl-D) */
            printf("\n");
            break;
        }
        if (g_interrupted) {
            free(line);
            printf("\n");
            continue;
        }

        char *trimmed = trim(line);
        if (*trimmed == '\0') {
            free(line);
            continue;
        }

        /* Add to history (skip duplicate consecutive entries) */
        add_history(trimmed);

        /* Handle built-in commands */
        int cmd_rc = handle_command(trimmed);
        if (cmd_rc < 0) { free(line); break; }
        if (cmd_rc > 0) { free(line); continue; }

        /* Assemble the input */
        uint8_t code[ASM_ENCODE_MAX_OUT];
        size_t  code_len = 0;
        int     enc_rc;

        errbuf[0] = '\0';
        switch (g_syntax) {
        case SYNTAX_INTEL:
            enc_rc = asm_encode_intel(trimmed, code, &code_len, errbuf, sizeof(errbuf));
            break;
        case SYNTAX_ATT:
            enc_rc = asm_encode_att(trimmed, code, &code_len, errbuf, sizeof(errbuf));
            break;
        case SYNTAX_HEX:
            enc_rc = asm_encode_hex(trimmed, code, &code_len);
            if (enc_rc < 0) snprintf(errbuf, sizeof(errbuf), "invalid hex string");
            break;
        default:
            enc_rc = -1;
        }

        if (enc_rc < 0) {
            printf(ANSI_RED "  assemble error:" ANSI_RESET "\n");
            if (*errbuf) {
                /* Indent assembler error message */
                char *p = errbuf;
                while (*p) {
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = '\0';
                    printf("  %s\n", p);
                    if (!nl) break;
                    p = nl + 1;
                }
            }
            free(line);
            continue;
        }

        print_bytes(code, code_len);

        /* Execute snippet in child */
        HwRegState state;
        int run_rc = ptrace_exec_run(g_pe, code, code_len, &state);
        if (run_rc < 0) {
            printf(ANSI_RED "  execution error — restarting child\n" ANSI_RESET);
            ptrace_exec_destroy(g_pe);
            g_pe = ptrace_exec_create(g_asmrepl_fd, g_devmem_fd);
            if (!g_pe) {
                fprintf(stderr, "fatal: cannot restart child process\n");
                free(line);
                break;
            }
            g_have_prev = 0;
            free(line);
            continue;
        }

        /* Display changed registers */
        const HwRegState *prev = g_have_prev ? &g_prev : NULL;
        hw_print_gpr(&state, prev);

        /* Only show FPR/XMM if they changed */
        if (prev) {
            if (memcmp(&state.fpr.xmm_space, &prev->fpr.xmm_space,
                       sizeof(state.fpr.xmm_space)) != 0) {
                hw_print_xmm(&state, prev);
            }
            if (memcmp(&state.fpr.st_space, &prev->fpr.st_space,
                       sizeof(state.fpr.st_space)) != 0) {
                hw_print_fpu(&state, prev);
            }
        }

        /* Update saved state */
        g_prev      = state;
        g_have_prev = 1;

        free(line);
    }
}

/* ── entry point ─────────────────────────────────────────────────── */

static const struct option kLongOpts[] = {
    { "att",   no_argument, NULL, 'a' },
    { "hex",   no_argument, NULL, 'x' },
    { "intel", no_argument, NULL, 'i' },
    { "help",  no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 }
};

int main(int argc, char *argv[])
{
    int opt;
    while ((opt = getopt_long(argc, argv, "axih", kLongOpts, NULL)) != -1) {
        switch (opt) {
        case 'a': g_syntax = SYNTAX_ATT;   break;
        case 'x': g_syntax = SYNTAX_HEX;   break;
        case 'i': g_syntax = SYNTAX_INTEL;  break;
        case 'h': print_help(); return 0;
        default:
            fprintf(stderr, "usage: asmrepl [--att] [--hex] [--intel]\n");
            return 1;
        }
    }

    /* Initialize register access paths */
    g_asmrepl_fd = hw_regs_init(&g_devmem_fd);
    if (g_asmrepl_fd < 0)
        printf(ANSI_DIM "  note: /dev/asmrepl not available (CR/DR/MSR disabled)\n"
               "        load kmod/asmrepl.ko to enable\n" ANSI_RESET);
    if (g_devmem_fd < 0)
        printf(ANSI_DIM "  note: /dev/mem not available (MMIO reads disabled)\n" ANSI_RESET);

    /* Create ptrace execution child */
    g_pe = ptrace_exec_create(g_asmrepl_fd, g_devmem_fd);
    if (!g_pe) {
        fprintf(stderr, "asmrepl: failed to create execution child\n");
        hw_regs_fini(g_asmrepl_fd, g_devmem_fd);
        return 1;
    }

    /* SIGINT — don't exit, just interrupt current readline */
    struct sigaction sa = { .sa_handler = sigint_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* libedit / readline setup */
    rl_initialize();
    using_history();

    /* Check assembler availability */
    {
        uint8_t tmp[16]; size_t tlen;
        char eb[256];
        if (asm_encode_intel("nop", tmp, &tlen, eb, sizeof(eb)) < 0) {
            fprintf(stderr,
                "asmrepl: assembler not available ('as' + 'objcopy' must be on PATH)\n"
                "         install binutils: sudo apt install binutils\n");
            ptrace_exec_destroy(g_pe);
            hw_regs_fini(g_asmrepl_fd, g_devmem_fd);
            return 1;
        }
    }

    printf(ANSI_BOLD ANSI_CYAN
           "x86-64 Assembly REPL  —  type .help for commands\n"
           ANSI_RESET);
    printf("Syntax: %s   |   Execution: ptrace child pid %d\n",
           syntax_name(g_syntax), (int)g_pe->child_pid);
    if (g_asmrepl_fd >= 0)
        printf("kmod:   /dev/asmrepl active (CR/DR/MSR enabled)\n");
    printf("\n");

    repl_loop();

    ptrace_exec_destroy(g_pe);
    hw_regs_fini(g_asmrepl_fd, g_devmem_fd);
    return 0;
}
