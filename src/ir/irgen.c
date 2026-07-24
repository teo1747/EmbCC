#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../sema/sema.h"
#include "../sema/type.h"

static struct ir_ins *emit(struct ir_func *fn)
{
    if (fn->nins == fn->cap) {
        fn->cap = fn->cap ? fn->cap * 2 : 16;
        fn->ins = xrealloc(fn->ins, (size_t)fn->cap * sizeof *fn->ins);
    }
    struct ir_ins *i = &fn->ins[fn->nins++];
    i->op = IR_CONST;
    i->dst = i->a = i->b = -1;
    i->w = 4;
    i->size = 4;
    i->sign = 1;
    i->imm = 0;
    i->pred = B_ADD;
    i->label = -1;
    i->callee = 0;
    i->nargs = 0;
    return i;
}

static int new_temp(struct ir_func *fn) { return fn->nvregs++; }
static int new_label(struct ir_func *fn) { return fn->nlabels++; }

static int ty_w(const struct type *t) { return ty_wide(t) ? 8 : 4; }

static void emit_label(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LABEL;
    i->label = label;
}

static void emit_jmp(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_JMP;
    i->label = label;
}

static void emit_brz(struct ir_func *fn, int v, int w, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRZ;
    i->a = v;
    i->w = w;
    i->label = label;
}

static void emit_brnz(struct ir_func *fn, int v, int w, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRNZ;
    i->a = v;
    i->w = w;
    i->label = label;
}

/* A floating constant is just its BIT PATTERN moved into the slot: a
 * float temp's slot holds raw bits, so no xmm and no constant pool are
 * involved. Same reason loads and stores need no float path. */
static int emit_fconst(struct ir_func *fn, double d, int w)
{
    long bits = 0;
    if (w == 8) {
        double v = d;
        memcpy(&bits, &v, 8);
    } else {
        float v = (float)d;
        unsigned int u = 0;
        memcpy(&u, &v, 4);
        bits = (long)u;
    }
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = bits;
    i->w = w;
    i->dst = new_temp(fn);
    return i->dst;
}

static int emit_const(struct ir_func *fn, long imm, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = imm;
    i->w = w;
    i->dst = new_temp(fn);
    return i->dst;
}

/* dst = a op b at width w; returns dst */
static int emit_bin(struct ir_func *fn, enum ir_op op, int a, int b,
                    int w, int sign)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->sign = sign;
    i->dst = new_temp(fn);
    return i->dst;
}

static int emit_fbin(struct ir_func *fn, enum ir_op op, int a, int b, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->flt = 1;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Load variable v (type t) into a fresh promoted temp. */
static int emit_ldvar(struct ir_func *fn, int v, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LDVAR;
    i->a = v;
    i->size = ty_size(t);
    i->sign = ty_signed_int(t);
    i->w = ty_w(t);
    i->dst = new_temp(fn);
    return i->dst;
}

static void emit_stvar(struct ir_func *fn, int v, int val,
                       const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_STVAR;
    i->dst = v;
    i->a = val;
    i->size = ty_size(t);
}

static int emit_gaddr(struct ir_func *fn, struct global *g)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_GADDR;
    i->glob = g;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Typed load/store through an address temp. */
static int emit_load(struct ir_func *fn, int addr, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LOAD;
    i->a = addr;
    i->size = ty_size(t);
    i->sign = ty_signed_int(t);
    i->w = ty_w(t);
    i->dst = new_temp(fn);
    return i->dst;
}

static void emit_store(struct ir_func *fn, int addr, int val,
                       const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_STORE;
    i->a = addr;
    i->b = val;
    i->size = ty_size(t);
}

static int gen_expr(struct ir_func *fn, struct expr *e);

/* The address of an lvalue (or of a struct-typed expression — struct
 * "values" are represented by their address, since sema bars them from
 * every value context). */
static int gen_addr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_VAR:
        if (e->gref)
            return emit_gaddr(fn, e->gref);
        {
            struct ir_ins *i = emit(fn);
            i->op = IR_ADDR;
            i->a = e->var_index;
            i->dst = new_temp(fn);
            return i->dst;
        }
    case EXPR_DEREF:
        return gen_expr(fn, e->rhs);
    case EXPR_MEMBER: {
        int base = e->is_arrow ? gen_expr(fn, e->lhs)
                               : gen_addr(fn, e->lhs);
        if (e->memb->off == 0)
            return base;
        int off = emit_const(fn, e->memb->off, 8);
        return emit_bin(fn, IR_ADD, base, off, 8, 1);
    }
    default:
        fprintf(stderr, "embcc: internal: address of a non-lvalue\n");
        exit(1);
    }
}

