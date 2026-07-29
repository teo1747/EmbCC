/* IR optimizer — local, per-function passes over the single-assignment
 * temporaries of EmbIR (see opt.h). Three passes iterate to a fixpoint:
 * constant folding (+ a few algebraic identities), copy propagation, and
 * dead-code elimination. Each is proven safe by the single-assignment
 * property: a value in a vreg with exactly one definition is invariant, so
 * no control-flow analysis is needed to know it is the same everywhere.
 */
#include "opt.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

/* ---- op classification ---- */

/* Writes a fresh temporary as dst. IR_STVAR writes a LOCAL slot and is
 * handled apart; every other dst-writer produces a single-assignment temp. */
static int writes_temp(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV:
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
    case IR_FADDR: case IR_LOAD: case IR_EXT: case IR_BSWAP:
    case IR_I2F: case IR_F2I: case IR_F2F: case IR_CALL: case IR_XCHG:
    case IR_XADD: case IR_CMPXCHG:
        return 1;
    default:
        return 0;
    }
}

/* No side effect and cannot fault, so removable when its result is unused.
 * DIV/MOD (÷0 traps), LOAD (may fault or be volatile), CALL (effects), and
 * every store/branch/label are deliberately NOT pure. */
static int is_pure(enum ir_op op)
{
    switch (op) {
    case IR_CONST: case IR_MOV:
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_BNOT: case IR_CMP:
    case IR_LDVAR: case IR_ADDR: case IR_STRADDR: case IR_GADDR:
    case IR_FADDR: case IR_EXT: case IR_BSWAP:
    case IR_I2F: case IR_F2I: case IR_F2F:
        return 1;
    default:
        return 0;
    }
}

/* The vreg an instruction assigns (a temp, or a local for IR_STVAR); -1 if
 * it assigns nothing. */
static int def_target(const struct ir_ins *i)
{
    if (i->op == IR_STVAR)
        return i->dst;
    if (writes_temp(i->op))
        return i->dst;
    return -1;
}

/* Visit &field for every vreg this instruction READS (never its dst). */
static void each_read(struct ir_ins *i, void (*cb)(int *, void *), void *ctx)
{
    switch (i->op) {
    case IR_MOV: case IR_NEG: case IR_BNOT:
    case IR_I2F: case IR_F2I: case IR_F2F:
    case IR_EXT: case IR_BSWAP: case IR_LDVAR: case IR_ADDR: case IR_LOAD:
    case IR_MEMZERO: case IR_VA_START: case IR_STVAR:
        cb(&i->a, ctx);
        break;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_CMP: case IR_STORE: case IR_MEMCPY: case IR_XCHG:
    case IR_XADD:
        cb(&i->a, ctx);
        cb(&i->b, ctx);
        break;
    case IR_CMPXCHG:
        cb(&i->a, ctx);
        cb(&i->b, ctx);
        cb(&i->c, ctx);
        break;
    case IR_BRZ: case IR_BRNZ:
        cb(&i->a, ctx);
        break;
    case IR_RET:
        if (i->a >= 0)
            cb(&i->a, ctx);
        break;
    case IR_CALL:
        if (i->indirect)
            cb(&i->a, ctx);
        for (int k = 0; k < i->nargs; k++)
            cb(&i->argv[k].vreg, ctx);
        break;
    case IR_ASM:
        for (int k = 0; k < i->asm_ir->nin; k++)
            cb(&i->asm_ir->in[k].temp, ctx);
        for (int k = 0; k < i->asm_ir->nout; k++)
            cb(&i->asm_ir->out[k].temp, ctx);
        break;
    default:
        break;   /* CONST, STRADDR, GADDR, FADDR, LABEL, JMP: no reads */
    }
}

/* ---- def analysis ---- */

struct defs {
    int *cnt;    /* number of definitions of each vreg */
    int *ins;    /* index of the sole defining instruction when cnt == 1 */
};

