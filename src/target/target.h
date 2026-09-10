/* The target machine EmbCC emits for.
 *
 * EmbLinkOS is two architectures now (myos/docs/ARM64.md), so the compiler
 * is too. One process compiles for one target, chosen by --target= and
 * fixed before the front-end runs: there is no per-function or per-file
 * switching, and nothing below reads the HOST architecture for any reason.
 *
 * Only the phases that genuinely differ consult this — the lexer, parser
 * and most of sema are machine-neutral and must stay that way.
 */
#ifndef EMBCC_TARGET_TARGET_H
#define EMBCC_TARGET_TARGET_H

enum target_arch {
    TARGET_X86_64 = 0,
    TARGET_AARCH64 = 1
};

/* The selected target. Defaults to x86_64 so every existing command line
 * keeps its meaning; --target= is the only thing that changes it. */
enum target_arch target_get(void);
void target_set(enum target_arch a);

/* Accepts the triples EmbLinkOS actually builds with — "x86_64-elf" and
 * "aarch64-elf", plus the aliases gcc answers to ("aarch64", "arm64",
 * "aarch64-none-elf"). Returns 0 and leaves *out alone on anything else,
 * so the driver can refuse loudly rather than silently emit for the
 * wrong machine (THE RULE). */
int target_from_triple(const char *triple, enum target_arch *out);

/* The canonical triple, for --version and diagnostics. */
const char *target_triple(enum target_arch a);

/* Machine-neutral relocation kinds.
 *
 * Codegen records a KIND at each patch site and the driver turns
 * (kind, target) into an ELF relocation type. The indirection exists
 * because the same source-level act costs a different number of
 * relocations per machine: taking a symbol's address is one RIP-relative
 * `lea` on x86-64, but an `adrp`/`add` PAIR on aarch64 — two sites, two
 * relocations, one address.
 */
enum reloc_kind {
    RK_CALL,      /* direct call to a function symbol */
    RK_PCREL32,   /* x86-64: the rel32 field of a RIP-relative lea */
    RK_ADR_HI21,  /* aarch64: adrp's 21-bit page-relative field */
    RK_ADD_LO12,  /* aarch64: the paired add's 12-bit in-page field */
    RK_ABS64      /* an absolute 64-bit pointer slot in .data */
};

/* The ELF relocation type for this kind on this target, or -1 if the kind
 * does not apply to it (which is a codegen bug, not an input error). */
int target_reloc_type(enum target_arch a, enum reloc_kind k);

/* The addend the kind carries. x86-64's PC-relative fields are measured
 * from the END of the instruction, so they bias by -4; aarch64's are
 * measured from the instruction itself and bias by 0. `bias` is the
 * site-specific part (a string's offset into .rodata, say). */
long target_reloc_addend(enum target_arch a, enum reloc_kind k, long bias);

/* ELF e_machine. */
int target_elf_machine(enum target_arch a);

#endif
