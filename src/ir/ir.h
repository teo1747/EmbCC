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
#include "../sema/type.h"

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
    IR_GADDR, /* dst = &global (glob) */
    IR_FADDR, /* dst = &function (callee) */
    IR_LOAD,  /* dst = *(temp a)      (size, sign, w: extend) */
    IR_STORE, /* *(temp a) = b        (size) */
    IR_EXT,   /* dst = a re-extended  (size, sign: from; w: to) */
    IR_I2F,   /* dst = (float)a       (size,sign: int src; w: float dst) */
    IR_F2I,   /* dst = (int)a         (size: float src; w,sign: int dst) */
    IR_F2F,   /* dst = (float)a       (size: src width; w: dst width) */
    IR_CALL,  /* dst = callee(args...); indirect: target fp in a */
    IR_RET,   /* return a (a == -1: void return) */
    IR_LABEL, /* label: (id in `label`) */
    IR_JMP,   /* goto label */
    IR_MEMCPY,/* copy `size` bytes: *(addr a) <- *(addr b) */
    IR_MEMZERO,/* zero `size` bytes at (addr a) */
    IR_BRZ,   /* if (a == 0) goto label  (w) */
    IR_BRNZ,  /* if (a != 0) goto label  (w) */
    IR_VA_START, /* init the va_list whose ADDRESS is in temp a (SysV:
                  * fill a __va_list_tag on the frame, point *a at it) */
    IR_BSWAP, /* dst = byteswap(a)   (size: 2/4/8; __builtin_bswapN) */
    IR_FENCE, /* a full memory barrier (mfence; __sync_synchronize) */
    IR_UD2,   /* the undefined instruction (ud2; __builtin_unreachable) */
    IR_XCHG,  /* dst = *(temp a); *(temp a) = b   (atomic; size) */
    IR_XADD,  /* dst = *(temp a); *(temp a) += b  (lock xadd; size) */
    IR_CMPXCHG, /* CAS at *(a): compare against *(b), set to c on match;
                 * dst = matched?1:0, and *(b) updated to the seen value.
                 * (lock cmpxchg; size) */
    IR_ASM    /* extended asm: load inputs to fixed registers, assemble the
               * template, store outputs. Detail in ir_ins.asm_ir */
};

/* One resolved asm operand: an input carries the temp holding its VALUE, an
 * output the temp holding its lvalue ADDRESS; reg is the fixed register
 * (0-15) the constraint pins it to. */
struct ir_asm_op {
    int temp;
    int reg;
    int size;
};

struct ir_asm {
    const unsigned char *code;   /* assembled template bytes */
    int codelen;
    struct ir_asm_op *in;
    int nin;
    struct ir_asm_op *out;
    int nout;
};

struct ir_ins {
    enum ir_op op;
    int line;                /* source line this instruction lowers from, 0
                              * if none — stamped by irgen, read only by the
                              * -g line-table pass in codegen */
    int dst, a, b;
    int c;                   /* IR_CMPXCHG: the third operand (desired value) */
    int w;                   /* 4 or 8: operation width class */
    int size;                /* 1/2/4/8: memory width for LD/ST/EXT */
    int sign;                /* signed variant of the op */
    int flt;                 /* operate in xmm at width w (SSE scalar) */
    long imm;                /* IR_CONST */
    enum binop pred;         /* IR_CMP */
    int label;               /* IR_LABEL/IR_JMP/IR_BRZ */
    struct func *callee;     /* IR_CALL (direct), IR_FADDR */
    int indirect;            /* IR_CALL through a function pointer */
    int call_varargs;        /* al = 0 needed at the call */
    struct global *glob;     /* IR_GADDR */
    /* IR_CALL arguments. SysV splits the argument REGISTERS by class —
     * integers walk rdi..r9, floats walk xmm0..7, independently — and
     * an aggregate is either taken apart into eightbytes or copied to
     * the stack. Each argument therefore carries its own classification,
     * decided in irgen where the types still exist. */
    struct ir_arg {
        int vreg;            /* value, or the ADDRESS when is_struct */
        int is_struct;
        int size;            /* struct size, or the scalar's width */
        int nclass;          /* eightbyte count; 0 = MEMORY (stack) */
        enum arg_class cls[2];
        int on_stack;        /* no registers left (or MEMORY class) */
        int stk_off;         /* offset in the outgoing area */
    } argv[MAX_PARAMS];
    int nargs;
    /* IR_CALL returning a struct: its size, classification, and the
     * caller-side scratch the result lands in. nclass 0 means MEMORY,
     * i.e. the hidden-pointer (sret) convention. */
    int retsize;
    int retnclass;
    enum arg_class retcls[2];
    int scratch;             /* frame offset of the returned struct */
    struct ir_asm *asm_ir;   /* IR_ASM */
};

/* One line-table row: a .text offset (within this function) maps to a
 * source line. Collected by codegen only under -g; consumed by the DWARF
 * emitter, which brackets each function's rows with set_address/end_sequence
 * using the function's code_off/code_len (on struct func). */
struct ir_line { int off; int line; };

/* -g: a source-level variable (parameter or local). Its storage is the frame
 * slot of vreg `vreg`; irgen records name/vreg/type, codegen fills the slot's
 * rbp-relative offset into ir_func.var_off[vreg], and the DWARF emitter turns
 * the pair into DW_AT_location = DW_OP_fbreg(offset). Statics are excluded —
 * they are globals, not frame storage. */
struct ir_dbgvar { const char *name; int vreg; int is_param; struct type *ty; };

struct ir_func {
    struct func *src;        /* name, linkage, code_off/len live here */
    int nvregs;
    int nlabels;
    int scratch_bytes;       /* struct-return temporaries */
    int outgoing_bytes;      /* widest stack-argument area of any call */
    struct ir_ins *ins;
    int nins, cap;
    struct ir_line *lines;   /* -g: (offset, line) rows in .text order */
    int nlines, linecap;
    struct ir_dbgvar *dbgvars; /* -g: params + locals (irgen) */
    int ndbgvars, dbgvarcap;
    int *var_off;            /* -g: rbp-relative slot offset per vreg (codegen) */
    /* Per-LOCAL lexical scope, as a half-open instruction range [lo, hi) (irgen).
     * Two locals whose scopes are disjoint never coexist — a stack pointer used
     * past its scope is UB — so codegen may give them one stack slot. Params and
     * function-level locals span the whole function; only nested-block locals get
     * a narrower range. Length nvars; unused (NULL) when there are no locals. */
    int *var_scope_lo, *var_scope_hi;
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

/* Intern a string into the unit's .rodata pool (used by the driver to
 * place a global initializer's string targets). Returns its index; the
 * offset is iu->strs[index].off. */
int ir_intern_string(struct ir_unit *iu, const char *bytes, int len);

#endif