static void compute_defs(struct ir_func *fn, struct defs *d)
{
    d->cnt = xcalloc((size_t)fn->nvregs, sizeof *d->cnt);
    d->ins = xmalloc((size_t)fn->nvregs * sizeof *d->ins);
    for (int v = 0; v < fn->nvregs; v++)
        d->ins[v] = -1;
    /* a parameter is bound once at entry — count that as its definition */
    int np = fn->src->nparams;
    for (int v = 0; v < np && v < fn->nvregs; v++)
        d->cnt[v] = 1;
    for (int n = 0; n < fn->nins; n++) {
        int t = def_target(&fn->ins[n]);
        if (t < 0)
            continue;
        if (d->cnt[t]++ == 0)
            d->ins[t] = n;
        else
            d->ins[t] = -1;   /* more than one def: not single-assignment */
    }
}

static void free_defs(struct defs *d)
{
    free(d->cnt);
    free(d->ins);
}

/* If vreg v holds an integer constant (single-def IR_CONST, not an SSE
 * bit-pattern), return 1 and its value. */
static int get_const(struct ir_func *fn, struct defs *d, int v, long *out)
{
    if (v < 0 || d->cnt[v] != 1 || d->ins[v] < 0)
        return 0;
    struct ir_ins *di = &fn->ins[d->ins[v]];
    if (di->op != IR_CONST || di->flt)
        return 0;
    *out = di->imm;
    return 1;
}

/* ---- constant folding ---- */

/* Normalize a computed result to the operation width: an int-class result
 * is the sign-extended low 32 bits, exactly what a 32-bit op leaves. */
static long norm(long r, int w)
{
    return w == 8 ? r : (long)(int)r;
}

static void to_const(struct ir_ins *i, long val)
{
    i->op = IR_CONST;
    i->imm = val;
    i->a = -1;
    i->b = -1;
}

static void to_mov(struct ir_ins *i, int src)
{
    i->op = IR_MOV;
    i->a = src;
    i->b = -1;
}

/* Fold a binary integer op on two constants, honoring width and signedness. */
static int fold_bin(enum ir_op op, long A, long B, int w, int sign,
                    enum binop pred, long *out)
{
    unsigned long ua = w == 8 ? (unsigned long)A : (unsigned int)A;
    unsigned long ub = w == 8 ? (unsigned long)B : (unsigned int)B;
    long sa = w == 8 ? A : (int)A;
    long sb = w == 8 ? B : (int)B;
    int sh = (int)(ub & (w == 8 ? 63 : 31));
    unsigned long r;
    switch (op) {
    case IR_ADD: r = ua + ub; break;
    case IR_SUB: r = ua - ub; break;
    case IR_MUL: r = ua * ub; break;
    case IR_AND: r = ua & ub; break;
    case IR_OR:  r = ua | ub; break;
    case IR_XOR: r = ua ^ ub; break;
    case IR_SHL: r = ua << sh; break;
    case IR_SHR: r = sign ? (unsigned long)(sa >> sh) : (ua >> sh); break;
    case IR_CMP: {
        int c;
        switch (pred) {
        case B_EQ: c = ua == ub; break;
        case B_NE: c = ua != ub; break;
        case B_LT: c = sign ? sa < sb  : ua < ub;  break;
        case B_LE: c = sign ? sa <= sb : ua <= ub; break;
        case B_GT: c = sign ? sa > sb  : ua > ub;  break;
        case B_GE: c = sign ? sa >= sb : ua >= ub; break;
        default: return 0;
        }
        r = c ? 1 : 0;
        break;
    }
    default:
        return 0;
    }
    *out = norm((long)r, w);
    return 1;
}

/* Fold an IR_EXT of a constant: keep `size` low bytes, then sign/zero-extend. */
static long fold_ext(long A, int size, int sign, int w)
{
    int bits = size * 8;
    unsigned long m = bits >= 64 ? ~0UL : (((unsigned long)1 << bits) - 1);
    unsigned long low = (unsigned long)A & m;
    long r;
    if (sign && bits < 64 && (low & ((unsigned long)1 << (bits - 1))))
        r = (long)(low | ~m);
    else
        r = (long)low;
    return norm(r, w);
}

