/* IR → AArch64, AAPCS64 (ARCHITECTURE §4; myos/docs/ARM64.md for the OS
 * side of the contract).
 *
 * Deliberately naive, exactly as the x86-64 backend was at the same stage:
 * every vreg lives in a stack slot and every operation goes through x9
 * (with x10 for a second operand). Correct-and-slow first — the residency
 * cache, slot coalescing and the register allocator on the x86 side are all
 * later work, and none of them changes what this file must get right.
 *
 * Two things differ from the x86 backend by necessity rather than taste:
 *
 *  - Slots are addressed [sp, #off] with a NON-NEGATIVE off, not [rbp-N].
 *    The scaled 12-bit unsigned-offset load reaches 32 KiB from sp where
 *    the signed form reaches only ±256 from x29, so sp-relative addressing
 *    is what keeps ordinary frames to one instruction per access.
 *
 *  - Argument placement is recomputed here to AAPCS64 rather than read
 *    from ir_arg's on_stack/stk_off, which irgen fills in with the SysV
 *    classification. The two ABIs disagree (8 integer argument registers
 *    against 6, composites by value on the stack rather than by MEMORY
 *    class), and honouring the wrong one is a silent miscompile.
 *
 * Anything not yet lowered fails loudly (THE RULE): floating point, the
 * atomics, va_start, inline asm, computed goto and -g. None of them is
 * miscompiled quietly.
 */
#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>

#include "../asm/emit_arm64.h"
#include "../driver/util.h"

/* -mgeneral-regs-only / -mno-sse: the FP registers are off limits. */
static int g_no_fp;

/* ---- site accumulation ---------------------------------------------- */

struct a64_callsite {
    int patch_off;          /* offset of the bl word in .text */
    struct func *target;
};

struct a64_sites {
    struct a64_callsite *call;
    int ncall, capcall;
    struct extcall *ext;
    int next, capext;
    struct strsite *str;
    int nstr, capstr;
    struct gsite *g;
    int ng, capg;
    struct fsite *f;
    int nf, capf;
};

#define PUSH(arr, n, cap, item)                                          \
    do {                                                                 \
        if ((n) == (cap)) {                                              \
            (cap) = (cap) ? (cap) * 2 : 16;                              \
            (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr));      \
        }                                                                \
        (arr)[(n)++] = (item);                                           \
    } while (0)

/* ---- AAPCS64 argument placement -------------------------------------- */

/* Where one outgoing argument goes. */
struct a64_argplan {
    int in_reg;             /* 1: registers x[reg]..x[reg+nreg-1] */
    int is_flt;             /* ... or v[reg], counted separately (NSRN) */
    int reg, nreg;
    long stk_off;           /* byte offset in the outgoing area otherwise */
    int size;               /* bytes; for a struct, the whole aggregate */
};

/* AAPCS64 §6.4.2 stage C, integer side only.
 *
 * A scalar takes one register; a composite of 16 bytes or fewer takes as
 * many consecutive registers as it has eightbytes; anything larger, or
 * anything that no longer fits, is copied to the stack. Rule C.11 matters:
 * once ANY argument has gone to the stack the register file is declared
 * exhausted, so later small arguments do not back-fill the gap.
 *
 * Returns the size of the outgoing stack area the call needs.
 */
static long plan_call_args(struct ir_ins *i, struct a64_argplan *pl,
                           struct func *fn_for_diag)
{
    int ngrn = 0, nsrn = 0;
    long nsaa = 0;

