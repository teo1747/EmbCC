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
    case IR_CMP:
        cb(&i->a, ctx);
        if (!i->imm_b)           /* b folded to an immediate: not a vreg read */
            cb(&i->b, ctx);
        break;
    case IR_STORE: case IR_MEMCPY: case IR_XCHG:
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

static void count_cb(int *p, void *ctx);   /* fwd: use-count accumulator (DCE) */

/* If b is 2^k (k>=1), return k; else -1. */
static int log2_pow2(long b)
{
    if (b <= 1 || (b & (b - 1)))
        return -1;
    int k = 0;
    while ((b >>= 1))
        k++;
    return k;
}

/* Retarget a single-use constant temp to a new value in place (used by strength
 * reduction: rewrite `x * 8` as `x << 3` by turning the `8` literal into `3`).
 * Safe only when the constant has exactly one use — CSE may have shared it. */
static int retarget_const(struct ir_func *fn, struct defs *d, const int *use,
                          int ctemp, long newval)
{
    if (ctemp < 0 || d->cnt[ctemp] != 1 || d->ins[ctemp] < 0 || use[ctemp] != 1)
        return 0;
    struct ir_ins *ci = &fn->ins[d->ins[ctemp]];
    if (ci->op != IR_CONST || ci->flt)
        return 0;
    ci->imm = newval;
    return 1;
}

static int pass_fold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    /* use counts, for the single-use check strength reduction needs */
    int *use = xcalloc((size_t)fn->nvregs, sizeof *use);
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], count_cb, use);
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
        case IR_DIV: case IR_MOD:   /* not const-folded (÷0), but strength-reduced */
            break;
        default:
            continue;
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
        case IR_MUL: {
            int sh;
            if ((ka && A == 0) || (kb && B == 0)) { to_const(i, 0); changed = 1; }
            else if (ka && A == 1) { to_mov(i, i->b); changed = 1; }
            else if (kb && B == 1) { to_mov(i, i->a); changed = 1; }
            /* x * 2^k -> x << k (retarget the literal to k). Handle either
             * operand being the constant, since MUL is commutative. */
            else if (kb && (sh = log2_pow2(B)) >= 0 &&
                     retarget_const(fn, &d, use, i->b, sh)) {
                i->op = IR_SHL; changed = 1;   /* x << k, count already in b */
            } else if (ka && (sh = log2_pow2(A)) >= 0 &&
                       retarget_const(fn, &d, use, i->a, sh)) {
                /* A_const * x -> x << k: put x in a, the retargeted count in b */
                int c = i->a; i->a = i->b; i->b = c;
                i->op = IR_SHL; changed = 1;
            }
            break;
        }
        case IR_DIV:
            /* unsigned x / 2^k -> x >> k (logical) */
            if (kb && !i->sign) {
                int sh = log2_pow2(B);
                if (sh >= 0 && retarget_const(fn, &d, use, i->b, sh)) {
                    i->op = IR_SHR; changed = 1;
                } else if (B == 1) { to_mov(i, i->a); changed = 1; }
            }
            break;
        case IR_MOD:
            /* unsigned x % 2^k -> x & (2^k - 1) */
            if (kb && !i->sign && log2_pow2(B) >= 0 &&
                retarget_const(fn, &d, use, i->b, B - 1)) {
                i->op = IR_AND; changed = 1;
            }
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
    free(use);
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
    /* Removing instructions renumbers the ones that follow. fn->var_scope_lo/hi
     * (irgen-stamped instruction indices, read by codegen's coalesce_locals to
     * decide which address-taken locals may share a stack slot) must move with
     * them — otherwise a stale scope index is compared against a FRESH liveness
     * index and two locals whose lifetimes actually overlap get the same slot,
     * so one's store clobbers the other. newpos[n] is the new index instruction
     * n lands at (a dropped instruction collapses onto the next survivor); it
     * maps the half-open [lo, hi) scope bounds, index nins included. */
    int *newpos = fn->var_scope_lo
        ? xmalloc((size_t)(fn->nins + 1) * sizeof *newpos) : NULL;
    int changed = 0, j = 0;
    for (int n = 0; n < fn->nins; n++) {
        if (newpos) newpos[n] = j;
        struct ir_ins *i = &fn->ins[n];
        int t = def_target(i);
        if (is_pure(i->op) && t >= 0 && use[t] == 0) {
            changed = 1;
            continue;   /* drop it */
        }
        /* Dead store: a non-volatile STVAR to a local nothing ever reads (no
         * LDVAR and no address-of, so use[dst] == 0) has no effect — drop it.
         * This is what clears an inlined parameter once store-forwarding has
         * rewritten its loads to the argument. */
        if (i->op == IR_STVAR && !i->vol && i->dst >= 0 &&
            i->dst < fn->nvregs && use[i->dst] == 0) {
            changed = 1;
            continue;
        }
        if (j != n)
            fn->ins[j] = *i;
        j++;
    }
    if (newpos) {
        newpos[fn->nins] = j;
        for (int v = 0; v < fn->src->nvars; v++) {
            int lo = fn->var_scope_lo[v], hi = fn->var_scope_hi[v];
            if (lo >= 0 && lo <= fn->nins) fn->var_scope_lo[v] = newpos[lo];
            if (hi >= 0 && hi <= fn->nins) fn->var_scope_hi[v] = newpos[hi];
        }
        free(newpos);
    }
    fn->nins = j;
    free(use);
    return changed;
}

/* ==== SSA-based mem2reg (-O2) =============================================== *
 *
 * Promotes every non-address-taken scalar local out of memory into SSA temps.
 * Builds the CFG, the dominator tree (Cooper-Harvey-Kennedy) and dominance
 * frontiers, inserts phi-functions at the iterated frontier of each variable's
 * defs, renames defs/uses to versioned temps down the dominator tree, then
 * destructs SSA by realising each phi as copies on its incoming edges — the
 * branch-taken edge via a trampoline block, a branch's fall-through with inline
 * copies (they run only when the branch is not taken), single-successor edges by
 * appending. Every copy set is sequenced read-all-then-write-all through fresh
 * temps, so a swap or a self-referential loop phi is safe. This turns
 * STVAR/LDVAR chains that cross basic blocks — loop counters, a value live down
 * one arm of an `if` — into temps that fold/lvn/copyprop then optimise, which is
 * what the block-local store-forwarding could not reach. */

struct bb {
    int start, end;                 /* instruction range [start, end) */
    int succ[2], nsucc;
    int *pred, npred;
    int idom, rpo;                  /* immediate dominator; reverse-postorder # */
    int *phi_local, *phi_res, nphi; /* phi(local) -> result temp, per block */
    int **phi_inc;                  /* phi_inc[p][k] = value on edge from pred p */
};

/* Growable instruction buffer, for rebuilding fn->ins out of SSA. */
struct ibuf { struct ir_ins *p; int n, cap; };
static struct ir_ins *ib_push(struct ibuf *b)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = xrealloc(b->p, (size_t)b->cap * sizeof *b->p);
    }
    struct ir_ins *i = &b->p[b->n++];
    memset(i, 0, sizeof *i);
    return i;
}

static void bb_add_pred(struct bb *b, int p)
{
    for (int i = 0; i < b->npred; i++)
        if (b->pred[i] == p) return;
    b->pred = xrealloc(b->pred, (size_t)(b->npred + 1) * sizeof *b->pred);
    b->pred[b->npred++] = p;
}

