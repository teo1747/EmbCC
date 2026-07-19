/* EmbIR at M1: a linear three-address form over virtual registers, all
 * of type int (ARCHITECTURE §3 — "the simplest thing that lets codegen
 * be written without lying"). No SSA, no passes, no blocks yet: the M1
 * subset has no control flow, so a function is one straight line.
 *
 * vreg numbering: [0, nparams) are the parameters, then locals in
 * declaration order, then expression temporaries.
 */
#ifndef EMBCC_IR_IR_H
#define EMBCC_IR_IR_H

#include "../parse/ast.h"

enum ir_op {
    IR_CONST, /* dst = imm */
    IR_MOV,   /* dst = a */
    IR_ADD,   /* dst = a + b */
    IR_SUB,   /* dst = a - b */
    IR_MUL,   /* dst = a * b */
    IR_CALL,  /* dst = callee(args...) */
    IR_RET    /* return a */
};

struct ir_ins {
    enum ir_op op;
    int dst, a, b;
    long imm;                /* IR_CONST */
    struct func *callee;     /* IR_CALL */
    int args[MAX_PARAMS];    /* IR_CALL: argument vregs */
    int nargs;
};

struct ir_func {
    struct func *src;        /* name, linkage, code_off/len live here */
    int nvregs;
    struct ir_ins *ins;
    int nins, cap;
};

struct ir_unit {
    struct unit *src;
    struct ir_func *funcs;   /* array, same order as src->funcs */
    int nfuncs;
};

struct ir_unit *irgen(struct unit *u);

#endif
