/* EmbIR: a linear three-address form over virtual registers, all of
 * type int (ARCHITECTURE §3 — "the simplest thing that lets codegen be
 * written without lying"). Control flow is labels and conditional
 * branches; short-circuit && and || are lowered to branches here, so
 * codegen never sees them. Still no SSA and no passes — that revision
 * comes with the optimizer (VISION_LONGTERM), not before.
 *
 * vreg numbering: [0, nparams) are the parameters, then locals in
 * declaration order, then expression temporaries.
 */
#ifndef EMBCC_IR_IR_H
#define EMBCC_IR_IR_H

#include "../parse/ast.h"

/* Width/representation model (see sema/type.h): temporaries hold
 * promoted values — `w` is 4 (int class) or 8 (long/pointer class) and
 * selects 32- vs 64-bit operations. Variables live in memory at their
 * true `size` (1/2/4/8); LDVAR/LOAD extend on the way in (per `sign`),
 * STVAR/STORE truncate on the way out. `sign` also picks signed vs
 * unsigned division, shift, and comparison. */
enum ir_op {
    IR_CONST, /* dst = imm            (w) */
    IR_MOV,   /* dst = a              (full temp-to-temp copy) */
    IR_ADD,   /* dst = a + b          (w) */
    IR_SUB,   /* dst = a - b          (w) */
    IR_MUL,   /* dst = a * b          (w) */
    IR_DIV,   /* dst = a / b          (w, sign: idiv/div) */
    IR_MOD,   /* dst = a % b          (w, sign) */
    IR_AND,   /* dst = a & b          (w) */
    IR_OR,    /* dst = a | b          (w) */
    IR_XOR,   /* dst = a ^ b          (w) */
    IR_SHL,   /* dst = a << b         (w) */
    IR_SHR,   /* dst = a >> b         (w, sign: sar/shr) */
    IR_NEG,   /* dst = -a             (w) */
    IR_BNOT,  /* dst = ~a             (w) */
    IR_CMP,   /* dst = (a pred b) 0/1 (w, sign; pred is B_EQ..B_GE) */
    IR_LDVAR, /* dst = var a          (size, sign, w: extend) */
    IR_STVAR, /* var dst = a          (size: truncating store) */
    IR_ADDR,  /* dst = &var a         (always w=8) */
    IR_STRADDR, /* dst = &.rodata string (label = string index) */
    IR_LOAD,  /* dst = *(temp a)      (size, sign, w: extend) */
    IR_STORE, /* *(temp a) = b        (size) */
    IR_EXT,   /* dst = a re-extended  (size, sign: from; w: to) */
    IR_CALL,  /* dst = callee(args...) */
    IR_RET,   /* return a (a == -1: void return) */
    IR_LABEL, /* label: (id in `label`) */
    IR_JMP,   /* goto label */
    IR_BRZ    /* if (a == 0) goto label  (w) */
};

struct ir_ins {
    enum ir_op op;
    int dst, a, b;
    int w;                   /* 4 or 8: operation width class */
    int size;                /* 1/2/4/8: memory width for LD/ST/EXT */
    int sign;                /* signed variant of the op */
    long imm;                /* IR_CONST */
    enum binop pred;         /* IR_CMP */
    int label;               /* IR_LABEL/IR_JMP/IR_BRZ */
    struct func *callee;     /* IR_CALL */
    int args[MAX_PARAMS];    /* IR_CALL: argument vregs */
    int nargs;
};

struct ir_func {
    struct func *src;        /* name, linkage, code_off/len live here */
    int nvregs;
    int nlabels;
    struct ir_ins *ins;
    int nins, cap;
};

/* One .rodata string; offsets are assigned sequentially at collection
 * time and become section offsets verbatim in the driver. */
struct ir_str {
    const char *bytes;
    int len;                 /* including the terminating NUL */
    int off;                 /* offset inside .rodata */
};

struct ir_unit {
    struct unit *src;
    struct ir_func *funcs;   /* array, same order as src->funcs */
    int nfuncs;
    struct ir_str *strs;
    int nstrs, capstrs;
    int rodata_len;
};

struct ir_unit *irgen(struct unit *u);

#endif
