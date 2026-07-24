/* EmbDBG core, the slice EmbLD reuses: turn a relocatable debug object into a
 * native .embdbg with absolute addresses, at link time. Compiling embdbg.c
 * with -DEMBDBG_NO_MAIN drops its CLI main and leaves this one entry point, so
 * the linker and the standalone tool share one implementation of the format
 * (no second writer — the "one reader, one dumper" discipline). */
#ifndef EMBCC_TOOLS_EMBDBG_CORE_H
#define EMBCC_TOOLS_EMBDBG_CORE_H

/* Merge the DWARF of n debug objects — each biased by biases[i] (its final
 * .text vaddr, so .text-relative addresses become absolute) — into one model
 * and write `out` as a .embdbg whose build_id is SHA-256(image[0..imagelen)).
 * n may be 1. Returns 0. */
int embdbg_emit_objects(const unsigned char **objs, const long *lens,
                        const long *biases, int n,
                        const unsigned char *image, long imagelen,
                        const char *out);

#endif