/* Build basic blocks + succ/pred + a label->block map. */
static struct bb *build_cfg(struct ir_func *fn, int *nbb_out, int **l2b_out)
{
    int N = fn->nins;
    char *lead = xcalloc((size_t)(N ? N : 1), 1);
    if (N) lead[0] = 1;
    for (int i = 0; i < N; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) lead[i] = 1;
        if ((op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
             op == IR_RET || op == IR_UD2) && i + 1 < N)
            lead[i + 1] = 1;
    }
    int nbb = 0;
    for (int i = 0; i < N; i++) if (lead[i]) nbb++;
    if (nbb == 0) nbb = 1;
    struct bb *bb = xcalloc((size_t)nbb, sizeof *bb);
    int b = 0, prev = 0;
    for (int i = 1; i <= N; i++)
        if (i == N || lead[i]) { bb[b].start = prev; bb[b].end = i; prev = i; b++; }
    for (int i = 0; i < nbb; i++) { bb[i].idom = -1; bb[i].rpo = -1; }

    int *l2b = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1) * sizeof *l2b);
    for (int l = 0; l < fn->nlabels; l++) l2b[l] = -1;
    for (int i = 0; i < nbb; i++)
        if (bb[i].end > bb[i].start && fn->ins[bb[i].start].op == IR_LABEL)
            l2b[fn->ins[bb[i].start].label] = i;

    for (int i = 0; i < nbb; i++) {
        enum ir_op op = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].op : IR_UD2;
        int L = bb[i].end > bb[i].start ? fn->ins[bb[i].end - 1].label : -1;
        if (op == IR_RET || op == IR_UD2) {
            /* no successors */
        } else if (op == IR_JMP) {
            if (l2b[L] >= 0) bb[i].succ[bb[i].nsucc++] = l2b[L];
        } else if (op == IR_BRZ || op == IR_BRNZ) {
            if (l2b[L] >= 0) bb[i].succ[bb[i].nsucc++] = l2b[L];
            if (i + 1 < nbb) bb[i].succ[bb[i].nsucc++] = i + 1;
        } else if (i + 1 < nbb) {
            bb[i].succ[bb[i].nsucc++] = i + 1;
        }
    }
    for (int i = 0; i < nbb; i++)
        for (int k = 0; k < bb[i].nsucc; k++)
            bb_add_pred(&bb[bb[i].succ[k]], i);
    free(lead);
    *nbb_out = nbb;
    *l2b_out = l2b;
    return bb;
}

/* Reverse-postorder numbering from the entry (block 0). */
static void compute_rpo(struct bb *bb, int nbb, int *order, int *norder)
{
    char *seen = xcalloc((size_t)nbb, 1);
    int *stk = xmalloc((size_t)nbb * sizeof *stk), *it = xmalloc((size_t)nbb * sizeof *it);
    int sp = 0, po = 0, *post = xmalloc((size_t)nbb * sizeof *post);
    stk[sp] = 0; it[sp] = 0; seen[0] = 1;
    while (sp >= 0) {
        int u = stk[sp];
        if (it[sp] < bb[u].nsucc) {
            int w = bb[u].succ[it[sp]++];
            if (!seen[w]) { seen[w] = 1; sp++; stk[sp] = w; it[sp] = 0; }
        } else { post[po++] = u; sp--; }
    }
    *norder = po;
    for (int i = 0; i < po; i++) order[i] = post[po - 1 - i];   /* reverse */
    for (int i = 0; i < po; i++) bb[order[i]].rpo = i;
    free(seen); free(stk); free(it); free(post);
}

static int idom_intersect(struct bb *bb, int a, int b)
{
    while (a != b) {
        while (bb[a].rpo > bb[b].rpo) a = bb[a].idom;
        while (bb[b].rpo > bb[a].rpo) b = bb[b].idom;
    }
    return a;
}

/* Immediate dominators (Cooper-Harvey-Kennedy) over the reachable blocks. */
static void compute_idom(struct bb *bb, int *order, int norder)
{
    bb[0].idom = 0;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 1; i < norder; i++) {       /* skip entry, RPO order */
            int b = order[i], nd = -1;
            for (int k = 0; k < bb[b].npred; k++) {
                int p = bb[b].pred[k];
                if (bb[p].idom < 0) continue;    /* not yet processed */
                nd = nd < 0 ? p : idom_intersect(bb, nd, p);
            }
            if (nd >= 0 && bb[b].idom != nd) { bb[b].idom = nd; changed = 1; }
        }
    }
}

/* Dominance frontiers. df[b] holds the blocks on b's frontier. */
static void compute_df(struct bb *bb, int nbb, int **df, int *ndf)
{
    for (int b = 0; b < nbb; b++) {
        if (bb[b].npred < 2) continue;
        for (int k = 0; k < bb[b].npred; k++) {
            int r = bb[b].pred[k];
            while (r >= 0 && r != bb[b].idom) {
                int dup = 0;
                for (int j = 0; j < ndf[r]; j++) if (df[r][j] == b) dup = 1;
                if (!dup) { df[r] = xrealloc(df[r], (size_t)(ndf[r]+1)*sizeof(int));
                            df[r][ndf[r]++] = b; }
                r = bb[r].idom;
            }
        }
    }
}

/* A full-width plain access (no truncation/extension mismatch between a store
 * and a load) — the same soundness gate mem2reg and store-forwarding share. */
static int m2r_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

/* Emit the phi copies for edge (pred p -> block s): read every incoming value
 * into a fresh temp, then write each phi result — read-all-then-write-all, so a
 * self-referential loop phi or a swap is realised correctly. */
static void emit_edge_copies(struct ibuf *nb, struct bb *bb, int s, int p,
                             struct ir_func *fn)
{
    struct bb *S = &bb[s];
    if (S->nphi == 0) return;
    int pi = -1;
    for (int k = 0; k < S->npred; k++) if (S->pred[k] == p) { pi = k; break; }
    if (pi < 0) return;
    /* A conflict — an incoming value that is another phi result of this block
     * (a swap, a self-referential loop phi) — needs read-all-then-write-all
     * through temps. The common case has none: emit direct copies, no temps. */
    int conflict = 0;
    for (int k = 0; k < S->nphi && !conflict; k++)
        for (int j = 0; j < S->nphi; j++)
            if (S->phi_inc[pi][k] == S->phi_res[j]) { conflict = 1; break; }
    if (!conflict) {
        for (int k = 0; k < S->nphi; k++) {
            struct ir_ins *mv = ib_push(nb);
            mv->op = IR_MOV; mv->dst = S->phi_res[k]; mv->a = S->phi_inc[pi][k];
        }
        return;
    }
    int *tmp = xmalloc((size_t)S->nphi * sizeof *tmp);
    for (int k = 0; k < S->nphi; k++) {
        tmp[k] = fn->nvregs++;
        struct ir_ins *mv = ib_push(nb);
        mv->op = IR_MOV; mv->dst = tmp[k]; mv->a = S->phi_inc[pi][k];
    }
    for (int k = 0; k < S->nphi; k++) {
        struct ir_ins *mv = ib_push(nb);
        mv->op = IR_MOV; mv->dst = S->phi_res[k]; mv->a = tmp[k];
    }
    free(tmp);
}

static void mem2reg_free(struct bb *bb, int nbb, int **df, int *ndf, int *l2b,
                         int *order)
{
    for (int i = 0; i < nbb; i++) {
        free(bb[i].pred);
        free(bb[i].phi_local); free(bb[i].phi_res);
        if (bb[i].phi_inc) {
            for (int k = 0; k < bb[i].npred; k++) free(bb[i].phi_inc[k]);
            free(bb[i].phi_inc);
        }
        free(df[i]);
    }
    free(bb); free(df); free(ndf); free(l2b); free(order);
}

