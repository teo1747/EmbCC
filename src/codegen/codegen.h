#ifndef EMBCC_CODEGEN_CODEGEN_H
#define EMBCC_CODEGEN_CODEGEN_H

#include "../asm/emit.h"
#include "../ir/ir.h"

/* Lowers the unit to x86-64 into one .text image and fills each
 * func's code_off/code_len. Intra-unit calls are resolved here (rel32
 * patched once all functions are placed) — no relocations exist in M1,
 * because the subset cannot reference anything outside the file. */
void codegen_unit(struct ir_unit *iu, struct code *text);

#endif
