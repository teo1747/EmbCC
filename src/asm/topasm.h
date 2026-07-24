#ifndef EMBCC_TOPASM_H
#define EMBCC_TOPASM_H

#include "../parse/ast.h"

/* Assemble a file-scope __asm__ block into ta->code / syms / rels. The
 * supported vocabulary is exactly what EmbLinkOS's crt0 _start stub needs:
 * .global/.globl, labels (named + numeric-local), `and $imm,%reg`, `call
 * sym`, `jmp local-label`, and `ret`. Anything else is refused loudly. */
void topasm_assemble(struct topasm *ta);

#endif