static int log2_size(int size)
{
    switch (size) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    }
    fprintf(stderr, "embcc: internal: bad object size %d\n", size);
    exit(1);
}

static int gen_expr(struct ir_func *fn, struct expr *e);

/* The unit being generated — for the string table. One compilation per
 * process, so a file-scope current-unit pointer is honest. */
static struct ir_unit *cur_unit;

/* Intern a string into the unit's .rodata pool, returning its index.
 * Deduping means a literal shared by code and a global initializer lands
 * once. */
int ir_intern_string(struct ir_unit *iu, const char *bytes, int len)
{
    for (int i = 0; i < iu->nstrs; i++)
        if (iu->strs[i].len == len &&
            memcmp(iu->strs[i].bytes, bytes, (size_t)len) == 0)
            return i;
    if (iu->nstrs == iu->capstrs) {
        iu->capstrs = iu->capstrs ? iu->capstrs * 2 : 8;
        iu->strs = xrealloc(iu->strs,
                            (size_t)iu->capstrs * sizeof *iu->strs);
    }
    struct ir_str *s = &iu->strs[iu->nstrs];
    s->bytes = bytes;
    s->len = len;
    s->off = iu->rodata_len;
    iu->rodata_len += len;
    return iu->nstrs++;
}

static int intern_str(const char *bytes, int len)
{
    return ir_intern_string(cur_unit, bytes, len);
}

/* !x and conditions want "is zero" — comparison against a zero of the
 * operand's width. */