static int pass_fold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt)
            continue;   /* never fold an SSE op as an integer */
        long A, B;
        int ka = get_const(fn, &d, i->a, &A);
        int kb = get_const(fn, &d, i->b, &B);

        if (i->op == IR_NEG || i->op == IR_BNOT) {
            if (ka) {
                unsigned long r = i->op == IR_NEG ? -(unsigned long)A
                                                  : ~(unsigned long)A;
                to_const(i, norm((long)r, i->w));
                changed = 1;
            }
            continue;
        }
        if (i->op == IR_EXT) {
            if (ka) {
                to_const(i, fold_ext(A, i->size, i->sign, i->w));
                changed = 1;
            }
            continue;
        }

        long r;
        switch (i->op) {
        case IR_ADD: case IR_SUB: case IR_MUL:
        case IR_AND: case IR_OR: case IR_XOR:
        case IR_SHL: case IR_SHR: case IR_CMP:
            break;
        default:
            continue;   /* not a foldable binary op (DIV/MOD left alone) */
        }
        if (ka && kb) {
            if (fold_bin(i->op, A, B, i->w, i->sign, i->pred, &r)) {
                to_const(i, r);
                changed = 1;
            }
            continue;
        }
        /* one-operand algebraic identities (valid for any width/signedness) */
        switch (i->op) {
        case IR_ADD:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_SUB:
            if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_OR:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_XOR:
            if (ka && A == 0) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_MUL:
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            else if (ka && A == 1) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 1) { to_mov(i, i->a); changed = 1; }
            break;
        case IR_AND:
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            break;
        case IR_SHL:
        case IR_SHR:
            if (kb && B == 0) { to_mov(i, i->a); changed = 1; }
            break;
        default:
            break;
        }
    }
    free_defs(&d);
    return changed;
}

/* ---- local value numbering (CSE within a basic block) ---- */

/* An op that may write memory (so a cached load past it is stale). */
static int writes_memory(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_STVAR: case IR_CALL: case IR_MEMCPY:
    case IR_MEMZERO: case IR_XCHG: case IR_XADD: case IR_CMPXCHG:
    case IR_ASM: case IR_VA_START:
        return 1;
    default:
        return 0;
    }
}

/* One value-number entry: the discriminating fields of a computation plus the
 * temp that first produced it. Two instructions with equal keys in the same
 * block compute the same value. Loads carry `memver` so a store between two
 * loads gives them different keys (no stale reuse). */
struct vn {
    enum ir_op op;
    int a, b, w, sign, size;
    enum binop pred;
    long imm;
    void *ptr;                 /* GADDR glob / FADDR callee */
    int label;                 /* STRADDR string index */
    int memver;                /* LDVAR / LOAD only */
    int result;                /* the temp holding this value */
};

static int vn_eq(const struct vn *x, const struct vn *y)
{
    return x->op == y->op && x->a == y->a && x->b == y->b && x->w == y->w &&
           x->sign == y->sign && x->size == y->size && x->pred == y->pred &&
           x->imm == y->imm && x->ptr == y->ptr && x->label == y->label &&
           x->memver == y->memver;
}

/* Build the value key for a CSE-able instruction; returns 0 if it is not one
 * (float ops, VOLATILE loads/ldvars, calls, stores — anything with an effect or
 * that we don't number). */
static int vn_key(struct ir_ins *i, int memver, struct vn *k)
{
    memset(k, 0, sizeof *k);
    k->op = i->op; k->a = -1; k->b = -1;
    if (i->flt)
        return 0;
    switch (i->op) {
    case IR_CONST:               /* same literal -> one temp, so uses of it CSE */
        k->imm = i->imm; k->w = i->w; return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
        k->a = i->a; k->b = i->b; k->w = i->w; k->sign = i->sign; return 1;
    case IR_CMP:
        k->a = i->a; k->b = i->b; k->w = i->w; k->sign = i->sign;
        k->pred = i->pred; return 1;
    case IR_NEG: case IR_BNOT:
        k->a = i->a; k->w = i->w; return 1;
    case IR_EXT:
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size; return 1;
    case IR_BSWAP:
        k->a = i->a; k->size = i->size; return 1;
    case IR_ADDR:                       /* &local: a frame-relative constant */
        k->a = i->a; return 1;
    case IR_GADDR: k->ptr = i->glob; return 1;
    case IR_FADDR: k->ptr = i->callee; return 1;
    case IR_STRADDR: k->label = i->label; return 1;
    case IR_LDVAR:                      /* a variable read (memory-versioned) */
        if (i->vol) return 0;
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size;
        k->memver = memver; return 1;
    case IR_LOAD:                       /* a pointer deref (memory-versioned) */
        if (i->vol) return 0;
        k->a = i->a; k->w = i->w; k->sign = i->sign; k->size = i->size;
        k->memver = memver; return 1;
    default:
        return 0;
    }
}

