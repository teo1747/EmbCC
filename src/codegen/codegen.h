#ifndef EMBCC_CODEGEN_CODEGEN_H
#define EMBCC_CODEGEN_CODEGEN_H

#include "../asm/emit.h"
#include "../ir/ir.h"

/* Calls to functions with no definition in this unit cannot be
 * resolved here — each becomes a relocation the driver hands to the
 * ELF writer (R_X86_64_PLT32 against the callee's UNDEF symbol). */
struct extcall {
    int patch_off;        /* offset of the rel32 field in .text */
    struct func *callee;  /* canonical, !has_defn */
};

/* String-address sites: lea rax,[rip+rel32] whose rel32 the linker
 * must point into .rodata — one R_X86_64_PC32 against the .rodata
 * section symbol per site (addend = str_off - 4). */
struct strsite {
    int patch_off;        /* offset of the rel32 field in .text */
    int str_off;          /* target offset inside .rodata */
};

/* Global-variable address sites: lea rax,[rip+rel32], one
 * R_X86_64_PC32 against the global's own symbol (addend -4). */
struct gsite {
    int patch_off;
    struct global *glob;
};

/* Function-address sites (&f / passing f): lea rax,[rip+rel32] with an
 * R_X86_64_PC32 against the function's symbol. */
struct fsite {
    int patch_off;
    struct func *target;
};

/* Lowers the unit to x86-64 into one .text image and fills each
 * func's code_off/code_len. Intra-unit calls are resolved here (rel32
 * patched once all functions are placed); external call sites are
 * returned via ext and next for the driver to relocate. The array is
 * malloc'd; caller frees. */
/* want_debug (from -g) turns on collection of each func's (offset,line)
 * line table (ir_func.lines); off, output is byte-for-byte as before. */
void codegen_unit(struct ir_unit *iu, struct code *text,
                  struct extcall **ext, int *next,
                  struct strsite **strs, int *nstrs,
                  struct gsite **gs, int *ngs,
                  struct fsite **fs, int *nfs, int want_debug, int optimize);

#endif