static int pass_mem2reg(struct ir_func *fn)
{
    int nvars = fn->src->nvars;
    if (nvars == 0 || fn->nins == 0)
        return 0;

    /* 1. Promotable locals: a scalar int/ptr of 4 or 8 bytes, never
     * address-taken, every load full-width plain. */
    int nparams = fn->src->nparams;
    char *ok = xmalloc((size_t)nvars);
    for (int L = 0; L < nvars; L++) {
        struct type *t = fn->src->var_tys[L];
        /* Params are excluded: their value is live on entry (no defining IR
         * instruction), so SSA has no version to seed a read with. Only true
         * locals, always assigned before use, are promoted. */
        ok[L] = L >= nparams && t &&
                (ty_is_integer(t) || t->kind == TY_PTR) &&
                (ty_size(t) == 4 || ty_size(t) == 8);
    }
    for (int L = 0; L < nvars; L++)
        if (fn->src->var_tys[L] && fn->src->var_tys[L]->is_volatile)
            ok[L] = 0;                          /* volatile: every access must stay */
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if (in->op == IR_ADDR && in->a >= 0 && in->a < nvars) ok[in->a] = 0;
        if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars &&
            (in->vol || !m2r_plain(in->size, in->sign, in->w))) ok[in->a] = 0;
        if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars && in->vol)
            ok[in->dst] = 0;
    }
    int nprom = 0;
    int *prom = xmalloc((size_t)nvars * sizeof *prom);     /* local -> prom idx */
    int *ploc = xmalloc((size_t)nvars * sizeof *ploc);     /* prom idx -> local */
    for (int L = 0; L < nvars; L++)
        prom[L] = ok[L] ? (ploc[nprom] = L, nprom++) : -1;
    free(ok);
    if (nprom == 0) { free(prom); free(ploc); return 0; }

    /* 2. CFG + dominance. */
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: bail rather than mis-dominate */
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free(prom); free(ploc); return 0;
    }
    compute_idom(bb, order, norder);
    int **df = xcalloc((size_t)nbb, sizeof *df);
    int *ndf = xcalloc((size_t)nbb, sizeof *ndf);
    compute_df(bb, nbb, df, ndf);

    /* 3. Phi insertion at the iterated dominance frontier of each var's defs. */
    char *hasphi = xcalloc((size_t)nbb * (size_t)nprom, 1);
    int *work = xmalloc((size_t)nbb * sizeof *work);
    for (int pidx = 0; pidx < nprom; pidx++) {
        int L = ploc[pidx], nw = 0;
        char *ondef = xcalloc((size_t)nbb, 1);
        for (int bI = 0; bI < nbb; bI++)
            for (int i = bb[bI].start; i < bb[bI].end; i++)
                if (fn->ins[i].op == IR_STVAR && fn->ins[i].dst == L) {
                    if (!ondef[bI]) { ondef[bI] = 1; work[nw++] = bI; }
                    break;
                }
        while (nw) {
            int x = work[--nw];
            for (int j = 0; j < ndf[x]; j++) {
                int d = df[x][j];
                if (hasphi[d * nprom + pidx]) continue;
                hasphi[d * nprom + pidx] = 1;
                bb[d].phi_local = xrealloc(bb[d].phi_local, (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_res   = xrealloc(bb[d].phi_res,   (size_t)(bb[d].nphi+1)*sizeof(int));
                bb[d].phi_local[bb[d].nphi] = L;
                bb[d].phi_res[bb[d].nphi] = fn->nvregs++;
                bb[d].nphi++;
                if (!ondef[d]) { ondef[d] = 1; work[nw++] = d; }
            }
        }
        free(ondef);
    }
    for (int b = 0; b < nbb; b++) if (bb[b].nphi) {
        bb[b].phi_inc = xcalloc((size_t)bb[b].npred, sizeof *bb[b].phi_inc);
        for (int k = 0; k < bb[b].npred; k++)
            bb[b].phi_inc[k] = xmalloc((size_t)bb[b].nphi * sizeof(int));
    }

    /* 4. Rename down the dominator tree. Each prom has a version stack; an entry
     * "undef" temp (0) gives an uninitialised read a defined value. */
    int *undef = xmalloc((size_t)nprom * sizeof *undef);
    int **stk = xmalloc((size_t)nprom * sizeof *stk);
    int *sp = xcalloc((size_t)nprom, sizeof *sp);
    int *scap = xcalloc((size_t)nprom, sizeof *scap);
    for (int p = 0; p < nprom; p++) {
        undef[p] = fn->nvregs++;
        stk[p] = xmalloc(sizeof(int) * 8); scap[p] = 8;
        stk[p][sp[p]++] = undef[p];
    }
    /* explicit dominator-tree DFS (children = blocks whose idom is this block) */
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *dpushed = xcalloc((size_t)nbb * nprom, sizeof *dpushed); /* per (block,prom) */
    char *entered = xcalloc((size_t)nbb, 1);
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            /* phi defs become the current version */
            for (int k = 0; k < bb[b].nphi; k++) {
                int pidx = prom[bb[b].phi_local[k]];
                if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                stk[pidx][sp[pidx]++] = bb[b].phi_res[k];
                dpushed[b * nprom + pidx]++;
            }
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op == IR_LDVAR && in->a >= 0 && in->a < nvars && prom[in->a] >= 0) {
                    int pidx = prom[in->a];
                    in->op = IR_MOV; in->a = stk[pidx][sp[pidx]-1]; in->b = -1;
                } else if (in->op == IR_STVAR && in->dst >= 0 && in->dst < nvars && prom[in->dst] >= 0) {
                    int pidx = prom[in->dst];
                    if (sp[pidx] == scap[pidx]) { scap[pidx]*=2; stk[pidx]=xrealloc(stk[pidx],(size_t)scap[pidx]*sizeof(int)); }
                    stk[pidx][sp[pidx]++] = in->a;   /* the stored temp is the new version */
                    dpushed[b * nprom + pidx]++;
                    in->op = IR_MOV; in->dst = -1; in->a = -1;  /* mark: drop in rebuild */
                }
            }
            /* fill successors' phi incoming from this block */
            for (int s = 0; s < bb[b].nsucc; s++) {
                int sb = bb[b].succ[s];
                if (!bb[sb].nphi) continue;
                int pk = -1;
                for (int k = 0; k < bb[sb].npred; k++) if (bb[sb].pred[k]==b){pk=k;break;}
                for (int k = 0; k < bb[sb].nphi; k++) {
                    int pidx = prom[bb[sb].phi_local[k]];
                    bb[sb].phi_inc[pk][k] = stk[pidx][sp[pidx]-1];
                }
            }
            /* push dom-tree children */
            for (int c = 0; c < nbb; c++)
                if (c != 0 && bb[c].idom == b && !entered[c]) dstk[dsp++] = c;
        } else {
            /* leaving b: pop its versions */
            for (int p = 0; p < nprom; p++) sp[p] -= dpushed[b * nprom + p];
            dsp--;
        }
    }

    /* 5. Rebuild the linear IR out of SSA. */
    struct ibuf nb = { 0, 0, 0 };
    for (int p = 0; p < nprom; p++) {   /* entry undef defs */
        struct ir_ins *c = ib_push(&nb);
        c->op = IR_CONST; c->dst = undef[p]; c->imm = 0;
        c->w = ty_size(fn->src->var_tys[ploc[p]]) == 8 ? 8 : 4;
    }
    struct { int lbl, from, edge_pred; } *tramp = NULL; int ntramp = 0, ctramp = 0;
    for (int b = 0; b < nbb; b++) {
        int hasterm = bb[b].end > bb[b].start;
        enum ir_op top = hasterm ? fn->ins[bb[b].end - 1].op : IR_UD2;
        int isterm = top == IR_JMP || top == IR_BRZ || top == IR_BRNZ ||
                     top == IR_RET || top == IR_UD2;
        int body_end = (hasterm && isterm) ? bb[b].end - 1 : bb[b].end;
        for (int i = bb[b].start; i < body_end; i++)
            if (!(fn->ins[i].op == IR_MOV && fn->ins[i].dst < 0))   /* dropped store */
                *ib_push(&nb) = fn->ins[i];
        if (top == IR_RET || top == IR_UD2) {
            if (isterm) *ib_push(&nb) = fn->ins[bb[b].end - 1];
        } else if (top == IR_JMP && isterm) {
            emit_edge_copies(&nb, bb, bb[b].succ[0], b, fn);
            *ib_push(&nb) = fn->ins[bb[b].end - 1];
        } else if ((top == IR_BRZ || top == IR_BRNZ) && isterm) {
            struct ir_ins br = fn->ins[bb[b].end - 1];   /* branch-taken = succ[0] */
            int taken = bb[b].succ[0];
            if (bb[taken].nphi) {                        /* trampoline the taken edge */
                int Lt = fn->nlabels++;
                if (ntramp == ctramp) { ctramp = ctramp?ctramp*2:8;
                    tramp = xrealloc(tramp, (size_t)ctramp*sizeof *tramp); }
                tramp[ntramp].lbl = Lt; tramp[ntramp].from = b;
                tramp[ntramp].edge_pred = taken; ntramp++;
                br.label = Lt;
            }
            *ib_push(&nb) = br;
            if (bb[b].nsucc > 1)                         /* fall-through copies (inline) */
                emit_edge_copies(&nb, bb, bb[b].succ[1], b, fn);
        } else {   /* falls through to the next block */
            if (bb[b].nsucc > 0)
                emit_edge_copies(&nb, bb, bb[b].succ[0], b, fn);
        }
    }
    /* The last real block may fall off the end — an implicit return that the
     * original linear IR carries no explicit RET for (codegen returns at the
     * function's physical end). Trampoline blocks are appended next, so a
     * fall-through last block would run straight into one. Cap it with a void
     * RET. Only a block that does NOT end in an unconditional jump/ret can
     * reach the next physical instruction, so only those need the cap. */
    if (ntramp > 0 && nbb > 0) {
        enum ir_op lt = bb[nbb - 1].end > bb[nbb - 1].start
                        ? fn->ins[bb[nbb - 1].end - 1].op : IR_UD2;
        if (lt != IR_JMP && lt != IR_RET && lt != IR_UD2) {
            struct ir_ins *r = ib_push(&nb);
            r->op = IR_RET; r->a = -1;
        }
    }
    for (int t = 0; t < ntramp; t++) {   /* trampoline blocks: label; copies; jmp */
        struct ir_ins *lb = ib_push(&nb);
        lb->op = IR_LABEL; lb->label = tramp[t].lbl;
        emit_edge_copies(&nb, bb, tramp[t].edge_pred, tramp[t].from, fn);
        struct ir_ins *jp = ib_push(&nb);
        int origlbl = -1;
        for (int i = bb[tramp[t].edge_pred].start; i < bb[tramp[t].edge_pred].end; i++)
            if (fn->ins[i].op == IR_LABEL) { origlbl = fn->ins[i].label; break; }
        jp->op = IR_JMP; jp->label = origlbl;
    }
    free(tramp);

    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;

    /* The rebuild renumbered every instruction, so the local scope ranges (which
     * are instruction indices, used by codegen to coalesce disjoint-lifetime
     * locals) are now stale. Drop them: codegen then gives each surviving local
     * its own slot — correct, if a touch larger. Promoted locals are dead. */
    if (fn->var_scope_lo) {
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = fn->var_scope_hi = NULL;
    }

    for (int p = 0; p < nprom; p++) free(stk[p]);
    free(undef); free(stk); free(sp); free(scap);
    free(dstk); free(dpushed); free(entered);
    free(hasphi); free(work); free(prom); free(ploc);
    mem2reg_free(bb, nbb, df, ndf, l2b, order);
    return 1;
}