    for (int k = 0; k < i->nargs; k++) {
        struct ir_arg *a = &i->argv[k];
        int size = a->size > 0 ? a->size : 8;
        pl[k].is_flt = 0;

        /* A struct of floats is an AAPCS64 Homogeneous Floating-point
         * Aggregate, passed in up to four v registers by a rule with no
         * SysV counterpart. irgen's classification cannot express it, so
         * refuse rather than guess. */
        if (a->is_struct)
            for (int q = 0; q < a->nclass; q++)
                if (a->cls[q] == CLASS_SSE)
                    diag_fatal(fn_for_diag->file, fn_for_diag->line,
                               "passing a struct containing floating-point "
                               "members is not supported for aarch64 yet: it "
                               "is an AAPCS64 HFA (in a call from '%s')",
                               fn_for_diag->name);

        /* Scalar floats walk v0..v7 on their own counter (NSRN), exactly as
         * integers walk x0..x7 on NGRN. */
        if (!a->is_struct && a->cls[0] == CLASS_SSE) {
            if (nsrn < 8) {
                pl[k].in_reg = 1;
                pl[k].is_flt = 1;
                pl[k].reg = nsrn++;
                pl[k].nreg = 1;
                pl[k].size = size;
                continue;
            }
            nsaa = (nsaa + 7) & ~7L;
            pl[k].in_reg = 0;
            pl[k].stk_off = nsaa;
            pl[k].size = size;
            pl[k].nreg = 0;
            pl[k].reg = 0;
            nsaa += 8;
            continue;
        }

        int nslot = a->is_struct ? (size + 7) / 8 : 1;
        int fits = a->is_struct ? (size <= 16 && ngrn + nslot <= 8)
                                : (ngrn < 8);
        if (fits) {
            pl[k].in_reg = 1;
            pl[k].reg = ngrn;
            pl[k].nreg = nslot;
            pl[k].size = size;
            ngrn += nslot;
            continue;
        }
        /* C.11: the register file is spent for every argument after this. */
        ngrn = 8;
        pl[k].in_reg = 0;
        /* Each stack argument is 8-byte aligned. A composite whose own
         * alignment is 16 should align to 16; ir_arg does not carry the
         * type's alignment, so that case (a struct with a 16-byte-aligned
         * member passed by value) is not yet handled. */
        nsaa = (nsaa + 7) & ~7L;
        pl[k].stk_off = nsaa;
        pl[k].size = size;
        pl[k].nreg = 0;
        pl[k].reg = 0;
        nsaa += (size + 7) & ~7;
    }
    return nsaa;
}

/* ---- frame layout ---------------------------------------------------- */

/* Slot displacements, all non-negative byte offsets from sp after the
 * prologue. Low to high: the outgoing argument area, the struct-return
 * scratch, the hidden return pointer, the variables, then the temporaries.
 * Returns the per-vreg table (caller frees).
 */
static long *layout_frame(struct ir_func *fn, int *frame_out,
                          long *outgoing_out, long *scratch_base_out,
                          long *sret_slot_out)
{
    struct func *f = fn->src;
    long *disp = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1) * sizeof *disp);

    /* The outgoing area must hold the widest stack-argument list of any
     * call in the function, measured with AAPCS64 rather than taken from
     * irgen's SysV-derived outgoing_bytes. */
    long outgoing = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (fn->ins[n].op != IR_CALL)
            continue;
        struct a64_argplan pl[MAX_PARAMS];
        long need = plan_call_args(&fn->ins[n], pl, f);
        if (need > outgoing)
            outgoing = need;
    }
    outgoing = (outgoing + 15) & ~15L;

    long running = outgoing;
    *outgoing_out = outgoing;

    *scratch_base_out = running;
    running += fn->scratch_bytes;

    /* A function returning a composite larger than 16 bytes is handed the
     * caller's buffer in x8; it must survive to the return, so it gets a
     * slot rather than staying in a caller-saved register. */
    *sret_slot_out = -1;
    if (f->ret_ty->kind == TY_STRUCT && ty_size(f->ret_ty) > 16) {
        running = (running + 7) & ~7L;
        *sret_slot_out = running;
        running += 8;
    }

    /* Variables, each at its own size and alignment. Unlike the x86
     * backend these are not coalesced by scope yet, so frames are wider
     * than they need to be. */
    for (int v = 0; v < f->nvars; v++) {
        int al = f->var_aligns ? f->var_aligns[v] : 0;
        int tal = ty_align(f->var_tys[v]);
        if (tal > al) al = tal;
        if (al > 16)
            diag_fatal(f->file, f->line,
                       "a local in '%s' needs %d-byte alignment, exceeding "
                       "the 16-byte stack alignment EmbCC can guarantee",
                       f->name, al);
        if (al > 1)
            running = (running + al - 1) & ~((long)al - 1);
        disp[v] = running;
        running += (ty_size(f->var_tys[v]) + 7) & ~7;
    }

    /* Temporaries: eight bytes each, one slot apiece. */
    running = (running + 7) & ~7L;
    for (int t = f->nvars; t < fn->nvregs; t++) {
        disp[t] = running;
        running += 8;
    }

    *frame_out = (int)((running + 15) & ~15L);
    return disp;
}