/* Replace a computation that reproduces an earlier one in the same block with a
 * copy of that earlier result; fold/copyprop/dce then remove the redundancy.
 * The block is the run between labels; a memory write bumps `memver` (part of a
 * load's key), so a load after a store is never reused. Sound: temps are
 * single-assignment, so equal operand temps => equal value; VOLATILE accesses
 * are never numbered (vn_key rejects them), preserving every MMIO access. */
static int pass_lvn(struct ir_func *fn)
{
    int changed = 0, memver = 0;
    struct vn *tab = NULL;
    int ntab = 0, cap = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        enum ir_op op0 = i->op;
        if (op0 == IR_LABEL) { ntab = 0; continue; }   /* block boundary */
        struct vn k;
        if (i->dst >= 0 && vn_key(i, memver, &k)) {
            int hit = -1;
            for (int t = 0; t < ntab; t++)
                if (vn_eq(&tab[t], &k)) { hit = tab[t].result; break; }
            if (hit >= 0 && hit != i->dst) {
                to_mov(i, hit);
                changed = 1;
            } else if (hit < 0) {
                if (ntab == cap) {
                    cap = cap ? cap * 2 : 32;
                    tab = xrealloc(tab, (size_t)cap * sizeof *tab);
                }
                k.result = i->dst;
                tab[ntab++] = k;
            }
        }
        if (writes_memory(op0))
            memver++;
    }
    free(tab);
    return changed;
}

/* ---- copy propagation ---- */

struct repl { int from, to, n; };

static void repl_cb(int *p, void *ctx)
{
    struct repl *r = ctx;
    if (*p == r->from) {
        *p = r->to;
        r->n++;
    }
}

static int pass_copyprop(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->op != IR_MOV || i->a < 0 || i->dst < 0 || i->a == i->dst)
            continue;
        /* Both ends must be single-assignment: the source so its value is
         * invariant, and the DEST so every use of it comes from THIS move.
         * A dest written in two branches (irgen's phi-style merge, e.g. the
         * va_arg result address) has cnt > 1 — propagating it would wrongly
         * force one branch's value onto the other's uses. */
        if (d.cnt[i->a] != 1 || d.cnt[i->dst] != 1)
            continue;
        struct repl r = { i->dst, i->a, 0 };
        for (int m = 0; m < fn->nins; m++)
            each_read(&fn->ins[m], repl_cb, &r);
        if (r.n)
            changed = 1;   /* the MOV is now dead; DCE removes it */
    }
    free_defs(&d);
    return changed;
}

/* ---- dead-code elimination ---- */

static void count_cb(int *p, void *ctx)
{
    int *use = ctx;
    use[*p]++;
}

static int pass_dce(struct ir_func *fn)
{
    int *use = xcalloc((size_t)fn->nvregs, sizeof *use);
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, use);
    int changed = 0, j = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        int t = def_target(i);
        if (is_pure(i->op) && t >= 0 && use[t] == 0) {
            changed = 1;
            continue;   /* drop it */
        }
        if (j != n)
            fn->ins[j] = *i;
        j++;
    }
    fn->nins = j;
    free(use);
    return changed;
}

/* ---- driver ---- */

static void opt_func(struct ir_func *fn)
{
    int changed = 1, guard = 0;
    while (changed && guard++ < 1000) {
        changed = 0;
        changed |= pass_fold(fn);
        changed |= pass_lvn(fn);      /* CSE: reuse identical computations */
        changed |= pass_copyprop(fn);
        changed |= pass_dce(fn);
    }
}

void opt_run(struct ir_unit *iu, int level)
{
    if (level < 1)
        return;
    for (int f = 0; f < iu->nfuncs; f++)
        opt_func(&iu->funcs[f]);
}
