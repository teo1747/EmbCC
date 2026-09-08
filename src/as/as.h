/* EmbAS — a standalone NASM/Intel-syntax assembler (A1).
 *
 * The last external tool in the EmbLinkOS kernel build is nasm: the kernel's
 * hand-written `.asm` are NASM syntax, which EmbCC's inline-asm encoder (AT&T,
 * operand-resolved) cannot read. Rather than port nasm, EmbCC grows its own
 * assembler front-end so the toolchain owns the whole build. Output is either a
 * relocatable ELF object (`-f elf64`, the kernel `.asm`) or a flat binary
 * (`-f bin`, the AP trampoline). Correctness bar: byte-identical to nasm on the
 * kernel corpus.
 */
#ifndef EMBCC_AS_AS_H
#define EMBCC_AS_AS_H

enum as_format { AS_ELF64, AS_BIN };

/* Assemble `in_path` to `out_path` in the given format. Returns 0, or 1 with a
 * diagnostic on stderr. */
int as_assemble(const char *in_path, const char *out_path, enum as_format fmt);

#endif