static int emit_isz(struct ir_func *fn, int v, int w)
{
    int zero = emit_const(fn, 0, w);
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = B_EQ;
    i->a = v;
    i->b = zero;
    i->w = w;
    i->sign = 1;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Change a temp's representation between type classes: truncating to a
 * narrow type re-extends from its low bytes; widening extends per the
 * SOURCE's signedness. Free conversions return the same temp. */
static int gen_convert(struct ir_func *fn, int v, const struct type *from,
                       const struct type *to)
{
    int fsize = ty_size(from), tsize = ty_size(to);
    int fw = ty_w(from), tw = ty_w(to);

    /* Floating conversions are real instructions, not reinterpretations
     * — the bit patterns have nothing in common. */
    if (ty_is_float(from) || ty_is_float(to)) {
        struct ir_ins *i;
        if (ty_is_float(from) && ty_is_float(to)) {
            if (fsize == tsize)
                return v;
            i = emit(fn);
            i->op = IR_F2F;
            i->a = v;
            i->size = fsize;
            i->w = tsize;
            i->dst = new_temp(fn);
            return i->dst;
        }
        if (ty_is_float(to)) {
            /* int -> float, and the source WIDTH matters: a 32-bit
             * operation zero-extends its result into the 8-byte slot
             * regardless of signedness, so a negative int read back as
             * 64 bits is 2^32 too large. Read a signed 32-bit source as
             * 32 bits and let cvtsi2sd interpret the sign; read an
             * unsigned int as 64, where the zero extension IS the value
             * (which is what makes it exact). unsigned long is refused
             * by sema — SSE2 cannot do it. */
            int srcw = 4;
            if (ty_wide(from) ||
                (from->is_unsigned && ty_size(from) == 4))
                srcw = 8;
            i = emit(fn);
            i->op = IR_I2F;
            i->a = v;
            i->size = srcw;
            i->sign = 1;
            i->w = tsize;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* float -> int: truncates toward zero, as C requires. Convert
         * to the 64-bit form then narrow, so unsigned int lands right. */
        i = emit(fn);
        i->op = IR_F2I;
        i->a = v;
        i->size = fsize;
        i->w = 8;
        i->sign = 1;
        i->dst = new_temp(fn);
        int iv = i->dst;
        if (tsize <= 2) {
            struct ir_ins *x = emit(fn);
            x->op = IR_EXT;
            x->a = iv;
            x->size = tsize;
            x->sign = ty_signed_int(to);
            x->w = 4;
            x->dst = new_temp(fn);
            return x->dst;
        }
        return iv;
    }

    if (tsize <= 2) {
        /* to char/short: truncate + extend per TARGET's signedness */
        struct ir_ins *i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        i->size = tsize;
        i->sign = ty_signed_int(to);
        i->w = 4;
        i->dst = new_temp(fn);
        return i->dst;
    }
    if (tw == 8 && fw == 4) {
        /* int class -> long/pointer: extend per SOURCE signedness;
         * narrow sources were already promoted, so extend from 32. */
        struct ir_ins *i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        i->size = 4;
        i->sign = fsize <= 2 ? 1 : ty_signed_int(from);
        i->w = 8;
        i->dst = new_temp(fn);
        return i->dst;
    }
    /* long->int (read low 32), ptr<->long, same class: free */
    return v;
}

static int gen_expr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
        return emit_const(fn, e->num, ty_w(e->ty));
    case EXPR_FNUM:
        return emit_fconst(fn, e->fnum, ty_size(e->ty));
    case EXPR_STR: {
        e->str_index = intern_str(e->name, (int)e->num);
        struct ir_ins *i = emit(fn);
        i->op = IR_STRADDR;
        i->label = e->str_index;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_VAR:
        if (e->fref) { /* function designator: its address */
            struct ir_ins *i = emit(fn);
            i->op = IR_FADDR;
            i->callee = e->fref;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* arrays and structs are represented by their address */
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return gen_addr(fn, e);
        if (e->gref)
            return emit_load(fn, emit_gaddr(fn, e->gref), e->ty);
        return emit_ldvar(fn, e->var_index, e->ty);
    case EXPR_MEMBER: {
        int addr = gen_addr(fn, e);
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr; /* array member decays; nested struct is addr */
        return emit_load(fn, addr, e->ty);
    }
    case EXPR_ASSIGN: {
        if (e->ty->kind == TY_STRUCT) {
            /* a struct assignment is a copy of its bytes */
            int dst = gen_addr(fn, e->lhs);
            int src = gen_expr(fn, e->rhs); /* structs ARE addresses */
            struct ir_ins *i = emit(fn);
            i->op = IR_MEMCPY;
            i->a = dst;
            i->b = src;
            i->size = ty_size(e->ty);
            return dst;
        }
        if (e->lhs->kind == EXPR_VAR && !e->lhs->gref) {
            int v = gen_expr(fn, e->rhs);
            emit_stvar(fn, e->lhs->var_index, v, e->ty);
            return v;
        }
        /* global, *p, or member: a store through an address */
        int addr = gen_addr(fn, e->lhs);
        int v = gen_expr(fn, e->rhs);
        emit_store(fn, addr, v, e->ty);
        return v;
    }
    case EXPR_INCDEC: {
        struct type *t = e->ty;
        int scale = t->kind == TY_PTR ? ty_size(t->pointee) : 1;
        int w = ty_w(t);
        int local = e->lhs->kind == EXPR_VAR && !e->lhs->gref;
        int addr = local ? -1 : gen_addr(fn, e->lhs);
        int cur = local ? emit_ldvar(fn, e->lhs->var_index, t)
                        : emit_load(fn, addr, t);
        int old = -1;
        if (e->is_post) {
            struct ir_ins *save = emit(fn);
            save->op = IR_MOV;
            save->a = cur;
            save->dst = old = new_temp(fn);
        }
        int d = emit_const(fn, (long)e->delta * scale, w);
        int sum = emit_bin(fn, IR_ADD, cur, d, w, 1);
        if (ty_size(t) <= 2) {
            /* ++c on a char must wrap like a char, in the value too */
            struct ir_ins *i = emit(fn);
            i->op = IR_EXT;
            i->a = sum;
            i->size = ty_size(t);
            i->sign = ty_signed_int(t);
            i->w = 4;
            i->dst = new_temp(fn);
            sum = i->dst;
        }
        if (local)
            emit_stvar(fn, e->lhs->var_index, sum, t);
        else
            emit_store(fn, addr, sum, t);
        return e->is_post ? old : sum;
    }
    case EXPR_NOT: {
        int v = gen_expr(fn, e->rhs);
        return emit_isz(fn, v, ty_w(e->rhs->ty));
    }
    case EXPR_NEG:
    case EXPR_BNOT: {
        int v = gen_expr(fn, e->rhs);
        if (e->kind == EXPR_NEG && ty_is_float(e->ty)) {
            /* -x on a float flips the sign BIT: exact for -0.0 and for
             * NaN, which 0.0-x is not, and it needs no new instruction
             * because the slot already holds the pattern. */
            int sz = ty_size(e->ty);
            int mask = emit_const(fn,
                                  sz == 8 ? (long)0x8000000000000000LL
                                          : (long)0x80000000L, sz);
            return emit_bin(fn, IR_XOR, v, mask, sz, 0);
        }
        struct ir_ins *i = emit(fn);
        i->op = e->kind == EXPR_NEG ? IR_NEG : IR_BNOT;
        i->a = v;
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_DEREF: {
        int addr = gen_expr(fn, e->rhs);
        /* *fp is fp (the OPERAND points at a function — nothing to
         * load); rows of 2-D arrays and structs are addresses too */
        if (e->rhs->ty->pointee->kind == TY_FUNC)
            return addr;
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr;
        struct ir_ins *i = emit(fn);
        i->op = IR_LOAD;
        i->a = addr;
        i->size = ty_size(e->ty);
        i->sign = ty_signed_int(e->ty);
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_ADDR:
        return gen_addr(fn, e->rhs);
    case EXPR_CAST: {
        int v = gen_expr(fn, e->rhs);
        if (e->ty->kind == TY_VOID)
            return v; /* evaluated for its effect; the value is dropped */
        return gen_convert(fn, v, e->rhs->ty, e->ty);
    }
    case EXPR_SIZEOF:
        break; /* folded to EXPR_NUM by sema; unreachable */
    case EXPR_BINOP: {
        struct type *lt = e->lhs->ty, *rt = e->rhs->ty;

        switch (e->op) {
        case B_LAND:
        case B_LOR: {
            /* Short-circuit: the right side must not run when the left
             * decides — observable through calls. */
            int dst = new_temp(fn);
            int l_short = new_label(fn);
            int l_end = new_label(fn);
            int a = gen_expr(fn, e->lhs);
            int aw = ty_w(lt);
            if (e->op == B_LAND) {
                emit_brz(fn, a, aw, l_short);
                int b = gen_expr(fn, e->rhs);
                int nz = emit_isz(fn, b, ty_w(rt));
                int one = emit_isz(fn, nz, 4); /* !!b */
                struct ir_ins *m = emit(fn);
                m->op = IR_MOV;
                m->a = one;
                m->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_short);
                struct ir_ins *z = emit(fn);
                z->op = IR_CONST;
                z->imm = 0;
                z->w = 4;
                z->dst = dst;
            } else {
                int l_rhs = new_label(fn);
                emit_brz(fn, a, aw, l_rhs);
                struct ir_ins *o = emit(fn);
                o->op = IR_CONST;
                o->imm = 1;
                o->w = 4;
                o->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_rhs);
                int b = gen_expr(fn, e->rhs);
                int nz = emit_isz(fn, b, ty_w(rt));
                int one = emit_isz(fn, nz, 4); /* !!b */
                struct ir_ins *m = emit(fn);
                m->op = IR_MOV;
                m->a = one;
                m->dst = dst;
            }
            emit_label(fn, l_end);
            return dst;
        }
        case B_ADD:
        case B_SUB: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp && rp) {
                /* ptr - ptr: byte difference, scaled down */
                int a = gen_expr(fn, e->lhs);
                int b = gen_expr(fn, e->rhs);
                int diff = emit_bin(fn, IR_SUB, a, b, 8, 1);
                int sh = log2_size(ty_size(lt->pointee));
                if (!sh)
                    return diff;
                int c = emit_const(fn, sh, 4);
                return emit_bin(fn, IR_SHR, diff, c, 8, 1);
            }
            if (lp || rp) {
                /* ptr +/- int: scale the (already long) index */
                struct expr *pe = lp ? e->lhs : e->rhs;
                struct expr *ie = lp ? e->rhs : e->lhs;
                int p = gen_expr(fn, lp ? pe : ie);
                int idx = gen_expr(fn, lp ? ie : pe);
                if (!lp) {
                    int t = p;
                    p = idx;
                    idx = t;
                }
                int size = ty_size((lp ? lt : rt)->pointee);
                if (size > 1) {
                    int c = emit_const(fn, size, 8);
                    idx = emit_bin(fn, IR_MUL, idx, c, 8, 1);
                }
                return emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB,
                                p, idx, 8, 1);
            }
            /* plain arithmetic */
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            enum ir_op o = e->op == B_ADD ? IR_ADD : IR_SUB;
            if (ty_is_float(e->ty))
                return emit_fbin(fn, o, a, b, ty_size(e->ty));
            return emit_bin(fn, o, a, b, ty_w(e->ty),
                            ty_signed_int(e->ty));
        }
        case B_EQ:
        case B_NE:
        case B_LT:
        case B_LE:
        case B_GT:
        case B_GE: {
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            struct ir_ins *i = emit(fn);
            i->op = IR_CMP;
            i->pred = e->op;
            i->a = a;
            i->b = b;
            if (ty_is_float(lt)) {
                i->flt = 1;
                i->w = ty_size(lt);
            } else {
                i->w = ty_w(lt);
            }
            /* pointers compare unsigned, as C requires */
            i->sign = ty_signed_int(lt);
            i->dst = new_temp(fn);
            return i->dst;
        }
        default: {
            static const enum ir_op map[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
            };
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            if (ty_is_float(e->ty))
                return emit_fbin(fn, map[e->op - B_ADD], a, b,
                                 ty_size(e->ty));
            return emit_bin(fn, map[e->op - B_ADD], a, b,
                            ty_w(e->ty), ty_signed_int(e->ty));
        }
        }
    }
    case EXPR_COMMA:
        gen_expr(fn, e->lhs); /* for side effects */
        return gen_expr(fn, e->rhs);
    case EXPR_INITLIST:
        break; /* consumed by sema's flattening; never evaluated */
    case EXPR_COMPOUND: {
        /* the address is computed ONCE — the whole reason this is not
         * desugared to `x = x op y` */
        struct type *lt = e->lhs->ty;
        int local = e->lhs->kind == EXPR_VAR && !e->lhs->gref;
        int addr = local ? -1 : gen_addr(fn, e->lhs);
        int cur = local ? emit_ldvar(fn, e->lhs->var_index, lt)
                        : emit_load(fn, addr, lt);
        int rv = gen_expr(fn, e->rhs);
        int res;
        if (lt->kind == TY_PTR) {
            int esz = ty_size(lt->pointee);
            if (esz > 1) {
                int k = emit_const(fn, esz, 8);
                rv = emit_bin(fn, IR_MUL, rv, k, 8, 1);
            }
            res = emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB, cur, rv,
                           8, 1);
        } else {
            struct type *ct = e->cast_ty;
            int cv = gen_convert(fn, cur, lt, ct);
            static const enum ir_op map[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
            };
            enum ir_op o = map[e->op - B_ADD];
            if (ty_is_float(ct))
                res = emit_fbin(fn, o, cv, rv, ty_size(ct));
            else
                res = emit_bin(fn, o, cv, rv, ty_w(ct),
                               ty_signed_int(ct));
            res = gen_convert(fn, res, ct, lt);
        }
        if (local)
            emit_stvar(fn, e->lhs->var_index, res, lt);
        else
            emit_store(fn, addr, res, lt);
        return res;
    }
    case EXPR_COND: {
        int dst = new_temp(fn);
        int l_else = new_label(fn);
        int l_end = new_label(fn);
        int c = gen_expr(fn, e->args[0]);
        emit_brz(fn, c, ty_w(e->args[0]->ty), l_else);
        int a = gen_expr(fn, e->lhs);
        struct ir_ins *m1 = emit(fn);
        m1->op = IR_MOV;
        m1->a = a;
        m1->dst = dst;
        emit_jmp(fn, l_end);
        emit_label(fn, l_else);
        int b = gen_expr(fn, e->rhs);
        struct ir_ins *m2 = emit(fn);
        m2->op = IR_MOV;
        m2->a = b;
        m2->dst = dst;
        emit_label(fn, l_end);
        return dst;
    }
    case EXPR_CALL: {
        int args[MAX_PARAMS];
        int fptemp = -1;
        if (!e->callee) /* through a pointer: evaluate the callee */
            fptemp = gen_expr(fn, e->lhs);
        for (int k = 0; k < e->nargs; k++)
            args[k] = gen_expr(fn, e->args[k]);
        struct ir_ins *i = emit(fn);
        i->op = IR_CALL;
        i->callee = e->callee;
        i->indirect = !e->callee;
        i->a = fptemp;
        i->call_varargs = e->callee ? e->callee->is_varargs
                                    : e->lhs->ty->pointee->is_varargs;
        i->nargs = e->nargs;

        /* Classify every argument here, where the types still exist;
         * codegen only places what it is told. MEMORY-class arguments
         * are assigned offsets in this call's outgoing area, and the
         * function's frame reserves the widest such area. */
        int stk = 0;
        int ireg = 0, freg = 0;
        /* a hidden return pointer consumes rdi before anything else */
        if (e->ty->kind == TY_STRUCT) {
            enum arg_class rc[2];
            if (ty_classify(e->ty, rc) == 0)
                ireg = 1;
        }
        for (int k = 0; k < e->nargs; k++) {
            struct type *at = e->args[k]->ty;
            struct ir_arg *ar = &i->argv[k];
            ar->vreg = args[k];
            ar->is_struct = at->kind == TY_STRUCT;
            ar->size = ty_size(at);
            ar->nclass = ty_classify(at, ar->cls);
            ar->stk_off = 0;
            ar->on_stack = 0;

            /* SysV: an argument goes on the stack when its class has no
             * registers left for ALL of its eightbytes — the decision is
             * made here so codegen only follows it, and the two cannot
             * drift apart. */
            int ni = 0, nf = 0;
            for (int q = 0; q < ar->nclass; q++) {
                if (ar->cls[q] == CLASS_SSE)
                    nf++;
                else
                    ni++;
            }
            if (ar->nclass == 0 || ireg + ni > 6 || freg + nf > 8) {
                ar->on_stack = 1;
                stk = (stk + 7) & ~7;
                ar->stk_off = stk;
                stk += (ar->size + 7) & ~7;
            } else {
                ireg += ni;
                freg += nf;
            }
        }
        if (stk > fn->outgoing_bytes)
            fn->outgoing_bytes = stk;

        if (e->ty->kind == TY_STRUCT) {
            i->retsize = ty_size(e->ty);
            i->retnclass = ty_classify(e->ty, i->retcls);
            fn->scratch_bytes = (fn->scratch_bytes + 7) & ~7;
            i->scratch = fn->scratch_bytes;
            fn->scratch_bytes += (i->retsize + 7) & ~7;
        }
        i->flt = ty_is_float(e->ty);
        i->w = i->flt ? ty_size(e->ty) : 8;
        i->dst = new_temp(fn);
        return i->dst;
    }
    }
    return -1; /* unreachable; every kind returns above */
}

