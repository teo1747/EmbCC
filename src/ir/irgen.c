#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
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

static int intern_str(const char *bytes, int len)
{
    struct ir_unit *iu = cur_unit;
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
    case EXPR_STR: {
        e->str_index = intern_str(e->name, (int)e->num);
        struct ir_ins *i = emit(fn);
        i->op = IR_STRADDR;
        i->label = e->str_index;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_VAR:
        if (e->undecayed) {
            /* an array's name IS the address of its first element */
            struct ir_ins *i = emit(fn);
            i->op = IR_ADDR;
            i->a = e->var_index;
            i->dst = new_temp(fn);
            return i->dst;
        }
        return emit_ldvar(fn, e->var_index, e->ty);
    case EXPR_ASSIGN: {
        if (e->lhs->kind == EXPR_VAR) {
            int v = gen_expr(fn, e->rhs);
            emit_stvar(fn, e->lhs->var_index, v, e->ty);
            return v;
        }
        /* *p = v */
        int addr = gen_expr(fn, e->lhs->rhs);
        int v = gen_expr(fn, e->rhs);
        struct ir_ins *i = emit(fn);
        i->op = IR_STORE;
        i->a = addr;
        i->b = v;
        i->size = ty_size(e->ty);
        return v;
    }
    case EXPR_INCDEC: {
        struct type *t = e->ty;
        int scale = t->kind == TY_PTR ? ty_size(t->pointee) : 1;
        int w = ty_w(t);
        int cur = emit_ldvar(fn, e->var_index, t);
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
        emit_stvar(fn, e->var_index, sum, t);
        return e->is_post ? old : sum;
    }
    case EXPR_NOT: {
        int v = gen_expr(fn, e->rhs);
        return emit_isz(fn, v, ty_w(e->rhs->ty));
    }
    case EXPR_NEG:
    case EXPR_BNOT: {
        int v = gen_expr(fn, e->rhs);
        struct ir_ins *i = emit(fn);
        i->op = e->kind == EXPR_NEG ? IR_NEG : IR_BNOT;
        i->a = v;
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_DEREF: {
        int addr = gen_expr(fn, e->rhs);
        if (e->undecayed)
            return addr; /* m[i] of a 2-D array: the row's address */
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
        if (e->rhs->kind == EXPR_VAR) {
            struct ir_ins *i = emit(fn);
            i->op = IR_ADDR;
            i->a = e->rhs->var_index;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* &*p is just p */
        return gen_expr(fn, e->rhs->rhs);
    case EXPR_CAST: {
        int v = gen_expr(fn, e->rhs);
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
            return emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB, a, b,
                            ty_w(e->ty), ty_signed_int(e->ty));
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
            i->w = ty_w(lt);
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
            return emit_bin(fn, map[e->op - B_ADD], a, b,
                            ty_w(e->ty), ty_signed_int(e->ty));
        }
        }
    }
    case EXPR_CALL: {
        int args[MAX_PARAMS];
        for (int k = 0; k < e->nargs; k++)
            args[k] = gen_expr(fn, e->args[k]);
        struct ir_ins *i = emit(fn);
        i->op = IR_CALL;
        i->callee = e->callee;
        i->nargs = e->nargs;
        for (int k = 0; k < e->nargs; k++)
            i->args[k] = args[k];
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
            if (s->expr) {
                int v = gen_expr(fn, s->expr);
                emit_stvar(fn, s->var_index, v, s->dty);
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
     * externals produce symbols and relocations instead (driver). */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn)
            iu->nfuncs++;
    iu->funcs = xcalloc((size_t)iu->nfuncs, sizeof *iu->funcs);

    int n = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn)
            gen_func(&iu->funcs[n++], f);
    return iu;
}
