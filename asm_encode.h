/*
 * asm_encode.h — assemble one line of AT&T/Intel syntax to machine bytes
 *
 * Delegates to the system 'as' (GNU assembler) via a temporary file.
 * This avoids linking a full assembler library.
 *
 * The caller receives raw x86-64 bytes.  All addressing is assumed
 * 64-bit (assembler is invoked with --64).
 */
#ifndef ASM_ENCODE_H
#define ASM_ENCODE_H

#include <stdint.h>
#include <stddef.h>

/* Maximum output size (one instruction ≤ 15 bytes; buffer for multi-insn) */
#define ASM_ENCODE_MAX_OUT  256

/*
 * asm_encode_intel — assemble Intel-syntax instruction(s) to machine bytes.
 *
 *   src     : null-terminated source line, e.g. "mov rax, 42"
 *   out     : caller-supplied buffer of at least ASM_ENCODE_MAX_OUT bytes
 *   out_len : set to number of bytes written
 *   errbuf  : if non-NULL and an error occurs, filled with assembler stderr
 *   errsz   : size of errbuf
 *
 * Returns 0 on success, -1 on failure.
 *
 * Note: Intel syntax (.intel_syntax noprefix) is the default here because
 * most assembly education material uses it.  Use asm_encode_att for AT&T.
 */
int asm_encode_intel(const char *src,
                     uint8_t *out, size_t *out_len,
                     char *errbuf, size_t errsz);

/*
 * asm_encode_att — assemble AT&T-syntax instruction(s).
 */
int asm_encode_att(const char *src,
                   uint8_t *out, size_t *out_len,
                   char *errbuf, size_t errsz);

/*
 * asm_encode_hex — parse a hex string like "48 89 c8" or "4889c8" to bytes.
 * Useful for injecting raw opcodes without the assembler.
 * Returns 0 on success, -1 on parse error.
 */
int asm_encode_hex(const char *hex,
                   uint8_t *out, size_t *out_len);

#endif /* ASM_ENCODE_H */