/* ---- global common-subexpression elimination (dominator-scoped VN) ----
 *
 * pass_lvn reuses an identical computation only within a block. Global CSE
 * carries a value across the dominator tree: a value computed in a block is
 * available to every block that block dominates. Sound because the operand
 * temps are single-assignment (equal temps => equal value) and the producing
 * temp, defined in a dominator, is live on every path to the reuse. Only
 * position-independent, non-memory pure ops are numbered — arithmetic, compares,
 * extends, address computations, constants; a memory read (LDVAR/LOAD) depends
 * on a store history that crosses blocks, so pass_lvn keeps those local. */
static int gcse_numberable(enum ir_op op)
{
    switch (op) {
    /* Only genuinely COMPUTED values. A cheap single-instruction
     * materialization (CONST, a lea for &local/&global/string/func, a
     * sign/zero-extend, a bswap) costs less to recompute than to keep live
     * across the dominated region — global-CSEing those only lengthens a live
     * range (forcing a spill or a callee-saved reg) for no win. Redundant
     * arithmetic/compares are the profitable case. Memory reads (LDVAR/LOAD)
     * stay with the memory-versioned pass_lvn. */
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_DIV: case IR_MOD: case IR_AND: case IR_OR: case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_CMP: case IR_NEG: case IR_BNOT:
        return 1;
    default:
        return 0;
    }
}

static int pass_gcse(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {   /* unreachable blocks: dominance is not total, bail */
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); return 0;
    }
    compute_idom(bb, order, norder);

    /* An active table holding the current block's and its dominators' values,
     * pushed on enter and truncated back on leave — an explicit dom-tree DFS so
     * siblings never see each other's values (they do not dominate each other). */
    struct vn *tab = NULL; int ntab = 0, captab = 0, changed = 0;
    int *dstk = xmalloc((size_t)nbb * sizeof *dstk);
    int *mark = xmalloc((size_t)nbb * sizeof *mark);
    char *entered = xcalloc((size_t)nbb, 1);
    int dsp = 0; dstk[dsp++] = 0;
    while (dsp) {
        int b = dstk[dsp - 1];
        if (!entered[b]) {
            entered[b] = 1;
            mark[b] = ntab;
            for (int n = bb[b].start; n < bb[b].end; n++) {
                struct ir_ins *i = &fn->ins[n];
                struct vn k;
                if (i->dst < 0 || !gcse_numberable(i->op) || !vn_key(i, 0, &k))
                    continue;
                int hit = -1;
                for (int t = 0; t < ntab; t++)
                    if (vn_eq(&tab[t], &k)) { hit = tab[t].result; break; }
                if (hit >= 0 && hit != i->dst) {
                    to_mov(i, hit); changed = 1;
                } else if (hit < 0) {
                    if (ntab == captab) { captab = captab ? captab * 2 : 64;
                        tab = xrealloc(tab, (size_t)captab * sizeof *tab); }
                    k.result = i->dst; tab[ntab++] = k;
                }
            }
            for (int c = 0; c < nbb; c++)
                if (c != 0 && bb[c].idom == b && !entered[c]) dstk[dsp++] = c;
        } else {
            ntab = mark[b];    /* leaving b: drop its (and its subtree's) values */
            dsp--;
        }
    }
    free(tab); free(dstk); free(mark); free(entered);
    free(order); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb);
    return changed;
}