/* Innermost enclosing loop's exit and continue targets; sema already
 * rejected break/continue outside any loop. */
struct loopctx {
    int brk, cont;
};

static void gen_stmt(struct ir_func *fn, struct stmt *s,
                     const struct loopctx *loop)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_BREAK:
            emit_jmp(fn, loop->brk);
            break;
        case STMT_CONTINUE:
            emit_jmp(fn, loop->cont);
            break;
        case STMT_DECL:
            if (s->sglob)
                break; /* a static local IS its global; no code here */
            if (s->ninits) {
                /* C zero-fills whatever the initializer does not
                 * mention, so clear the object first and then place
                 * the listed values. */
                struct ir_ins *ad = emit(fn);
                ad->op = IR_ADDR;
                ad->a = s->var_index;
                ad->dst = new_temp(fn);
                int base = ad->dst;
                struct ir_ins *z = emit(fn);
                z->op = IR_MEMZERO;
                z->a = base;
                z->size = ty_size(s->dty);
                for (int k = 0; k < s->ninits; k++) {
                    int v = gen_expr(fn, s->inits[k].e);
                    int at = base;
                    if (s->inits[k].off) {
                        int o = emit_const(fn, s->inits[k].off, 8);
                        at = emit_bin(fn, IR_ADD, base, o, 8, 1);
                    }
                    if (s->inits[k].ty->kind == TY_STRUCT) {
                        struct ir_ins *m = emit(fn);
                        m->op = IR_MEMCPY;
                        m->a = at;
                        m->b = v;
                        m->size = ty_size(s->inits[k].ty);
                    } else {
                        emit_store(fn, at, v, s->inits[k].ty);
                    }
                }
                break;
            }
            if (s->expr) {
                int v = gen_expr(fn, s->expr);
                if (s->dty->kind == TY_STRUCT) {
                    /* initializing a struct is the same byte copy an
                     * assignment is */
                    struct ir_ins *a = emit(fn);
                    a->op = IR_ADDR;
                    a->a = s->var_index;
                    a->dst = new_temp(fn);
                    struct ir_ins *i = emit(fn);
                    i->op = IR_MEMCPY;
                    i->a = a->dst;
                    i->b = v;
                    i->size = ty_size(s->dty);
                } else {
                    emit_stvar(fn, s->var_index, v, s->dty);
                }
            }
            break;
        case STMT_EXPR:
            gen_expr(fn, s->expr); /* value discarded */
            break;
        case STMT_RETURN: {
            struct ir_ins *i;
            int v = s->expr ? gen_expr(fn, s->expr) : -1;
            i = emit(fn);
            i->op = IR_RET;
            i->a = v;
            if (s->expr && ty_is_float(s->expr->ty)) {
                i->flt = 1; /* the value goes home in xmm0, not rax */
                i->w = ty_size(s->expr->ty);
            } else if (s->expr && s->expr->ty->kind == TY_STRUCT) {
                /* `a` is the ADDRESS of the value; how it travels home
                 * is the callee's classification, computed in codegen
                 * from the function's own return type. */
                i->size = ty_size(s->expr->ty);
            }
            break;
        }
        case STMT_IF: {
            int l_else = new_label(fn);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, ty_w(s->cond->ty), l_else);
            gen_stmt(fn, s->thn, loop);
            if (s->els) {
                int l_end = new_label(fn);
                emit_jmp(fn, l_end);
                emit_label(fn, l_else);
                gen_stmt(fn, s->els, loop);
                emit_label(fn, l_end);
            } else {
                emit_label(fn, l_else);
            }
            break;
        }
        case STMT_DO: {
            /* body first, THEN the test — the whole point of do-while;
             * continue re-tests, so it targets the condition. */
            struct loopctx lc;
            int l_top = new_label(fn);
            lc.cont = new_label(fn);
            lc.brk = new_label(fn);
            emit_label(fn, l_top);
            gen_stmt(fn, s->body, &lc);
            emit_label(fn, lc.cont);
            int c = gen_expr(fn, s->cond);
            emit_brnz(fn, c, ty_w(s->cond->ty), l_top);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_CASE:
        case STMT_DEFAULT:
            emit_label(fn, s->label);
            break;
        case STMT_SWITCH: {
            /* A compare-and-branch chain: correct and slow, the house
             * rule (ARCHITECTURE §3). A jump table is an OPTIMIZATION
             * and belongs to the optimizer era, not here. */
            struct loopctx lc;
            lc.brk = new_label(fn);
            /* continue inside a switch belongs to the enclosing LOOP;
             * sema has already refused it when there is none. */
            lc.cont = loop ? loop->cont : -1;

            int v = gen_expr(fn, s->cond);
            int w = ty_w(s->cond->ty);
            int sign = ty_signed_int(s->cond->ty);
            struct stmt *list = switch_stmts(s->body);
            int dflt = -1;

            for (struct stmt *c = list; c; c = c->next) {
                if (c->kind == STMT_DEFAULT) {
                    c->label = new_label(fn);
                    dflt = c->label;
                    continue;
                }
                if (c->kind != STMT_CASE)
                    continue;
                c->label = new_label(fn);
                int k = emit_const(fn, c->cval, w);
                struct ir_ins *i = emit(fn);
                i->op = IR_CMP;
                i->pred = B_EQ;
                i->a = v;
                i->b = k;
                i->w = w;
                i->sign = sign;
                i->dst = new_temp(fn);
                emit_brnz(fn, i->dst, 4, c->label);
            }
            emit_jmp(fn, dflt >= 0 ? dflt : lc.brk);
            gen_stmt(fn, list, &lc); /* fallthrough is just: no jumps */
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_WHILE: {
            struct loopctx lc;
            lc.cont = new_label(fn); /* while: continue re-tests */
            lc.brk = new_label(fn);
            emit_label(fn, lc.cont);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, ty_w(s->cond->ty), lc.brk);
            gen_stmt(fn, s->body, &lc);
            emit_jmp(fn, lc.cont);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_FOR: {
            /* for: continue jumps to the STEP, not the condition. */
            int l_cond = new_label(fn);
            struct loopctx lc;
            lc.cont = new_label(fn);
            lc.brk = new_label(fn);
            if (s->initdecl)
                gen_stmt(fn, s->initdecl, &lc);
            if (s->init)
                gen_expr(fn, s->init);
            emit_label(fn, l_cond);
            if (s->cond) { /* NULL = forever, left by break */
                int c = gen_expr(fn, s->cond);
                emit_brz(fn, c, ty_w(s->cond->ty), lc.brk);
            }
            gen_stmt(fn, s->body, &lc);
            emit_label(fn, lc.cont);
            if (s->step)
                gen_expr(fn, s->step);
            emit_jmp(fn, l_cond);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_BLOCK:
            gen_stmt(fn, s->body, loop);
            break;
        }
    }
}

static void gen_func(struct ir_func *fn, struct func *f)
{
    fn->src = f;
    fn->nvregs = f->nvars; /* params + locals occupy [0, nvars) */
    gen_stmt(fn, f->body, NULL);
}

struct ir_unit *irgen(struct unit *u)
{
    struct ir_unit *iu = xcalloc(1, sizeof *iu);
    iu->src = u;
    cur_unit = iu;

    /* Only canonical, defined functions produce code; prototypes of
     * externals produce symbols and relocations instead (driver).
     * UNUSED static functions are skipped entirely — headers define
     * static inline helpers wholesale (newlib stdio does), and
     * emitting the unused ones would drag their callees into every
     * link. Internal linkage makes this invisible to other objects. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn &&
            (!f->is_static || f->used || strcmp(f->name, "main") == 0))
            iu->nfuncs++;
    iu->funcs = xcalloc((size_t)iu->nfuncs, sizeof *iu->funcs);

    int n = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn &&
            (!f->is_static || f->used || strcmp(f->name, "main") == 0))
            gen_func(&iu->funcs[n++], f);
    return iu;
}
