/* EmbDBG core, the slice EmbLD reuses: turn a relocatable debug object into a
 * native .embdbg with absolute addresses, at link time. Compiling embdbg.c
 * with -DEMBDBG_NO_MAIN drops its CLI main and leaves this one entry point, so
 * the linker and the standalone tool share one implementation of the format
 * (no second writer — the "one reader, one dumper" discipline). */
#ifndef EMBCC_TOOLS_EMBDBG_CORE_H
#define EMBCC_TOOLS_EMBDBG_CORE_H

/* Parse one object's DWARF, biasing every code address by `addr_bias` (its
 * final .text vaddr, so a .o's .text-relative addresses become absolute), and
 * write `out` as a .embdbg whose build_id is SHA-256(image[0..imagelen)).
 * Returns 0. */
int embdbg_emit_object(const unsigned char *obj, long objlen, long addr_bias,
                       const unsigned char *image, long imagelen,
                       const char *out);

#endif