/* ---- global redundant-load elimination (available-expressions) ------------ *
 *
 * pass_gcse leaves memory reads (LDVAR/LOAD) to the block-local pass_lvn: their
 * value depends on a store history that crosses blocks, which dominance alone
 * cannot reason about (a store on a non-dominator-tree path still kills a load).
 * This pass does the real thing — an available-expressions dataflow. A load is
 * redundant at a point if an identical earlier load reaches it on EVERY path
 * with no intervening write that could alias it; the reload becomes a copy.
 *
 * Soundness rests on three things:
 *   - the meet is "same representative TEMP from all predecessors", so the reused
 *     value is one dominating definition (loads produce single-def temps), never
 *     a per-path phi;
 *   - a LOAD is keyed only when its address temp is single-def, so the address
 *     cannot change between the two loads;
 *   - the kill model separates a store to a non-address-taken local (kills only
 *     that local's LDVARs) from a real memory write / call / asm (kills every
 *     LOAD and every address-taken local's LDVAR — no finer alias analysis).
 */
struct lkey { enum ir_op op; int a, size, sign, w; };

static int lcse_kills_mem(enum ir_op op)
{
    switch (op) {
    case IR_STORE: case IR_CALL: case IR_MEMCPY: case IR_MEMZERO:
    case IR_XCHG: case IR_XADD: case IR_CMPXCHG: case IR_ASM: case IR_VA_START:
        return 1;
    default:
        return 0;
    }
}

static int pass_loadcse(struct ir_func *fn)
{
    int nvars = fn->src->nvars;
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);
    int *order = xmalloc((size_t)nbb * sizeof *order), norder;
    compute_rpo(bb, nbb, order, &norder);
    if (norder != nbb) {
        free(order); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free_defs(&d); return 0;
    }

    char *taken = xcalloc((size_t)(nvars ? nvars : 1), 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 && fn->ins[i].a < nvars)
            taken[fn->ins[i].a] = 1;

    /* Enumerate distinct load keys; keyidx[i] maps a load instruction to one. */
    struct lkey *keys = NULL; int nk = 0, capk = 0;
    int *keyidx = xmalloc((size_t)fn->nins * sizeof *keyidx);
    for (int i = 0; i < fn->nins; i++) {
        keyidx[i] = -1;
        struct ir_ins *in = &fn->ins[i];
        struct lkey k;
        if (in->op == IR_LDVAR && !in->vol && in->a >= 0 && in->a < nvars) {
            k = (struct lkey){ IR_LDVAR, in->a, in->size, in->sign, in->w };
        } else if (in->op == IR_LOAD && !in->vol && in->a >= 0 &&
                   in->a < fn->nvregs && d.cnt[in->a] == 1) {
            k = (struct lkey){ IR_LOAD, in->a, in->size, in->sign, in->w };
        } else {
            continue;
        }
        int found = -1;
        for (int j = 0; j < nk; j++)
            if (keys[j].op == k.op && keys[j].a == k.a && keys[j].size == k.size &&
                keys[j].sign == k.sign && keys[j].w == k.w) { found = j; break; }
        if (found < 0) {
            if (nk == capk) { capk = capk ? capk * 2 : 32;
                keys = xrealloc(keys, (size_t)capk * sizeof *keys); }
            keys[nk] = k; found = nk++;
        }
        keyidx[i] = found;
    }
    if (nk == 0) {
        free(order); free(l2b); free(taken); free(keyidx); free(keys);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free_defs(&d); return 0;
    }
    /* is_mem[k]: a LOAD or an address-taken local's LDVAR (killed by any write).
     * A non-address-taken local's LDVAR is killed only by a store to that local. */
    char *is_mem = xmalloc((size_t)nk);
    for (int k = 0; k < nk; k++)
        is_mem[k] = keys[k].op == IR_LOAD ||
                    (keys[k].op == IR_LDVAR && taken[keys[k].a]);

    /* Dataflow. avail[b][k]: -2 top (init), -1 not available, >=0 the temp. */
    int *aout = xmalloc((size_t)nbb * (size_t)nk * sizeof *aout);
    int *ain  = xmalloc((size_t)nbb * (size_t)nk * sizeof *ain);
    for (int i = 0; i < nbb * nk; i++) aout[i] = -2;
    int *s = xmalloc((size_t)nk * sizeof *s);

    for (int iter = 0, changed = 1; changed && iter < nbb + 2; iter++) {
        changed = 0;
        for (int oi = 0; oi < nbb; oi++) {
            int b = order[oi];
            int *in = ain + (size_t)b * nk;
            if (b == 0) {
                for (int k = 0; k < nk; k++) in[k] = -1;   /* entry: empty */
            } else {
                for (int k = 0; k < nk; k++) in[k] = -2;    /* top */
                for (int p = 0; p < bb[b].npred; p++) {
                    int *po = aout + (size_t)bb[b].pred[p] * nk;
                    for (int k = 0; k < nk; k++) {
                        int m = in[k], v = po[k];        /* three-valued meet */
                        in[k] = (m == -1 || v == -1) ? -1
                              : (m == -2) ? v : (v == -2) ? m
                              : (m == v) ? m : -1;
                    }
                }
                for (int k = 0; k < nk; k++) if (in[k] == -2) in[k] = -1;
            }
            for (int k = 0; k < nk; k++) s[k] = in[k];
            for (int i = bb[b].start; i < bb[b].end; i++) {
                struct ir_ins *ins = &fn->ins[i];
                if (ins->op == IR_STVAR) {
                    for (int k = 0; k < nk; k++)
                        if ((keys[k].op == IR_LDVAR && keys[k].a == ins->dst) ||
                            (taken[ins->dst] && is_mem[k]))
                            s[k] = -1;
                } else if (lcse_kills_mem(ins->op)) {
                    for (int k = 0; k < nk; k++) if (is_mem[k]) s[k] = -1;
                }
                int k = keyidx[i];
                if (k >= 0 && s[k] < 0) s[k] = ins->dst;   /* first def of the value */
            }
            int *out = aout + (size_t)b * nk;
            for (int k = 0; k < nk; k++)
                if (out[k] != s[k]) { out[k] = s[k]; changed = 1; }
        }
    }

    /* Replacement: replay each block from its (now stable) avail_in. */
    int changed = 0;
    for (int b = 0; b < nbb; b++) {
        int *in = ain + (size_t)b * nk;
        for (int k = 0; k < nk; k++) s[k] = in[k];
        for (int i = bb[b].start; i < bb[b].end; i++) {
            struct ir_ins *ins = &fn->ins[i];
            if (ins->op == IR_STVAR) {
                for (int k = 0; k < nk; k++)
                    if ((keys[k].op == IR_LDVAR && keys[k].a == ins->dst) ||
                        (taken[ins->dst] && is_mem[k]))
                        s[k] = -1;
            } else if (lcse_kills_mem(ins->op)) {
                for (int k = 0; k < nk; k++) if (is_mem[k]) s[k] = -1;
            }
            int k = keyidx[i];
            if (k < 0) continue;
            if (s[k] >= 0 && s[k] != ins->dst) {
                to_mov(ins, s[k]); changed = 1;            /* redundant reload */
            } else if (s[k] < 0) {
                s[k] = ins->dst;
            }
        }
    }

    free(order); free(l2b); free(taken); free(keyidx); free(keys);
    free(is_mem); free(aout); free(ain); free(s);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb); free_defs(&d);
    return changed;
}

/* ---- conditional constant propagation (the reachability half of SCCP) ----
 *
 * A conditional branch whose condition is a known constant has one live edge.
 * Resolve it (BRZ/BRNZ -> unconditional jump, or fall-through), then drop every
 * block no longer reachable over the live edges. The constant conditions come
 * from inlining a call with a constant argument, mem2reg + folding collapsing a
 * flag, config constants — code the earlier passes leave as `test; jz` over a
 * value they have already proven constant, plus the now-dead arm behind it.
 *
 * One rebuild handles both: emit each reachable block, replacing a resolved
 * branch with a jump to its live successor (or nothing when that successor is
 * the fall-through), and skip unreachable blocks entirely. */