/* ---- helpers --------------------------------------------------------- */

static void align16(struct code *t)
{
    while (t->len & 15)
        a64_word(t, 0xD503201FUL);       /* nop */
}

/* Load vreg's slot into `reg`, extending per size/sign into a w-wide value. */
static void ld_slot(struct code *t, const long *sd, int vreg, int reg,
                    int size, int sign, int w)
{
    a64_ldr(t, reg, A64_SP, sd[vreg], size, sign, w);
}

/* Store `reg`'s low `size` bytes into vreg's slot. */
static void st_slot(struct code *t, const long *sd, int vreg, int reg,
                    int size)
{
    a64_str(t, reg, A64_SP, sd[vreg], size);
}

/* Materialise operand b into A64_TMP, whether it is a vreg or a folded
 * immediate (the optimizer's imm_b). */
static void operand_b(struct code *t, const long *sd, struct ir_ins *i)
{
    if (i->imm_b)
        a64_mov_imm(t, A64_TMP, i->imm, i->w);
    else
        ld_slot(t, sd, i->b, A64_TMP, 8, 0, 8);
}

/* dst = src + off, where src may be sp. */
static void addr_of(struct code *t, int dst, int base, long off)
{
    if (!a64_add_imm(t, dst, base, off, 8)) {
        a64_mov_imm(t, A64_SCR, off, 8);
        if (base == A64_SP)
            a64_word(t, 0x8B2063E0UL | ((unsigned long)A64_SCR << 16) |
                        (unsigned long)dst);      /* add dst, sp, scr */
        else
            a64_alu_reg(t, '+', dst, base, A64_SCR, 8);
    }
}

/* Copy `size` bytes from [src] to [dst]. Unrolled: struct copies in real
 * code are small, and an unrolled copy needs no spare register for a
 * counter. A very large aggregate therefore costs a long instruction run
 * — a bulk-copy loop is a later optimisation, not a correctness gap. */
static void emit_copy(struct code *t, int dst, int src, int size)
{
    int off = 0;
    while (off < size) {
        int chunk = size - off >= 8 ? 8 : size - off >= 4 ? 4
                  : size - off >= 2 ? 2 : 1;
        a64_ldr(t, A64_ACC, src, off, chunk, 0, 8);
        a64_str(t, A64_ACC, dst, off, chunk);
        off += chunk;
    }
}

static void emit_zero(struct code *t, int dst, int size)
{
    int off = 0;
    while (off < size) {
        int chunk = size - off >= 8 ? 8 : size - off >= 4 ? 4
                  : size - off >= 2 ? 2 : 1;
        a64_str(t, A64_ZR, dst, off, chunk);
        off += chunk;
    }
}

/* A floating-point binary operation: both operands come from their slots
 * into the FP scratch pair, and the result goes back to the destination
 * slot at the operation's own width. */
static void fbin(struct code *t, const long *sd, struct ir_ins *i, int op)
{
    a64_fldr(t, A64_FACC, A64_SP, sd[i->a], i->w);
    a64_fldr(t, A64_FTMP, A64_SP, sd[i->b], i->w);
    a64_falu(t, op, A64_FACC, A64_FACC, A64_FTMP, i->w);
    a64_fstr(t, A64_FACC, A64_SP, sd[i->dst], i->w);
}

/* The condition code for an IR comparison predicate. */
static int cond_for(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return A64_EQ;
    case B_NE: return A64_NE;
    case B_LT: return sign ? A64_LT : A64_CC;   /* cc == lo */
    case B_LE: return sign ? A64_LE : A64_LS;
    case B_GT: return sign ? A64_GT : A64_HI;
    case B_GE: return sign ? A64_GE : A64_CS;   /* cs == hs */
    default:   return A64_AL;
    }
}

