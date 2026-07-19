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

/* Lowers the unit to x86-64 into one .text image and fills each
 * func's code_off/code_len. Intra-unit calls are resolved here (rel32
 * patched once all functions are placed); external call sites are
 * returned via ext and next for the driver to relocate. The array is
 * malloc'd; caller frees. */
void codegen_unit(struct ir_unit *iu, struct code *text,
                  struct extcall **ext, int *next);

#endif