static int pass_sccp(struct ir_func *fn)
{
    if (fn->nins == 0)
        return 0;
    struct defs d;
    compute_defs(fn, &d);
    int nbb, *l2b;
    struct bb *bb = build_cfg(fn, &nbb, &l2b);

    /* live_only[b] = index into bb[b].succ of the sole live edge, or -1 = all.
     * succ[0] is the branch-taken target, succ[1] the fall-through. */
    int *live_only = xmalloc((size_t)nbb * sizeof *live_only);
    for (int b = 0; b < nbb; b++) {
        live_only[b] = -1;
        if (bb[b].end <= bb[b].start)
            continue;
        struct ir_ins *t = &fn->ins[bb[b].end - 1];
        if ((t->op != IR_BRZ && t->op != IR_BRNZ) || t->a < 0 ||
            d.cnt[t->a] != 1 || d.ins[t->a] < 0 ||
            fn->ins[d.ins[t->a]].op != IR_CONST)
            continue;
        long v = fn->ins[d.ins[t->a]].imm;
        int taken = (t->op == IR_BRZ) ? (v == 0) : (v != 0);
        int want = taken ? 0 : 1;
        if (want < bb[b].nsucc)      /* only if that edge actually exists */
            live_only[b] = want;
    }

    /* Reachability over the live edges only. */
    char *reach = xcalloc((size_t)nbb, 1);
    int *wl = xmalloc((size_t)nbb * sizeof *wl), nwl = 0;
    reach[0] = 1; wl[nwl++] = 0;
    while (nwl) {
        int b = wl[--nwl];
        for (int s = 0; s < bb[b].nsucc; s++) {
            if (live_only[b] >= 0 && s != live_only[b])
                continue;
            int sb = bb[b].succ[s];
            if (!reach[sb]) { reach[sb] = 1; wl[nwl++] = sb; }
        }
    }

    int work = 0;
    for (int b = 0; b < nbb; b++)
        if (!reach[b] || live_only[b] >= 0) { work = 1; break; }
    if (!work) {
        free(live_only); free(reach); free(wl); free(l2b);
        for (int i = 0; i < nbb; i++) free(bb[i].pred);
        free(bb); free_defs(&d);
        return 0;
    }

    struct ibuf nb = { 0, 0, 0 };
    for (int b = 0; b < nbb; b++) {
        if (!reach[b])
            continue;                       /* unreachable: drop the whole block */
        if (live_only[b] >= 0) {
            for (int n = bb[b].start; n < bb[b].end - 1; n++)  /* body, not branch */
                *ib_push(&nb) = fn->ins[n];
            int ls = bb[b].succ[live_only[b]];
            /* Jump to the live successor by label; if it has none it is the
             * fall-through (b+1, reachable, emitted next) — just fall in. */
            if (bb[ls].end > bb[ls].start && fn->ins[bb[ls].start].op == IR_LABEL) {
                struct ir_ins *j = ib_push(&nb);
                j->op = IR_JMP; j->dst = -1; j->a = -1; j->b = -1;
                j->label = fn->ins[bb[ls].start].label;
            }
        } else {
            for (int n = bb[b].start; n < bb[b].end; n++)
                *ib_push(&nb) = fn->ins[n];
        }
    }
    free(fn->ins);
    fn->ins = nb.p; fn->nins = nb.n; fn->cap = nb.cap;

    /* The rebuild renumbered every instruction, so the local scope ranges (used
     * by codegen to coalesce disjoint-lifetime locals) are stale — drop them,
     * as mem2reg does; each surviving local then takes its own slot. */
    if (fn->var_scope_lo) {
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = fn->var_scope_hi = NULL;
    }

    free(live_only); free(reach); free(wl); free(l2b);
    for (int i = 0; i < nbb; i++) free(bb[i].pred);
    free(bb); free_defs(&d);
    return 1;
}

/* ---- local store-forwarding (a lightweight mem2reg) ----
 *
 * EmbIR keeps locals in memory (STVAR/LDVAR). Within an extended basic block a
 * store `STVAR L, v` makes every later `LDVAR L` yield v — until the next store
 * to L or a control-flow join. Forwarding v turns the reload into a copy that
 * copyprop/DCE then erase. Sound only for a local that is never address-taken
 * (so no aliased write can change it) and a full-width plain load (size 4 or 8,
 * no narrowing/extension between the store and the load). This is what lets an
 * inlined body's parameter plumbing (STVAR param, arg; LDVAR param) collapse to
 * the argument, so folding flows through the inline. */
static int sf_plain(int size, int sign, int w)
{
    return size == 8 || (size == 4 && !(sign && w == 8));
}

static int pass_storefwd(struct ir_func *fn)
{
    int nvars = fn->src->nvars;
    if (nvars == 0)
        return 0;
    char *taken = xcalloc((size_t)fn->nvregs, 1);
    for (int i = 0; i < fn->nins; i++)
        if (fn->ins[i].op == IR_ADDR && fn->ins[i].a >= 0 &&
            fn->ins[i].a < fn->nvregs)
            taken[fn->ins[i].a] = 1;
    int *cur = xmalloc((size_t)nvars * sizeof *cur);
    for (int v = 0; v < nvars; v++) cur[v] = -1;
    int changed = 0;
    for (int i = 0; i < fn->nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        if (in->op == IR_LABEL) {                 /* a join: values may differ */
            for (int v = 0; v < nvars; v++) cur[v] = -1;
        } else if (in->op == IR_STVAR) {
            int L = in->dst;
            if (L >= 0 && L < nvars)
                cur[L] = (!taken[L] && sf_plain(in->size, 0, in->size))
                             ? in->a : -1;
        } else if (in->op == IR_LDVAR) {
            int L = in->a;
            if (L >= 0 && L < nvars && !taken[L] && cur[L] >= 0 &&
                sf_plain(in->size, in->sign, in->w)) {
                in->op = IR_MOV;                  /* LDVAR L -> MOV of the stored temp */
                in->a = cur[L];
                in->b = -1;
                changed = 1;
            }
        }
    }
    free(taken); free(cur);
    return changed;
}

/* ---- immediate-operand folding ---- */

/* x86 ALU/compare immediates are imm32 (sign-extended to 64). A value outside
 * that range must stay in a register. */
static int fits_imm32(long v)
{
    return v >= -2147483647L - 1 && v <= 2147483647L;
}

/* The predicate when a comparison's operands are swapped: `a < b` becomes
 * `b > a`, so folding a constant `a` into `cmp b, imm` flips the direction. */
static enum binop swap_pred(enum binop p)
{
    switch (p) {
    case B_LT: return B_GT; case B_GT: return B_LT;
    case B_LE: return B_GE; case B_GE: return B_LE;
    default:   return p;   /* EQ/NE are symmetric */
    }
}

/* Fold a constant operand of an integer ALU/compare op into an immediate, so
 * the value need not be materialised in a register. Run once AFTER the main
 * fixpoint (fold/lvn/copyprop never see the imm_b form) and followed by DCE,
 * which drops the CONSTs that folding left unreferenced. */