/* Branch fixups within one function. */
/* kind: 26-bit branch, 19-bit conditional branch, or a 21-bit adr. */
enum a64_fixkind { FIX_B26, FIX_B19, FIX_ADR };
struct a64_fix { int at; int label; enum a64_fixkind kind; };

/* ---- one function ---------------------------------------------------- */

static void gen_func(struct ir_func *fn, struct code *t, struct a64_sites *st,
                     int want_debug)
{
    struct func *f = fn->src;

    if (want_debug)
        diag_fatal(f->file, f->line,
                   "-g is not supported for aarch64 yet: DWARF locations "
                   "would describe x86 frame offsets");

    int frame;
    long outgoing, scratch_base, sret_slot;
    long *sd = layout_frame(fn, &frame, &outgoing, &scratch_base, &sret_slot);

    align16(t);
    f->code_off = t->len;

    a64_prologue(t, frame);

    /* Incoming parameters: AAPCS64 places them the same way plan_call_args
     * places outgoing ones, so the two must agree exactly. Arguments that
     * arrived on the caller's stack sit above the frame we just built:
     * x29 points at the saved x29/x30 pair, so the caller's area begins at
     * x29+16. */
    {
        int ngrn = 0, nsrn = 0;
        long incoming = 16;
        if (sret_slot >= 0)
            a64_str(t, A64_SRET, A64_SP, sret_slot, 8);
        for (int p = 0; p < f->nparams; p++) {
            struct type *pt = f->param_tys[p];
            int size = ty_size(pt);
            if (ty_is_float(pt)) {
                if (nsrn < 8) {
                    a64_fstr(t, nsrn++, A64_SP, sd[p], size);
                } else {
                    /* Arrived on the caller's stack; the bits move through
                     * an integer register, which is all a copy needs. */
                    a64_ldr(t, A64_ACC, A64_FP, incoming, 8, 0, 8);
                    st_slot(t, sd, p, A64_ACC, size);
                    incoming += 8;
                }
                continue;
            }
            if (pt->kind == TY_STRUCT) {
                int nslot = (size + 7) / 8;
                if (size <= 16 && ngrn + nslot <= 8) {
                    for (int q = 0; q < nslot; q++)
                        a64_str(t, ngrn + q, A64_SP, sd[p] + q * 8, 8);
                    ngrn += nslot;
                } else {
                    ngrn = 8;
                    /* Copy it out of the caller's frame so its address is
                     * an ordinary local. */
                    addr_of(t, A64_ADDR, A64_SP, sd[p]);
                    addr_of(t, A64_TMP, A64_FP, incoming);
                    emit_copy(t, A64_ADDR, A64_TMP, size);
                    incoming += (size + 7) & ~7;
                }
                continue;
            }
            if (ngrn < 8) {
                a64_str(t, ngrn++, A64_SP, sd[p], size > 8 ? 8 : size);
            } else {
                a64_ldr(t, A64_ACC, A64_FP, incoming, 8, 0, 8);
                st_slot(t, sd, p, A64_ACC, size > 8 ? 8 : size);
                incoming += 8;
            }
        }
    }

    /* Label offsets and the branches waiting on them. */
    int *loff = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *loff);
    for (int l = 0; l < fn->nlabels; l++)
        loff[l] = -1;
    struct a64_fix *fix = NULL;
    int nfix = 0, capfix = 0;
    /* Every `return` jumps to the single epilogue at the end. */
    int *retfix = NULL;
    int nret = 0, capret = 0;

    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];

        /* -mgeneral-regs-only (the kernel's mode) means the FP registers
         * may not be touched at all: on aarch64 they trap until
         * CPACR_EL1.FPEN is set, exactly as SSE does before CR4.OSFXSR. */
        if (g_no_fp && (i->flt || i->op == IR_I2F || i->op == IR_F2I ||
                        i->op == IR_F2F))
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "floating point used under -mgeneral-regs-only "
                       "(in '%s')", f->name);

        switch (i->op) {
        case IR_CONST:
            a64_mov_imm(t, A64_ACC, i->imm, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_MOV:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
            if (i->flt) {
                /* Only +, - and * ever reach here as floats; the bitwise
                 * operators have no floating-point form in C. */
                fbin(t, sd, i, i->op == IR_ADD ? '+' : '-');
                break;
            }
            int op = i->op == IR_ADD ? '+' : i->op == IR_SUB ? '-'
                   : i->op == IR_AND ? '&' : i->op == IR_OR  ? '|' : '^';
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_alu_reg(t, op, A64_ACC, A64_ACC, A64_TMP, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_MUL:
            if (i->flt) { fbin(t, sd, i, '*'); break; }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_mul(t, A64_ACC, A64_ACC, A64_TMP, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_DIV:
            if (i->flt) { fbin(t, sd, i, '/'); break; }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_div(t, A64_ACC, A64_ACC, A64_TMP, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_MOD:
            /* q = a / b ; r = a - q*b. msub does the second half. */
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_div(t, A64_ADDR, A64_ACC, A64_TMP, i->sign, i->w);
            a64_msub(t, A64_ACC, A64_ADDR, A64_TMP, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_SHL: case IR_SHR:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_shift_reg(t, i->op == IR_SHL ? '<' : (i->sign ? '>' : 'u'),
                          A64_ACC, A64_ACC, A64_TMP, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_NEG:
            if (i->flt) {
                a64_fldr(t, A64_FACC, A64_SP, sd[i->a], i->w);
                a64_fneg(t, A64_FACC, A64_FACC, i->w);
                a64_fstr(t, A64_FACC, A64_SP, sd[i->dst], i->w);
                break;
            }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_neg(t, A64_ACC, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_BNOT:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_mvn(t, A64_ACC, A64_ACC, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_CMP:
            if (i->flt) {
                /* FCMP leaves NZCV "unordered" (N=0 Z=0 C=1 V=1) when either
                 * operand is NaN, which is why LT and LE need mi/ls rather
                 * than lt/le: lt tests N!=V and would read TRUE on a NaN,
                 * where C requires every ordered comparison against NaN to
                 * be false. eq/ne/gt/ge already fall out correctly. */
                a64_fldr(t, A64_FACC, A64_SP, sd[i->a], i->w);
                a64_fldr(t, A64_FTMP, A64_SP, sd[i->b], i->w);
                a64_fcmp(t, A64_FACC, A64_FTMP, i->w);
                int cc = i->pred == B_EQ ? A64_EQ : i->pred == B_NE ? A64_NE
                       : i->pred == B_LT ? A64_MI : i->pred == B_LE ? A64_LS
                       : i->pred == B_GT ? A64_GT : A64_GE;
                a64_cset(t, A64_ACC, cc);
                st_slot(t, sd, i->dst, A64_ACC, 8);
                break;
            }
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            operand_b(t, sd, i);
            a64_cmp_reg(t, A64_ACC, A64_TMP, i->w);
            a64_cset(t, A64_ACC, cond_for(i->pred, i->sign));
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_LDVAR:
            ld_slot(t, sd, i->a, A64_ACC, i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_STVAR:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            st_slot(t, sd, i->dst, A64_ACC, i->size);
            break;

        case IR_ADDR:
            addr_of(t, A64_ACC, A64_SP, sd[i->a]);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_STRADDR: {
            struct strsite hi, lo;
            hi.patch_off = a64_adrp(t, A64_ACC);
            hi.str_off = i->label;          /* resolved to an offset below */
            hi.kind = RK_ADR_HI21;
            PUSH(st->str, st->nstr, st->capstr, hi);
            lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
            lo.str_off = i->label;
            lo.kind = RK_ADD_LO12;
            PUSH(st->str, st->nstr, st->capstr, lo);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_GADDR: {
            struct gsite hi, lo;
            hi.patch_off = a64_adrp(t, A64_ACC);
            hi.glob = i->glob;
            hi.kind = RK_ADR_HI21;
            PUSH(st->g, st->ng, st->capg, hi);
            lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
            lo.glob = i->glob;
            lo.kind = RK_ADD_LO12;
            PUSH(st->g, st->ng, st->capg, lo);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_FADDR: {
            struct fsite hi, lo;
            hi.patch_off = a64_adrp(t, A64_ACC);
            hi.target = i->callee;
            hi.kind = RK_ADR_HI21;
            PUSH(st->f, st->nf, st->capf, hi);
            lo.patch_off = a64_add_lo12(t, A64_ACC, A64_ACC);
            lo.target = i->callee;
            lo.kind = RK_ADD_LO12;
            PUSH(st->f, st->nf, st->capf, lo);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }

        case IR_LOAD:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            a64_ldr(t, A64_ACC, A64_ADDR, 0, i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_STORE:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            ld_slot(t, sd, i->b, A64_ACC, 8, 0, 8);
            a64_str(t, A64_ACC, A64_ADDR, 0, i->size);
            break;

        case IR_EXT:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_extend(t, A64_ACC, A64_ACC, i->size, i->sign, i->w);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_BSWAP:
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            a64_rev(t, A64_ACC, A64_ACC, i->size);
            if (i->size == 2)                 /* rev16 leaves the top bits */
                a64_extend(t, A64_ACC, A64_ACC, 2, 0, 8);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;

        case IR_MEMCPY:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            ld_slot(t, sd, i->b, A64_SCR, 8, 0, 8);
            emit_copy(t, A64_ADDR, A64_SCR, i->size);
            break;

        case IR_MEMZERO:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            emit_zero(t, A64_ADDR, i->size);
            break;

        case IR_FENCE:
            a64_dmb_ish(t);
            break;

        case IR_UD2:
            a64_udf(t);
            break;

        case IR_LABEL:
            loff[i->label] = t->len;
            break;

        case IR_JMP: {
            struct a64_fix fx;
            fx.at = a64_b(t); fx.label = i->label; fx.kind = FIX_B26;
            PUSH(fix, nfix, capfix, fx);
            break;
        }

        case IR_BRZ: case IR_BRNZ: {
            ld_slot(t, sd, i->a, A64_ACC, 8, 0, 8);
            struct a64_fix fx;
            fx.at = a64_cbz(t, A64_ACC, i->op == IR_BRNZ, i->w);
            fx.label = i->label; fx.kind = FIX_B19;
            PUSH(fix, nfix, capfix, fx);
            break;
        }

        case IR_RET: {
            if (i->a >= 0) {
                struct type *rt = f->ret_ty;
                if (i->flt) {
                    a64_fldr(t, 0, A64_SP, sd[i->a], i->w);
                } else if (rt->kind == TY_STRUCT) {
                    int size = ty_size(rt);
                    /* The value's ADDRESS is in the operand slot. Small
                     * composites return in x0/x1; a large one is copied
                     * through the hidden pointer the caller supplied. */
                    ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
                    if (size <= 16) {
                        for (int q = 0; q * 8 < size; q++)
                            a64_ldr(t, q, A64_ADDR, q * 8, 8, 0, 8);
                    } else {
                        a64_ldr(t, A64_SCR, A64_SP, sret_slot, 8, 0, 8);
                        emit_copy(t, A64_SCR, A64_ADDR, size);
                        a64_ldr(t, 0, A64_SP, sret_slot, 8, 0, 8);
                    }
                } else {
                    ld_slot(t, sd, i->a, 0, 8, 0, 8);
                }
            }
            struct a64_fix fx;
            fx.at = a64_b(t); fx.label = -1; fx.kind = FIX_B26;
            PUSH(retfix, nret, capret, fx.at);
            break;
        }

        case IR_CALL: {
            struct a64_argplan pl[MAX_PARAMS];
            plan_call_args(i, pl, f);

            /* Stack arguments first: placing them uses the scratch
             * registers, which must not run over an argument register that
             * has already been loaded. */
            for (int k = 0; k < i->nargs; k++) {
                if (pl[k].in_reg)
                    continue;
                struct ir_arg *a = &i->argv[k];
                if (a->is_struct) {
                    /* Destination FIRST: addr_of falls back to A64_SCR for
                     * an offset the add immediate cannot hold, which would
                     * clobber the source address if that were already in it. */
                    addr_of(t, A64_ADDR, A64_SP, pl[k].stk_off);
                    ld_slot(t, sd, a->vreg, A64_SCR, 8, 0, 8);
                    emit_copy(t, A64_ADDR, A64_SCR, pl[k].size);
                } else {
                    ld_slot(t, sd, a->vreg, A64_ACC, 8, 0, 8);
                    a64_str(t, A64_ACC, A64_SP, pl[k].stk_off, 8);
                }
            }
            /* Then the register arguments, in order. Each writes only its
             * own x register, so no shuffle is needed. */
            for (int k = 0; k < i->nargs; k++) {
                if (!pl[k].in_reg)
                    continue;
                struct ir_arg *a = &i->argv[k];
                if (pl[k].is_flt) {
                    a64_fldr(t, pl[k].reg, A64_SP, sd[a->vreg], pl[k].size);
                } else if (a->is_struct) {
                    ld_slot(t, sd, a->vreg, A64_ADDR, 8, 0, 8);
                    for (int q = 0; q < pl[k].nreg; q++)
                        a64_ldr(t, pl[k].reg + q, A64_ADDR, q * 8, 8, 0, 8);
                } else {
                    ld_slot(t, sd, a->vreg, pl[k].reg, 8, 0, 8);
                }
            }
            /* A composite return larger than 16 bytes: hand the callee the
             * caller-side scratch in x8. */
            if (i->retsize > 16)
                addr_of(t, A64_SRET, A64_SP, scratch_base + i->scratch);

            if (i->indirect) {
                ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
                a64_blr(t, A64_ADDR);
            } else if (i->callee->has_defn) {
                struct a64_callsite cs;
                cs.patch_off = a64_bl(t);
                cs.target = i->callee;
                PUSH(st->call, st->ncall, st->capcall, cs);
            } else {
                struct extcall ec;
                ec.patch_off = a64_bl(t);
                ec.callee = i->callee;
                PUSH(st->ext, st->next, st->capext, ec);
            }

            if (i->dst < 0)
                break;
            if (i->retsize) {
                /* Small composites arrive in x0/x1 and are unpacked into
                 * the scratch; a large one the callee already wrote there.
                 * Either way dst receives the scratch's ADDRESS, which is
                 * the contract irgen and the x86 backend share. */
                if (i->retsize <= 16) {
                    addr_of(t, A64_ADDR, A64_SP, scratch_base + i->scratch);
                    for (int q = 0; q * 8 < i->retsize; q++)
                        a64_str(t, q, A64_ADDR, q * 8, 8);
                }
                addr_of(t, A64_ACC, A64_SP, scratch_base + i->scratch);
                st_slot(t, sd, i->dst, A64_ACC, 8);
            } else if (i->flt) {
                a64_fstr(t, 0, A64_SP, sd[i->dst], i->w);
            } else {
                st_slot(t, sd, i->dst, 0, 8);
            }
            break;
        }

        case IR_I2F: {
            /* size/sign describe the integer SOURCE, w the float result. */
            int iw = i->size >= 8 ? 8 : 4;
            ld_slot(t, sd, i->a, A64_ACC, i->size, i->sign, iw);
            a64_cvt_i2f(t, A64_FACC, A64_ACC, i->sign, iw, i->w);
            a64_fstr(t, A64_FACC, A64_SP, sd[i->dst], i->w);
            break;
        }
        case IR_F2I: {
            /* size is the float source; w/sign the integer result. Unlike
             * SSE, aarch64 has a native unsigned form, so the u64 case needs
             * no fixup sequence. */
            int iw = i->w >= 8 ? 8 : 4;
            a64_fldr(t, A64_FACC, A64_SP, sd[i->a], i->size);
            a64_cvt_f2i(t, A64_ACC, A64_FACC, i->sign, iw, i->size);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_F2F:
            a64_fldr(t, A64_FACC, A64_SP, sd[i->a], i->size);
            a64_fcvt(t, A64_FACC, A64_FACC, i->size, i->w);
            a64_fstr(t, A64_FACC, A64_SP, sd[i->dst], i->w);
            break;
        case IR_VA_START:
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "va_start is not supported for aarch64 yet: the "
                       "AAPCS64 register save area is not built (in '%s')",
                       f->name);
            break;
        case IR_ASM:
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "inline asm is not supported for aarch64 yet: "
                       "EmbCC's assembler is x86-64 NASM syntax (in '%s')",
                       f->name);
            break;
        case IR_XCHG: case IR_XADD: case IR_CMPXCHG:
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "atomic operations are not supported for aarch64 "
                       "yet: they need ldxr/stxr pairs (in '%s')", f->name);
            break;
        case IR_LABELADDR: {
            /* &&label. adr gives the label's RUN-TIME address directly,
             * and its ±1 MiB reach covers any function EmbCC will emit. */
            struct a64_fix fx;
            fx.at = a64_adr(t, A64_ACC);
            fx.label = i->label;
            fx.kind = FIX_ADR;
            PUSH(fix, nfix, capfix, fx);
            st_slot(t, sd, i->dst, A64_ACC, 8);
            break;
        }
        case IR_IGOTO:
            ld_slot(t, sd, i->a, A64_ADDR, 8, 0, 8);
            a64_br(t, A64_ADDR);
            break;
        default:
            diag_fatal(f->file, i->line ? i->line : f->line,
                       "aarch64 codegen: unhandled IR op %d in '%s'",
                       (int)i->op, f->name);
            break;
        }
    }

    /* The single epilogue every `return` branches to. */
    int epi = t->len;
    a64_epilogue(t, frame);

    for (int k = 0; k < nret; k++)
        a64_patch_b26(t, retfix[k], epi);
    for (int k = 0; k < nfix; k++) {
        int target = loff[fix[k].label];
        if (target < 0)
            diag_fatal(f->file, f->line,
                       "aarch64 codegen: branch to undefined label %d in '%s'",
                       fix[k].label, f->name);
        switch (fix[k].kind) {
        case FIX_B26: a64_patch_b26(t, fix[k].at, target); break;
        case FIX_B19: a64_patch_b19(t, fix[k].at, target); break;
        case FIX_ADR: a64_patch_adr(t, fix[k].at, target); break;
        }
    }

    f->code_len = t->len - f->code_off;
    free(loff); free(fix); free(retfix); free(sd);
}

/* ---- the unit -------------------------------------------------------- */

void codegen_unit_arm64(struct ir_unit *iu, struct code *text,
                        struct extcall **ext, int *next,
                        struct strsite **strs, int *nstrs,
                        struct gsite **gs, int *ngs,
                        struct fsite **fs, int *nfs, int want_debug,
                        int optimize, int no_sse, int regalloc)
{
    (void)optimize;   /* the IR arrives already optimized; this backend has
                       * no level-dependent output of its own yet */
    g_no_fp = no_sse;
    (void)regalloc;

    struct a64_sites st;
    st.call = NULL; st.ncall = st.capcall = 0;
    st.ext = NULL;  st.next = st.capext = 0;
    st.str = NULL;  st.nstr = st.capstr = 0;
    st.g = NULL;    st.ng = st.capg = 0;
    st.f = NULL;    st.nf = st.capf = 0;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st, want_debug);

    /* Intra-unit calls resolve here, now that every function is placed. */
    for (int n = 0; n < st.ncall; n++)
        a64_patch_b26(text, st.call[n].patch_off,
                      st.call[n].target->code_off);
    free(st.call);

    /* String sites carried the literal's INDEX; turn it into its .rodata
     * offset now that the pool is final. */
    for (int n = 0; n < st.nstr; n++)
        st.str[n].str_off = iu->strs[st.str[n].str_off].off;

    *ext = st.ext;   *next = st.next;
    *strs = st.str;  *nstrs = st.nstr;
    *gs = st.g;      *ngs = st.ng;
    *fs = st.f;      *nfs = st.nf;
}
