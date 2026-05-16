/*
 * asm_encode.c — assemble source text to x86-64 machine bytes
 *
 * Strategy:
 *   1. Write a minimal .s file to a mkstemp() path.
 *   2. Run: as --64 [--msyntax=intel --mnaked-reg] -o <obj> <src>
 *   3. Run: objcopy -O binary --only-section=.text <obj> <bin>
 *   4. Read the binary back.
 *   5. Unlink temp files.
 *
 * This requires GNU binutils (as, objcopy) to be on PATH, which is
 * standard on any Linux development system.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "asm_encode.h"

/* ── internal helpers ─────────────────────────────────────────────── */

/*
 * run_cmd — execute argv[0..] with stdin /dev/null.
 * Captures stderr into errbuf if non-NULL.
 * Returns the exit code, or -1 on fork/exec failure.
 */
static int run_cmd(char *const argv[], char *errbuf, size_t errsz)
{
    int pipe_fds[2] = {-1, -1};
    if (errbuf && errsz > 0) {
        if (pipe(pipe_fds) < 0) return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* child */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }

        if (pipe_fds[1] >= 0) {
            dup2(pipe_fds[1], STDERR_FILENO);
            dup2(pipe_fds[1], STDOUT_FILENO);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        } else {
            /* suppress output */
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) {
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                close(null);
            }
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent */
    if (pipe_fds[1] >= 0) close(pipe_fds[1]);

    if (pipe_fds[0] >= 0 && errbuf && errsz > 0) {
        size_t total = 0;
        ssize_t n;
        while (total < errsz - 1 &&
               (n = read(pipe_fds[0], errbuf + total, errsz - 1 - total)) > 0)
            total += n;
        errbuf[total] = '\0';
        close(pipe_fds[0]);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * make_asm_src — write the .s wrapper to path.
 * intel=1 → .intel_syntax noprefix
 * intel=0 → AT&T syntax (default)
 */
static int make_asm_src(const char *path, const char *src, int intel)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    if (intel) {
        fprintf(f, ".intel_syntax noprefix\n");
    }
    fprintf(f, ".section .text\n");
    fprintf(f, ".globl _start\n");
    fprintf(f, "_start:\n");
    /* The source may contain multiple lines / semicolons */
    fprintf(f, "%s\n", src);
    if (intel) {
        /* restore default to avoid confusing future invocations */
        fprintf(f, ".att_syntax\n");
    }
    fclose(f);
    return 0;
}

/*
 * read_binary — read at most max_len bytes from path into buf.
 * Sets *len to actual bytes read.  Returns 0 on success, -1 on error.
 */
static int read_binary(const char *path, uint8_t *buf, size_t max_len, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    *len = fread(buf, 1, max_len, f);
    int err = ferror(f);
    fclose(f);
    return err ? -1 : 0;
}

/* Core implementation shared by intel/att paths */
static int encode_impl(const char *src, int intel,
                        uint8_t *out, size_t *out_len,
                        char *errbuf, size_t errsz)
{
    /* mkstemp requires writable template; use /tmp */
    char src_path[64], obj_path[64], bin_path[64];
    snprintf(src_path, sizeof(src_path), "/tmp/asmrepl_XXXXXX.s");
    snprintf(obj_path, sizeof(obj_path), "/tmp/asmrepl_XXXXXX.o");
    snprintf(bin_path, sizeof(bin_path), "/tmp/asmrepl_XXXXXX.bin");

    int sfd = mkstemps(src_path, 2);
    if (sfd < 0) { perror("mkstemps src"); return -1; }
    close(sfd);

    int ofd = mkstemps(obj_path, 2);
    if (ofd < 0) { perror("mkstemps obj"); unlink(src_path); return -1; }
    close(ofd);

    int bfd = mkstemps(bin_path, 4);
    if (bfd < 0) { perror("mkstemps bin"); unlink(src_path); unlink(obj_path); return -1; }
    close(bfd);

    int rc = -1;

    if (make_asm_src(src_path, src, intel) < 0)
        goto cleanup;

    /* Step 1: assemble */
    char *as_argv[] = {
        "as", "--64", src_path, "-o", obj_path, NULL
    };
    if (errbuf && errsz) errbuf[0] = '\0';
    int as_rc = run_cmd(as_argv, errbuf, errsz);
    if (as_rc != 0)
        goto cleanup;

    /* Step 2: strip to raw binary */
    char *oc_argv[] = {
        "objcopy", "-O", "binary", "--only-section=.text",
        obj_path, bin_path, NULL
    };
    int oc_rc = run_cmd(oc_argv, NULL, 0);
    if (oc_rc != 0) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "objcopy failed (exit %d)", oc_rc);
        goto cleanup;
    }

    /* Step 3: read result */
    if (read_binary(bin_path, out, ASM_ENCODE_MAX_OUT, out_len) < 0) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "failed to read output binary");
        goto cleanup;
    }

    if (*out_len == 0) {
        if (errbuf && errsz)
            snprintf(errbuf, errsz, "assembler produced empty output");
        goto cleanup;
    }

    rc = 0;

cleanup:
    unlink(src_path);
    unlink(obj_path);
    unlink(bin_path);
    return rc;
}

/* ── public API ──────────────────────────────────────────────────── */

int asm_encode_intel(const char *src,
                     uint8_t *out, size_t *out_len,
                     char *errbuf, size_t errsz)
{
    return encode_impl(src, 1, out, out_len, errbuf, errsz);
}

int asm_encode_att(const char *src,
                   uint8_t *out, size_t *out_len,
                   char *errbuf, size_t errsz)
{
    return encode_impl(src, 0, out, out_len, errbuf, errsz);
}

int asm_encode_hex(const char *hex,
                   uint8_t *out, size_t *out_len)
{
    size_t len = 0;
    const char *p = hex;

    while (*p) {
        /* skip whitespace and separator characters */
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == '_'))
            p++;
        if (!*p) break;

        /* must have two hex digits */
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            return -1;
        }
        if (len >= ASM_ENCODE_MAX_OUT) return -1;

        char tmp[3] = { p[0], p[1], '\0' };
        out[len++] = (uint8_t)strtoul(tmp, NULL, 16);
        p += 2;
    }

    *out_len = len;
    return (len == 0) ? -1 : 0;
}