static int pass_immfold(struct ir_func *fn)
{
    struct defs d;
    compute_defs(fn, &d);
    int changed = 0;
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        if (i->flt || i->imm_b)
            continue;
        long A, B;
        int commutative;
        /* Shifts fold only their count (b), and only a valid small one — the
         * value being shifted (a) is not an immediate operand. */
        if (i->op == IR_SHL || i->op == IR_SHR) {
            if (get_const(fn, &d, i->b, &B) && B >= 0 && B <= 63) {
                i->imm = B; i->imm_b = 1; i->b = -1;
                changed = 1;
            }
            continue;
        }
        switch (i->op) {
        case IR_ADD: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
            commutative = 1; break;
        case IR_SUB: case IR_CMP:
            commutative = 0; break;
        default:
            continue;
        }
        if (get_const(fn, &d, i->b, &B) && fits_imm32(B)) {
            i->imm = B; i->imm_b = 1; i->b = -1;    /* op a, imm */
            changed = 1;
        } else if ((commutative || i->op == IR_CMP) &&
                   get_const(fn, &d, i->a, &A) && fits_imm32(A)) {
            /* Constant in the first operand: move it to the immediate, keeping
             * a valid instruction — commutative ops just swap, a compare swaps
             * and flips its predicate. */
            i->a = i->b; i->b = -1; i->imm = A; i->imm_b = 1;
            if (i->op == IR_CMP)
                i->pred = swap_pred(i->pred);
            changed = 1;
        }
    }
    free_defs(&d);
    return changed;
}

/* ---- function inlining (inter-procedural, -O2) --------------------------- *
 *
 * A call to a small, defined function is replaced by the function's body. The
 * callee's params and locals become caller LOCALS (the memory model EmbIR uses
 * for addressable, possibly-reassigned variables — treating them as temps would
 * break codegen's single-assignment / no-alias assumptions), so the caller's
 * temps renumber UP to open a contiguous local range for them, and its
 * var_tys/var_aligns/scope metadata extend to match. Params are initialised by
 * an STVAR of each argument; every RET becomes `MOV result` + a jump to one
 * shared label after the inlined body. Gated to -O2, so -O0/-O1 are untouched. */

#define INLINE_MAX_CALLEE 24     /* instruction budget for an inline candidate */
#define INLINE_MAX_CALLER 800    /* stop expanding a caller past this many ins */
#define INLINE_MAX_PER_FUNC 64   /* and cap inlines per caller, for termination */

/* Vreg remap over one instruction. kind 0 = caller shift (a temp >= p1 moves up
 * by p2); kind 1 = callee map (a callee local < p3 -> p1+x, a temp -> p2+x). */
struct rmp { int kind, p1, p2, p3, lbase; };

static int vmap(const struct rmp *r, int x)
{
    if (x < 0) return x;
    if (r->kind == 0) return x < r->p1 ? x : x + r->p2;
    return x < r->p3 ? r->p1 + x : r->p2 + x;
}
static void rmp_cb(int *p, void *ctx) { *p = vmap(ctx, *p); }

static void remap_ins(struct ir_ins *in, struct rmp *r)
{
    each_read(in, rmp_cb, r);
    if (def_target(in) >= 0)
        in->dst = vmap(r, in->dst);
    if (in->op == IR_JMP || in->op == IR_BRZ || in->op == IR_BRNZ ||
        in->op == IR_LABEL)
        in->label += r->lbase;
}

/* The ir_func for a callee, or NULL if not defined in this unit. */
static struct ir_func *func_ir(struct ir_unit *iu, struct func *callee)
{
    for (int i = 0; i < iu->nfuncs; i++)
        if (iu->funcs[i].src == callee)
            return &iu->funcs[i];
    return NULL;
}

/* Conservative eligibility: a real, small body; scalar-integer params and
 * return only (no varargs / struct / float); no inline asm, va_start, or a
 * struct-returning call in the body. */
static int inlinable(struct ir_func *cf)
{
    struct func *c = cf->src;
    if (c->is_varargs || cf->nins == 0 || cf->nins > INLINE_MAX_CALLEE)
        return 0;
    if (c->ret_ty->kind == TY_STRUCT || ty_is_float(c->ret_ty))
        return 0;
    for (int k = 0; k < c->nvars; k++)
        if (c->var_tys[k] &&
            (c->var_tys[k]->kind == TY_STRUCT || ty_is_float(c->var_tys[k])))
            return 0;
    for (int i = 0; i < cf->nins; i++) {
        const struct ir_ins *in = &cf->ins[i];
        if (in->op == IR_ASM || in->op == IR_VA_START || in->flt)
            return 0;
        /* Computed goto: a label address / indirect jump can't be inlined —
         * the callee's label ids would need remapping into the caller, and the
         * caller then can't be optimized either (opt_func bails on it). */
        if (in->op == IR_LABELADDR || in->op == IR_IGOTO)
            return 0;
        if (in->op == IR_CALL && in->retsize)
            return 0;
    }
    return 1;
}

/* Splice the body of cf in place of the call at fn->ins[ci]. */
static void inline_call(struct ir_func *fn, int ci, struct ir_func *cf)
{
    int V = fn->src->nvars, N = fn->nvregs, L = fn->nlabels;
    int v = cf->src->nvars, n = cf->nvregs, nparams = cf->src->nparams;

    /* 1. Open room: shift the caller's temps up by v (locals stay put). */
    struct rmp shift = { 0, V, v, 0, 0 };
    for (int i = 0; i < fn->nins; i++)
        remap_ins(&fn->ins[i], &shift);

    struct ir_ins call = fn->ins[ci];    /* the (now-shifted) call */
    int dst = call.dst;
    int after = L + cf->nlabels;

    /* 2. Build param stores + the remapped body + one exit label. */
    struct ir_ins *buf = xmalloc((size_t)(nparams + 2 * cf->nins + 1) * sizeof *buf);
    int m = 0;
    for (int k = 0; k < nparams; k++) {
        struct ir_ins *s = &buf[m++];
        memset(s, 0, sizeof *s);
        s->op = IR_STVAR;
        s->dst = V + k;                  /* callee param -> caller local */
        s->a = call.argv[k].vreg;
        s->size = ty_size(cf->src->var_tys[k]);
    }
    struct rmp cm = { 1, V, N, v, L };
    for (int i = 0; i < cf->nins; i++) {
        struct ir_ins in = cf->ins[i];
        remap_ins(&in, &cm);
        if (in.op == IR_RET) {
            if (in.a >= 0 && dst >= 0) {
                struct ir_ins *mv = &buf[m++];
                memset(mv, 0, sizeof *mv);
                mv->op = IR_MOV; mv->dst = dst; mv->a = in.a;
            }
            if (i != cf->nins - 1) {     /* the last RET falls into `after` */
                struct ir_ins *jp = &buf[m++];
                memset(jp, 0, sizeof *jp);
                jp->op = IR_JMP; jp->label = after;
            }
        } else {
            buf[m++] = in;
        }
    }
    struct ir_ins *lb = &buf[m++];
    memset(lb, 0, sizeof *lb);
    lb->op = IR_LABEL; lb->label = after;

    /* 3. Splice buf over the call. */
    int newn = fn->nins - 1 + m;
    struct ir_ins *ni = xmalloc((size_t)newn * sizeof *ni);
    memcpy(ni, fn->ins, (size_t)ci * sizeof *ni);
    memcpy(ni + ci, buf, (size_t)m * sizeof *ni);
    memcpy(ni + ci + m, fn->ins + ci + 1,
           (size_t)(fn->nins - ci - 1) * sizeof *ni);
    free(fn->ins); free(buf);
    fn->ins = ni; fn->nins = newn; fn->cap = newn;

    /* 4. Grow vreg/label space and the caller's var metadata. */
    fn->nvregs = N + n;
    fn->nlabels = L + cf->nlabels + 1;
    int nv = V + v;
    struct type **vt = xmalloc((size_t)(nv ? nv : 1) * sizeof *vt);
    int *va = xmalloc((size_t)(nv ? nv : 1) * sizeof *va);
    for (int k = 0; k < V; k++) { vt[k] = fn->src->var_tys[k]; va[k] = fn->src->var_aligns[k]; }
    for (int k = 0; k < v; k++) { vt[V + k] = cf->src->var_tys[k]; va[V + k] = cf->src->var_aligns[k]; }
    fn->src->var_tys = vt;
    fn->src->var_aligns = va;

    /* Scope ranges are instruction indices; the splice inserted (m-1) net at ci.
     * Shift every existing endpoint past ci, and scope the new callee locals to
     * the inlined region. Kept precise so local-slot coalescing still works. */
    if (fn->var_scope_lo) {
        int *lo = xmalloc((size_t)nv * sizeof *lo);
        int *hi = xmalloc((size_t)nv * sizeof *hi);
        int d = m - 1;
        for (int k = 0; k < V; k++) {
            int a = fn->var_scope_lo[k], b = fn->var_scope_hi[k];
            lo[k] = a <= ci ? a : a + d;
            hi[k] = b <= ci ? b : b + d;
        }
        for (int k = 0; k < v; k++) { lo[V + k] = ci; hi[V + k] = ci + m; }
        free(fn->var_scope_lo); free(fn->var_scope_hi);
        fn->var_scope_lo = lo; fn->var_scope_hi = hi;
    }
    fn->src->nvars = nv;
}

/* Inline eligible calls across the unit (a bounded fixpoint per caller). */
static void inline_unit(struct ir_unit *iu)
{
    for (int f = 0; f < iu->nfuncs; f++) {
        struct ir_func *fn = &iu->funcs[f];
        int done = 0;
        for (;;) {
            if (done >= INLINE_MAX_PER_FUNC || fn->nins > INLINE_MAX_CALLER)
                break;
            int ci = -1;
            struct ir_func *cf = NULL;
            for (int i = 0; i < fn->nins; i++) {
                struct ir_ins *in = &fn->ins[i];
                if (in->op != IR_CALL || in->indirect || !in->callee ||
                    in->retsize)
                    continue;
                struct ir_func *c = func_ir(iu, in->callee);
                if (c && c != fn && inlinable(c)) { ci = i; cf = c; break; }
            }
            if (ci < 0)
                break;
            inline_call(fn, ci, cf);
            done++;
        }
    }
}

/* ---- driver ---- */

static int g_mem2reg;   /* -O2: promote locals to SSA before the fixpoint */
static int g_gcse;      /* -O2: dominator-scoped global CSE inside the fixpoint */
static int g_loadcse;   /* -O2: global redundant-load elimination (avail. exprs) */
static int g_sccp;      /* -O2: const-branch resolution + unreachable-block drop */

/* ---- IR verifier (opt-in via EMBCC_VERIFY) --------------------------------
 * A cheap post-optimization sanity net for the two invariants a silent
 * miscompile breaks, and which the 99-test suite did NOT catch when var_scope
 * went stale: (1) every TEMP a surviving instruction reads still has a
 * definition — a pass that drops a live value trips this; (2) var_scope_lo/hi,
 * when present, index into the CURRENT instruction stream — a pass that
 * renumbers instructions without remapping (the DCE bug) trips this. Off by
 * default so normal builds pay nothing; the test suite runs with it set.
 * Aborts loudly (THE RULE) rather than let wrong code through. */
struct vrfy { struct defs *d; int np, nv; struct ir_func *fn; const char *tag; };
static void vrfy_read_cb(int *p, void *ctx)
{
    struct vrfy *v = ctx;
    int r = *p;
    if (r < 0 || r < v->nv)          /* param or local: a local may be read uninit'd */
        return;
    if (r < v->fn->nvregs && v->d->cnt[r] > 0)   /* a temp with a definition: fine */
        return;
    diag_fatal(v->fn->src->file, 0,
        "internal: %s reads temp %%%d with no definition (after %s) — an optimizer "
        "pass dropped a value that is still used", v->fn->src->name, r, v->tag);
}
static void verify_func(struct ir_func *fn, const char *tag)
{
    struct defs d;
    compute_defs(fn, &d);
    struct vrfy v = { &d, fn->src->nparams, fn->src->nvars, fn, tag };
    for (int n = 0; n < fn->nins; n++)
        each_read(&fn->ins[n], vrfy_read_cb, &v);
    if (fn->var_scope_lo)
        for (int i = 0; i < fn->src->nvars; i++) {
            int lo = fn->var_scope_lo[i], hi = fn->var_scope_hi[i];
            if (lo < 0 || lo > fn->nins || hi < lo || hi > fn->nins)
                diag_fatal(fn->src->file, 0,
                    "internal: %s local %d has out-of-range scope [%d,%d] for nins=%d "
                    "(after %s) — a pass renumbered instructions without remapping "
                    "var_scope", fn->src->name, i, lo, hi, fn->nins, tag);
        }
    free_defs(&d);
}

static void opt_func(struct ir_func *fn)
{
    /* Computed goto (`goto *p`) makes the CFG imprecise — an indirect jump can
     * reach any address-taken label — which the dominance/liveness passes are
     * not built to model. Such functions are rare; leave them unoptimized
     * (still correct, memory-model codegen) rather than risk a mis-analysis. */
    for (int n = 0; n < fn->nins; n++)
        if (fn->ins[n].op == IR_IGOTO || fn->ins[n].op == IR_LABELADDR)
            return;
    int verify = getenv("EMBCC_VERIFY") != NULL;
    if (verify) verify_func(fn, "irgen");
    if (g_mem2reg)
        pass_mem2reg(fn);         /* global mem2reg (subsumes store-forwarding) */
    /* Global load CSE is the expensive pass (CFG + an available-expressions
     * dataflow), so it runs ONCE per outer round instead of on every inner
     * iteration. When it exposes copies, the inner fixpoint reconverges and we
     * round again — it settles in one or two rounds. */
    int outer = 1, oguard = 0;
    while (outer && oguard++ < 100) {
        outer = 0;
        int changed = 1, guard = 0;
        while (changed && guard++ < 1000) {
            changed = 0;
            changed |= pass_storefwd(fn); /* forward local stores to loads (mem2reg-lite) */
            changed |= pass_fold(fn);
            changed |= pass_lvn(fn);      /* CSE: reuse identical computations */
            if (g_gcse)
                changed |= pass_gcse(fn); /* CSE across the dominator tree */
            if (g_sccp)
                changed |= pass_sccp(fn); /* resolve const branches, drop dead blocks */
            changed |= pass_copyprop(fn);
            changed |= pass_dce(fn);
        }
        if (g_loadcse && pass_loadcse(fn)) {   /* reuse loads redundant on every path */
            pass_copyprop(fn);
            pass_dce(fn);
            outer = 1;
        }
    }
    /* After the fixpoint: fold constant operands into immediates, then DCE the
     * CONSTs that leaves unreferenced. Kept out of the fixpoint so the earlier
     * passes never reason about the imm_b form. */
    if (pass_immfold(fn))
        pass_dce(fn);
    if (verify) verify_func(fn, "opt");
}

void opt_run(struct ir_unit *iu, int level)
{
    if (level < 1)
        return;
    g_mem2reg = level >= 2;       /* SSA mem2reg: promote scalar locals to temps */
    g_gcse = level >= 2;          /* global CSE across the dominator tree */
    g_loadcse = level >= 2;       /* global redundant-load elimination */
    g_sccp = level >= 2;          /* conditional constant propagation */
    if (level >= 2)               /* inline before the per-function passes clean up */
        inline_unit(iu);
    for (int f = 0; f < iu->nfuncs; f++)
        opt_func(&iu->funcs[f]);
}
