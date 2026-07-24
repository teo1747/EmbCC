/* Name resolution and type checking. Sema's output contract: every
 * expression node carries a type, and every implicit conversion C
 * would perform is materialized as an explicit EXPR_CAST node — irgen
 * never guesses about widths or signedness, it just reads the tree.
 */
#include "sema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "type.h"

struct vardef {
    const char *name;
    struct type *ty;
    struct global *g;   /* non-NULL for a static local: it lives in
                         * static storage, not the frame */
    int active;         /* 0 once its block has closed */
    const char *asm_reg; /* a register-asm binding, else NULL */
};

/* Block scoping without giving up unique frame slots: entries are never
 * removed — the index IS the variable's slot for the whole function —
 * they are only deactivated when their block ends. Lookup runs backward
 * so an inner declaration shadows an outer one. */
struct scope {
    struct vardef *vars;
    int n, cap;
    int block_start;    /* index where the innermost block began */
};

static int scope_find(struct scope *sc, const char *name)
{
    for (int i = sc->n - 1; i >= 0; i--)
        if (sc->vars[i].active && strcmp(sc->vars[i].name, name) == 0)
            return i;
    return -1;
}

/* A redeclaration is an error only within the SAME block; an inner
 * block may shadow, exactly as C allows. */
static int scope_find_here(struct scope *sc, const char *name)
{
    for (int i = sc->n - 1; i >= sc->block_start; i--)
        if (sc->vars[i].active && strcmp(sc->vars[i].name, name) == 0)
            return i;
    return -1;
}

static int scope_add(struct scope *sc, const char *name, struct type *ty,
                     struct global *g)
{
    if (sc->n == sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 8;
        sc->vars = xrealloc(sc->vars, (size_t)sc->cap * sizeof *sc->vars);
    }
    sc->vars[sc->n].name = name;
    sc->vars[sc->n].ty = ty;
    sc->vars[sc->n].g = g;
    sc->vars[sc->n].active = 1;
    sc->vars[sc->n].asm_reg = NULL;
    return sc->n++;
}

/* Returns the canonical node for a name: the first declaration, into
 * which any later declarations have been merged. */
static struct func *find_func(struct unit *u, const char *name)
{
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

static struct global *find_global(struct unit *u, const char *name)
{
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && strcmp(g->name, name) == 0)
            return g;
    return NULL;
}

/* The source position of the function body being checked — a global is
 * visible inside it only if declared above it (C's rule). */
static int cur_body_seq;

/* ---- conversions ---- */

static int is_null_const(const struct expr *e)
{
    return e->kind == EXPR_NUM && e->num == 0;
}

/* Wrap in an implicit cast node unless already exactly that type. */
static struct expr *mk_cast(struct expr *inner, struct type *to)
{
    if (ty_equal(inner->ty, to))
        return inner;
    struct expr *c = xcalloc(1, sizeof *c);
    c->kind = EXPR_CAST;
    c->line = inner->line;
    c->cast_ty = to;
    c->rhs = inner;
    c->ty = to;
    return c;
}

/* C's integer promotions: everything narrower than int becomes int
 * (all narrow values fit, so the promoted type is always signed). */
static struct type *promote(struct type *t)
{
    if (t->kind == TY_CHAR || t->kind == TY_SHORT)
        return ty_base(TY_INT, 0);
    return t;
}

/* The DEFAULT ARGUMENT promotions, which are the integer promotions
 * PLUS float -> double. Distinct from promote() on purpose: a variadic
 * callee reads a float argument as a double, so passing a bare float
 * would hand printf garbage. */
static struct type *default_arg_promote(struct type *t)
{
    if (t->kind == TY_FLOAT)
        return ty_base(TY_DOUBLE, 0);
    return promote(t);
}

/* Usual arithmetic conversions, LP64: ranks are int(32) and long(64);
 * long can represent every unsigned int, so mixed int/long keeps the
 * long's signedness. */
static struct type *arith_common(struct type *a, struct type *b)
{
    /* Floating types outrank every integer, and double outranks float
     * — the usual arithmetic conversions, floating half first. */
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE)
        return ty_base(TY_DOUBLE, 0);
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT)
        return ty_base(TY_FLOAT, 0);
    a = promote(a);
    b = promote(b);
    int wa = ty_wide(a), wb = ty_wide(b);
    if (wa || wb) {
        int uns;
        if (wa && wb)
            uns = a->is_unsigned || b->is_unsigned;
        else
            uns = (wa ? a : b)->is_unsigned;
        return ty_base(TY_LONG, uns);
    }
    return ty_base(TY_INT, a->is_unsigned || b->is_unsigned);
}

static void need_scalar(struct unit *u, struct expr *e, const char *what)
{
    if (!ty_is_scalar(e->ty))
        diag_fatal(u->file, e->line, "%s needs a scalar value, got %s",
                   what, ty_name(e->ty));
}

static void need_integer(struct unit *u, struct expr *e, const char *what)
{
    if (!ty_is_integer(e->ty))
        diag_fatal(u->file, e->line, "%s needs an integer, got %s",
                   what, ty_name(e->ty));
}

static void need_arith(struct unit *u, struct expr *e, const char *what)
{
    if (!ty_is_arith(e->ty))
        diag_fatal(u->file, e->line,
                   "%s needs an arithmetic value, got %s", what,
                   ty_name(e->ty));
}

/* The conversions assignment performs (also used for arguments and
 * return values). Explicit casts are looser; this is the implicit set. */
/* SSE2 has no instruction converting between a 64-bit UNSIGNED integer
 * and a float: cvtsi2sd and cvttsd2si are both signed, so anything at
 * or above 2^63 would come out wrong. gcc emits a branchy fixup; EmbCC
 * refuses instead of quietly producing the wrong number (THE RULE).
 * Every other combination is exact: narrower integers are converted
 * through their 64-bit form. */
static void check_u64_float(struct unit *u, int line, struct type *a,
                            struct type *b)
{
    struct type *i = ty_is_float(a) ? b : a;
    struct type *fp = ty_is_float(a) ? a : b;
    if (!ty_is_float(fp) || !ty_is_integer(i))
        return;
    if (i->kind == TY_LONG && i->is_unsigned)
        diag_fatal(u->file, line,
                   "converting between unsigned long and %s is not "
                   "supported yet (SSE2 has no unsigned 64-bit "
                   "conversion; cast through a signed long if the value "
                   "fits)", ty_name(fp));
}

static struct expr *convert_assign(struct unit *u, struct expr *rhs,
                                   struct type *to, const char *ctx)
{
    if (to->kind == TY_STRUCT || rhs->ty->kind == TY_STRUCT) {
        if (!ty_equal(to, rhs->ty))
            diag_fatal(u->file, rhs->line, "%s: cannot convert %s to %s",
                       ctx, ty_name(rhs->ty), ty_name(to));
        return rhs; /* same struct type: passed/returned as its bytes */
    }
    if (ty_is_arith(to) && ty_is_arith(rhs->ty)) {
        check_u64_float(u, rhs->line, to, rhs->ty);
        return mk_cast(rhs, to);
    }
    if (to->kind == TY_PTR) {
        if (rhs->ty->kind == TY_PTR &&
            (ty_equal(rhs->ty, to) || to->pointee->kind == TY_VOID ||
             rhs->ty->pointee->kind == TY_VOID))
            return mk_cast(rhs, to);
        if (is_null_const(rhs))
            return mk_cast(rhs, to);
        diag_fatal(u->file, rhs->line,
                   "%s: cannot convert %s to %s without a cast",
                   ctx, ty_name(rhs->ty), ty_name(to));
    }
    if (ty_is_float(to) && rhs->ty->kind == TY_PTR)
        diag_fatal(u->file, rhs->line,
                   "%s: a pointer cannot become %s", ctx, ty_name(to));
    if (ty_is_integer(to) && rhs->ty->kind == TY_PTR)
        diag_fatal(u->file, rhs->line,
                   "%s: converting %s to %s needs an explicit cast",
                   ctx, ty_name(rhs->ty), ty_name(to));
    diag_fatal(u->file, rhs->line, "%s: cannot convert %s to %s",
               ctx, ty_name(rhs->ty), ty_name(to));
    return NULL;
}

static int is_lvalue(const struct expr *e)
{
    return e->kind == EXPR_VAR || e->kind == EXPR_DEREF ||
           e->kind == EXPR_MEMBER;
}

static int const_fold(const struct expr *e, long *out);

/* ---- expression checking ---- */

static void check_expr(struct unit *u, struct func *f, struct scope *sc,
                       struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
    case EXPR_FNUM:
        /* type assigned by the parser from the literal's shape */
        break;
    case EXPR_STR:
        /* char[N], decaying to char* like any array (sizeof sees the
         * array through `undecayed`) */
        e->undecayed = ty_array(ty_base(TY_CHAR, 0), (int)e->num);
        e->ty = ty_ptr(ty_base(TY_CHAR, 0));
        break;
    case EXPR_VAR: {
        int i = scope_find(sc, e->name);
        if (i >= 0) {
            e->var_index = i;
            e->ty = sc->vars[i].ty;
            e->gref = sc->vars[i].g; /* set for a static local */
            e->asm_reg = sc->vars[i].asm_reg; /* register-asm binding */
        } else {
            /* enumerators fold to their constant right here */
            struct econst *ec = u->econsts;
            for (; ec; ec = ec->next)
                if (strcmp(ec->name, e->name) == 0)
                    break;
            if (ec && ec->seq < cur_body_seq) {
                e->kind = EXPR_NUM;
                e->num = ec->val;
                e->ty = ty_base(TY_INT, 0);
                break;
            }
            if (ec)
                diag_fatal(u->file, e->line,
                           "enumerator '%s' is used before its "
                           "declaration", e->name);
            struct global *g = find_global(u, e->name);
            if (g && g->seq < cur_body_seq) {
                e->gref = g;
                g->used = 1;
                e->ty = g->ty;
            } else if (g) {
                diag_fatal(u->file, e->line,
                           "'%s' is used before its declaration "
                           "(line %d)", e->name, g->line);
            } else if (find_func(u, e->name)) {
                struct func *fd = find_func(u, e->name);
                if (!fd->declared)
                    diag_fatal(u->file, e->line,
                               "'%s' is used before its declaration",
                               e->name);
                e->fref = fd;
                fd->used = 1;
                e->ty = ty_ptr(ty_func(fd->ret_ty, fd->param_tys,
                                       fd->nparams, fd->is_varargs));
            } else {
                diag_fatal(u->file, e->line,
                           "'%s' is not declared in '%s' — for a call, "
                           "add a prototype or define it first",
                           e->name, f->name);
            }
        }
        if (e->ty->kind == TY_ARRAY) {
            e->undecayed = e->ty;
            e->ty = ty_ptr(e->ty->pointee);
        }
        break;
    }
    case EXPR_ASSIGN:
        check_expr(u, f, sc, e->lhs);
        if (!is_lvalue(e->lhs))
            diag_fatal(u->file, e->line, "assignment target is not an "
                                         "lvalue");
        if (e->lhs->undecayed)
            diag_fatal(u->file, e->line, "cannot assign to an array");
        if (e->lhs->fref)
            diag_fatal(u->file, e->line, "cannot assign to a function");
        check_expr(u, f, sc, e->rhs);
        if (e->lhs->ty->kind == TY_STRUCT) {
            if (!ty_equal(e->lhs->ty, e->rhs->ty))
                diag_fatal(u->file, e->line,
                           "cannot assign %s to %s",
                           ty_name(e->rhs->ty), ty_name(e->lhs->ty));
        } else {
            need_scalar(u, e->rhs, "assignment");
            e->rhs = convert_assign(u, e->rhs, e->lhs->ty, "assignment");
        }
        e->ty = e->lhs->ty;
        break;
    case EXPR_INCDEC: {
        check_expr(u, f, sc, e->lhs);
        if (!is_lvalue(e->lhs) || e->lhs->undecayed || e->lhs->fref)
            diag_fatal(u->file, e->line, "++/-- needs an lvalue");
        e->ty = e->lhs->ty;
        if (e->ty->kind == TY_PTR) {
            if (e->ty->pointee->kind == TY_VOID ||
                e->ty->pointee->kind == TY_FUNC)
                diag_fatal(u->file, e->line, "++/-- on %s",
                           ty_name(e->ty));
        } else if (!ty_is_arith(e->ty)) {
            diag_fatal(u->file, e->line, "++/-- needs an integer or "
                                         "pointer, got %s",
                       ty_name(e->ty));
        }
        break;
    }
    case EXPR_NOT:
        check_expr(u, f, sc, e->rhs);
        need_scalar(u, e->rhs, "'!'");
        e->ty = ty_base(TY_INT, 0);
        break;
    case EXPR_NEG:
        check_expr(u, f, sc, e->rhs);
        need_arith(u, e->rhs, "unary '-'");
        e->ty = promote(e->rhs->ty);
        e->rhs = mk_cast(e->rhs, e->ty);
        break;
    case EXPR_BNOT:
        check_expr(u, f, sc, e->rhs);
        need_integer(u, e->rhs, "'~'");
        e->ty = promote(e->rhs->ty);
        e->rhs = mk_cast(e->rhs, e->ty);
        break;
    case EXPR_DEREF:
        check_expr(u, f, sc, e->rhs);
        if (e->rhs->ty->kind != TY_PTR)
            diag_fatal(u->file, e->line, "cannot dereference %s",
                       ty_name(e->rhs->ty));
        if (e->rhs->ty->pointee->kind == TY_FUNC) {
            e->ty = e->rhs->ty; /* *fp is fp, as in C */
            break;
        }
        if (e->rhs->ty->pointee->kind == TY_VOID)
            diag_fatal(u->file, e->line, "cannot dereference void *");
        e->ty = e->rhs->ty->pointee;
        if (e->ty->kind == TY_ARRAY) {
            /* m[i] of a 2-D array is itself an array: it decays, and
             * irgen "loads" it as its address, not its bytes. */
            e->undecayed = e->ty;
            e->ty = ty_ptr(e->ty->pointee);
        }
        break;
    case EXPR_ADDR:
        check_expr(u, f, sc, e->rhs);
        if (e->rhs->fref) {
            *e = *e->rhs; /* &f is f: already a pointer-to-function */
            break;
        }
        if (!is_lvalue(e->rhs))
            diag_fatal(u->file, e->line,
                       "'&' needs a variable or *pointer");
        if (e->rhs->undecayed)
            diag_fatal(u->file, e->line,
                       "'&' on an array is not supported yet (its name "
                       "is already the address of the first element)");
        e->ty = ty_ptr(e->rhs->ty);
        break;
    case EXPR_CAST:
        check_expr(u, f, sc, e->rhs);
        if (e->cast_ty->kind == TY_VOID) {
            /* (void)x — evaluate and discard, the standard way to say
             * "yes, I meant to ignore this" */
            e->ty = e->cast_ty;
            break;
        }
        need_scalar(u, e->rhs, "a cast");
        if (!ty_is_scalar(e->cast_ty))
            diag_fatal(u->file, e->line, "cannot cast to %s",
                       ty_name(e->cast_ty));
        if ((ty_is_float(e->cast_ty) && e->rhs->ty->kind == TY_PTR) ||
            (e->cast_ty->kind == TY_PTR && ty_is_float(e->rhs->ty)))
            diag_fatal(u->file, e->line,
                       "cannot convert between %s and %s",
                       ty_name(e->rhs->ty), ty_name(e->cast_ty));
        if (ty_is_arith(e->cast_ty) && ty_is_arith(e->rhs->ty))
            check_u64_float(u, e->line, e->cast_ty, e->rhs->ty);
        e->ty = e->cast_ty;
        break;
    case EXPR_COMMA:
        check_expr(u, f, sc, e->lhs); /* evaluated, value discarded */
        check_expr(u, f, sc, e->rhs);
        e->ty = e->rhs->ty;
        break;
    case EXPR_COND: {
        check_expr(u, f, sc, e->args[0]);
        need_scalar(u, e->args[0], "'?:'");
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        struct type *a = e->lhs->ty, *b = e->rhs->ty;
        if (ty_is_arith(a) && ty_is_arith(b)) {
            e->ty = arith_common(a, b);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
        } else if (a->kind == TY_PTR && b->kind == TY_PTR) {
            if (!ty_equal(a, b) && a->pointee->kind != TY_VOID &&
                b->pointee->kind != TY_VOID)
                diag_fatal(u->file, e->line,
                           "'?:' branches have incompatible pointer "
                           "types (%s vs %s)", ty_name(a), ty_name(b));
            e->ty = a->pointee->kind == TY_VOID ? b : a;
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
        } else if (a->kind == TY_PTR && is_null_const(e->rhs)) {
            e->ty = a;
            e->rhs = mk_cast(e->rhs, a);
        } else if (b->kind == TY_PTR && is_null_const(e->lhs)) {
            e->ty = b;
            e->lhs = mk_cast(e->lhs, b);
        } else if (a->kind == TY_VOID && b->kind == TY_VOID) {
            e->ty = a;
        } else if (a->kind == TY_STRUCT && ty_equal(a, b)) {
            e->ty = a; /* both arms are the same aggregate */
        } else {
            diag_fatal(u->file, e->line,
                       "'?:' branches have incompatible types "
                       "(%s vs %s)", ty_name(a), ty_name(b));
        }
        break;
    }
    case EXPR_INITLIST:
        /* Only ever reached through flatten_init(), which knows the
         * target type; a brace list has no type of its own. */
        diag_fatal(u->file, e->line,
                   "a brace initializer cannot appear here");
        break;
    case EXPR_COMPOUND: {
        /* x op= y. The lvalue is evaluated once (irgen keeps its
         * address); the operation happens in the usual common type and
         * the result converts back to the target's type, as C says. */
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        if (!is_lvalue(e->lhs) || e->lhs->undecayed || e->lhs->fref)
            diag_fatal(u->file, e->line,
                       "compound assignment needs an lvalue");
        if (e->lhs->ty->kind == TY_PTR) {
            if (e->op != B_ADD && e->op != B_SUB)
                diag_fatal(u->file, e->line,
                           "only += and -= apply to a pointer");
            need_integer(u, e->rhs, "pointer arithmetic");
            e->rhs = mk_cast(e->rhs, ty_base(TY_LONG, 0));
            e->cast_ty = e->lhs->ty;
            e->ty = e->lhs->ty;
            break;
        }
        if (e->op == B_SHL || e->op == B_SHR) {
            need_integer(u, e->lhs, "a shift");
            need_integer(u, e->rhs, "a shift");
            e->cast_ty = promote(e->lhs->ty);
            e->rhs = mk_cast(e->rhs, ty_base(TY_INT, 0));
            e->ty = e->lhs->ty;
            break;
        }
        if (e->op == B_ADD || e->op == B_SUB || e->op == B_MUL ||
            e->op == B_DIV) {
            need_arith(u, e->lhs, "compound assignment");
            need_arith(u, e->rhs, "compound assignment");
        } else {
            need_integer(u, e->lhs, "this operator");
            need_integer(u, e->rhs, "this operator");
        }
        e->cast_ty = arith_common(e->lhs->ty, e->rhs->ty);
        check_u64_float(u, e->line, e->cast_ty, e->rhs->ty);
        e->rhs = mk_cast(e->rhs, e->cast_ty);
        e->ty = e->lhs->ty;
        break;
    }
    case EXPR_MEMBER: {
        check_expr(u, f, sc, e->lhs);
        struct type *base = e->lhs->ty;
        if (e->is_arrow) {
            if (base->kind != TY_PTR || base->pointee->kind != TY_STRUCT)
                diag_fatal(u->file, e->line,
                           "'->' needs a pointer to a struct/union, "
                           "got %s", ty_name(base));
            base = base->pointee;
        } else if (base->kind != TY_STRUCT) {
            diag_fatal(u->file, e->line,
                       "'.' needs a struct/union, got %s (use '->' "
                       "through a pointer)", ty_name(base));
        }
        if (!base->complete)
            diag_fatal(u->file, e->line,
                       "%s is incomplete here (its body comes later "
                       "or never)", ty_name(base));
        e->memb = ty_find_member(base, e->name);
        if (!e->memb)
            diag_fatal(u->file, e->line, "%s has no member '%s'",
                       ty_name(base), e->name);
        e->ty = e->memb->ty;
        if (e->ty->kind == TY_ARRAY) {
            e->undecayed = e->ty;
            e->ty = ty_ptr(e->ty->pointee);
        }
        break;
    }
    case EXPR_SIZEOF: {
        long size;
        if (e->cast_ty) {
            if (e->cast_ty->kind == TY_VOID)
                diag_fatal(u->file, e->line, "sizeof(void)");
            if (ty_size(e->cast_ty) == 0)
                diag_fatal(u->file, e->line, "sizeof of incomplete %s",
                           ty_name(e->cast_ty));
            size = ty_size(e->cast_ty);
        } else {
            check_expr(u, f, sc, e->rhs);
            if (e->rhs->ty->kind == TY_VOID)
                diag_fatal(u->file, e->line, "sizeof a void expression");
            /* sizeof is the one context where an array does NOT decay */
            size = ty_size(e->rhs->undecayed ? e->rhs->undecayed
                                             : e->rhs->ty);
        }
        /* Folded to a constant here; the operand is never evaluated,
         * exactly as C specifies. size_t is unsigned long in LP64. */
        e->kind = EXPR_NUM;
        e->num = size;
        e->rhs = NULL;
        e->ty = ty_base(TY_LONG, 1);
        break;
    }
    case EXPR_VA_ARG:
        check_expr(u, f, sc, e->lhs);   /* the va_list */
        if (e->cast_ty->kind == TY_VOID)
            diag_fatal(u->file, e->line, "va_arg cannot read type 'void'");
        if (e->cast_ty->kind == TY_STRUCT)
            diag_fatal(u->file, e->line,
                       "va_arg of a struct passed by value is not "
                       "supported yet");
        if (ty_is_float(e->cast_ty))
            diag_fatal(u->file, e->line,
                       "va_arg of a floating type is not supported yet "
                       "(EmbCC reads integer and pointer varargs)");
        e->ty = e->cast_ty;
        break;
    case EXPR_BINOP: {
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        struct type *lt = e->lhs->ty, *rt = e->rhs->ty;

        switch (e->op) {
        case B_LAND:
        case B_LOR:
            need_scalar(u, e->lhs, "'&&'/'||'");
            need_scalar(u, e->rhs, "'&&'/'||'");
            e->ty = ty_base(TY_INT, 0);
            break;
        case B_ADD:
        case B_SUB: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp && rp) {
                if (e->op == B_ADD)
                    diag_fatal(u->file, e->line,
                               "cannot add two pointers");
                if (!ty_equal(lt, rt))
                    diag_fatal(u->file, e->line,
                               "subtracting incompatible pointers "
                               "(%s vs %s)", ty_name(lt), ty_name(rt));
                if (lt->pointee->kind == TY_VOID ||
                    lt->pointee->kind == TY_FUNC)
                    diag_fatal(u->file, e->line, "arithmetic on %s",
                               ty_name(lt));
                e->ty = ty_base(TY_LONG, 0); /* ptrdiff_t */
            } else if (lp || rp) {
                if (rp && e->op == B_SUB)
                    diag_fatal(u->file, e->line,
                               "cannot subtract a pointer from an "
                               "integer");
                struct expr **ip = lp ? &e->rhs : &e->lhs;
                struct type *pt = lp ? lt : rt;
                if (pt->pointee->kind == TY_VOID ||
                    pt->pointee->kind == TY_FUNC)
                    diag_fatal(u->file, e->line, "arithmetic on %s",
                               ty_name(pt));
                need_integer(u, *ip, "pointer arithmetic");
                *ip = mk_cast(*ip, ty_base(TY_LONG, 0));
                e->ty = pt;
            } else {
                need_arith(u, e->lhs, "arithmetic");
                need_arith(u, e->rhs, "arithmetic");
                e->ty = arith_common(lt, rt);
                e->lhs = mk_cast(e->lhs, e->ty);
                e->rhs = mk_cast(e->rhs, e->ty);
            }
            break;
        }
        case B_EQ:
        case B_NE:
        case B_LT:
        case B_LE:
        case B_GT:
        case B_GE: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp || rp) {
                if (lp && rp) {
                    if (!ty_equal(lt, rt) &&
                        lt->pointee->kind != TY_VOID &&
                        rt->pointee->kind != TY_VOID)
                        diag_fatal(u->file, e->line,
                                   "comparing incompatible pointers "
                                   "(%s vs %s)", ty_name(lt),
                                   ty_name(rt));
                } else {
                    struct expr **ip = lp ? &e->rhs : &e->lhs;
                    if (!is_null_const(*ip))
                        diag_fatal(u->file, e->line,
                                   "comparing a pointer with an "
                                   "integer needs a cast");
                    *ip = mk_cast(*ip, lp ? lt : rt);
                }
            } else {
                need_arith(u, e->lhs, "comparison");
                need_arith(u, e->rhs, "comparison");
                struct type *ct = arith_common(lt, rt);
                e->lhs = mk_cast(e->lhs, ct);
                e->rhs = mk_cast(e->rhs, ct);
            }
            e->ty = ty_base(TY_INT, 0);
            break;
        }
        case B_SHL:
        case B_SHR:
            need_integer(u, e->lhs, "shift");
            need_integer(u, e->rhs, "shift");
            e->ty = promote(lt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, ty_base(TY_INT, 0));
            break;
        case B_MUL:
        case B_DIV:
            need_arith(u, e->lhs, "arithmetic");
            need_arith(u, e->rhs, "arithmetic");
            e->ty = arith_common(lt, rt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
            break;
        default: /* MOD AND OR XOR — integers only, as in C */
            need_integer(u, e->lhs, "this operator");
            need_integer(u, e->rhs, "this operator");
            e->ty = arith_common(lt, rt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
            break;
        }
        break;
    }
    case EXPR_CALL: {
        /* The stdarg builtins are not real functions: va_start needs the
         * ADDRESS of its va_list (irgen takes it), and neither is
         * declared anywhere. Type-check the operands, mark the enclosing
         * function variadic, and hand back void. va_end is a no-op. */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            (strcmp(e->lhs->name, "__builtin_va_start") == 0 ||
             strcmp(e->lhs->name, "__builtin_va_end") == 0)) {
            int is_start = strcmp(e->lhs->name, "__builtin_va_start") == 0;
            int want = is_start ? 2 : 1;
            if (e->nargs != want)
                diag_fatal(u->file, e->line,
                           "%s takes %d argument%s", e->lhs->name, want,
                           want == 1 ? "" : "s");
            for (int i = 0; i < e->nargs; i++)
                check_expr(u, f, sc, e->args[i]);
            if (!is_lvalue(e->args[0]))
                diag_fatal(u->file, e->line,
                           "the first argument to %s must be a va_list "
                           "variable", e->lhs->name);
            if (is_start && !f->is_varargs)
                diag_fatal(u->file, e->line,
                           "va_start in '%s', which is not variadic",
                           f->name);
            e->name = e->lhs->name;   /* irgen dispatches on it */
            e->ty = ty_base(TY_VOID, 0);
            break;
        }
        /* Direct when the callee is a name that is not a variable in
         * scope and names a function; otherwise a call through a
         * function-pointer value. */
        struct type *ft = NULL;
        e->callee = NULL;
        if (e->lhs->kind == EXPR_VAR &&
            scope_find(sc, e->lhs->name) < 0 &&
            !find_global(u, e->lhs->name) &&
            find_func(u, e->lhs->name)) {
            struct func *callee = find_func(u, e->lhs->name);
            if (!callee->declared)
                diag_fatal(u->file, e->line,
                           "call to '%s' before its declaration — "
                           "declare or define functions before their "
                           "callers", e->lhs->name);
            e->callee = callee;
            callee->used = 1;
            ft = ty_func(callee->ret_ty, callee->param_tys,
                         callee->nparams, callee->is_varargs);
        } else {
            check_expr(u, f, sc, e->lhs);
            if (e->lhs->ty->kind != TY_PTR ||
                e->lhs->ty->pointee->kind != TY_FUNC)
                diag_fatal(u->file, e->line,
                           "called object is not a function (type %s)",
                           ty_name(e->lhs->ty));
            ft = e->lhs->ty->pointee;
        }
        if (ft->is_varargs ? e->nargs < ft->nptypes
                           : e->nargs != ft->nptypes)
            diag_fatal(u->file, e->line,
                       "this call needs %s%d argument%s, got %d",
                       ft->is_varargs ? "at least " : "", ft->nptypes,
                       ft->nptypes == 1 ? "" : "s", e->nargs);
        for (int i = 0; i < e->nargs; i++) {
            check_expr(u, f, sc, e->args[i]);
            if (e->args[i]->ty->kind != TY_STRUCT)
                need_scalar(u, e->args[i], "an argument");
            if (i < ft->nptypes)
                e->args[i] = convert_assign(u, e->args[i],
                                            ft->ptypes[i], "argument");
            else /* variadic tail: default argument promotions */
                e->args[i] = mk_cast(e->args[i],
                                     default_arg_promote(e->args[i]->ty));
        }
        e->ty = ft->ret;
        break;
    }
    }
}

/* Enough constant folding for a case label. Anything it cannot fold is
 * refused by name rather than guessed at — case labels must be integer
 * constant expressions, and a label we cannot evaluate is a label we
 * cannot dispatch on (THE RULE). */
static int const_fold(const struct expr *e, long *out)
{
    long a, b;

    switch (e->kind) {
    case EXPR_NUM:
        *out = e->num;
        return 1;
    case EXPR_CAST:
        return const_fold(e->rhs, out);
    case EXPR_NEG:
        if (!const_fold(e->rhs, &a))
            return 0;
        *out = -a;
        return 1;
    case EXPR_BNOT:
        if (!const_fold(e->rhs, &a))
            return 0;
        *out = ~a;
        return 1;
    case EXPR_NOT:
        if (!const_fold(e->rhs, &a))
            return 0;
        *out = !a;
        return 1;
    case EXPR_BINOP:
        if (!const_fold(e->lhs, &a) || !const_fold(e->rhs, &b))
            return 0;
        switch (e->op) {
        case B_ADD: *out = a + b; return 1;
        case B_SUB: *out = a - b; return 1;
        case B_MUL: *out = a * b; return 1;
        case B_DIV: if (!b) return 0; *out = a / b; return 1;
        case B_MOD: if (!b) return 0; *out = a % b; return 1;
        case B_AND: *out = a & b; return 1;
        case B_OR:  *out = a | b; return 1;
        case B_XOR: *out = a ^ b; return 1;
        case B_SHL: *out = a << b; return 1;
        case B_SHR: *out = a >> b; return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* The statement list a switch dispatches over: its body, unwrapped when
 * it is the usual brace block. Case markers must live at THIS level. */
struct stmt *switch_stmts(struct stmt *body)
{
    if (body && body->kind == STMT_BLOCK)
        return body->body;
    return body;
}

/* Flattens an initializer against its target type into (offset, type,
 * value) triples. Nested braces recurse; a scalar initializer for an
 * aggregate member is checked and converted like any assignment. C's
 * rule that unlisted elements are zero is honoured by irgen, which
 * clears the whole object first. */
struct initbuf {
    struct initelem *v;
    int n, cap;
};

static void init_push(struct initbuf *b, int off, struct type *ty,
                      struct expr *e)
{
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->v = xrealloc(b->v, (size_t)b->cap * sizeof *b->v);
    }
    b->v[b->n].off = off;
    b->v[b->n].ty = ty;
    b->v[b->n].e = e;
    b->n++;
}

static void flatten_init(struct unit *u, struct func *f, struct scope *sc,
                         struct expr *init, struct type *ty, int off,
                         struct initbuf *out)
{
    if (init->kind != EXPR_INITLIST) {
        if (ty->kind == TY_ARRAY) {
            /* char a[] = "..." — the literal's bytes ARE the object */
            if (init->kind == EXPR_STR &&
                ty->pointee->kind == TY_CHAR) {
                int len = (int)init->num;
                if (ty->count && ty->count < len - 1)
                    diag_fatal(u->file, init->line,
                               "initializer is longer than the array");
                for (int i = 0; i < len && (!ty->count || i < ty->count);
                     i++) {
                    struct expr *ch = xcalloc(1, sizeof *ch);
                    ch->kind = EXPR_NUM;
                    ch->line = init->line;
                    ch->num = (unsigned char)init->name[i];
                    ch->ty = ty_base(TY_CHAR, 0);
                    init_push(out, off + i, ty->pointee, ch);
                }
                return;
            }
            diag_fatal(u->file, init->line,
                       "an array needs a brace initializer or a string");
        }
        check_expr(u, f, sc, init);
        if (ty->kind != TY_STRUCT)
            need_scalar(u, init, "an initializer");
        init_push(out, off, ty,
                  convert_assign(u, init, ty, "initialization"));
        return;
    }

    if (ty->kind == TY_ARRAY) {
        int esz = ty_size(ty->pointee);
        if (ty->count && init->nelems > ty->count)
            diag_fatal(u->file, init->line,
                       "%d initializers for an array of %d",
                       init->nelems, ty->count);
        for (int i = 0; i < init->nelems; i++) {
            if (init->elems[i]->desig_field)
                diag_fatal(u->file, init->elems[i]->line,
                           "field designator '.%s' in an array initializer",
                           init->elems[i]->desig_field);
            flatten_init(u, f, sc, init->elems[i], ty->pointee,
                         off + i * esz, out);
        }
        return;
    }
    if (ty->kind == TY_STRUCT) {
        /* Positional by default; a `.field =` designator jumps to that
         * member and initialization continues positionally after it. */
        int mi = 0;
        for (int i = 0; i < init->nelems; i++) {
            struct expr *el = init->elems[i];
            if (el->desig_field) {
                struct member *m = ty_find_member(ty, el->desig_field);
                if (!m)
                    diag_fatal(u->file, el->line,
                               "%s has no member '%s'", ty_name(ty),
                               el->desig_field);
                mi = (int)(m - ty->members);
            }
            if (mi >= ty->nmembers)
                diag_fatal(u->file, init->line,
                           "too many initializers for %s, which has %d "
                           "members", ty_name(ty), ty->nmembers);
            flatten_init(u, f, sc, el, ty->members[mi].ty,
                         off + ty->members[mi].off, out);
            mi++;
        }
        return;
    }
    /* a braced scalar: { x } */
    if (init->nelems != 1)
        diag_fatal(u->file, init->line,
                   "a scalar takes exactly one initializer");
    flatten_init(u, f, sc, init->elems[0], ty, off, out);
}

/* Lower flattened initializer leaves into a static object's byte image
 * plus a relocation list. Shared by file-scope globals and static locals
 * — both have static storage, so every leaf must reduce to constant
 * bytes now, save pointer slots initialized by a string literal, which
 * become relocations the linker resolves. */
static void lower_static_bytes(struct unit *u, int line, int size,
                               struct initelem *v, int n,
                               const char **out_bytes,
                               struct greloc **out_rel, int *out_nrel)
{
    char *bytes = xcalloc(1, (size_t)(size ? size : 1));
    struct greloc *rel = NULL;
    int nrel = 0, caprel = 0;
    for (int k = 0; k < n; k++) {
        struct expr *core = v[k].e;
        while (core && core->kind == EXPR_CAST)
            core = core->rhs;
        if (core && core->kind == EXPR_STR && v[k].ty->kind == TY_PTR) {
            /* a pointer slot pointing at a string literal: 8 zero bytes
             * stay in the image; the linker writes the address. */
            if (nrel == caprel) {
                caprel = caprel ? caprel * 2 : 4;
                rel = xrealloc(rel, (size_t)caprel * sizeof *rel);
            }
            rel[nrel].off = v[k].off;
            rel[nrel].str = core->name;
            rel[nrel].str_len = (int)core->num;
            rel[nrel].addend = 0;
            nrel++;
            continue;
        }
        long cv;
        if (!const_fold(v[k].e, &cv))
            diag_fatal(u->file, line,
                       "a static initializer must be a constant or a "
                       "string-literal address");
        int sz = ty_size(v[k].ty);
        for (int b = 0; b < sz; b++)
            bytes[v[k].off + b] = (char)((unsigned long)cv >> (8 * b));
    }
    *out_bytes = bytes;
    *out_rel = rel;
    *out_nrel = nrel;
}

/* Reduce every file-scope global's aggregate/relocatable initializer to
 * its byte image + relocations, now that types and sizes are settled. */
static void lower_globals(struct unit *u)
{
    struct func gf = { 0 };
    gf.name = "<global initializer>";
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || !g->init_expr)
            continue;
        /* An initializer may reference any name declared before this
         * global — mirror the source position so the declare-before-use
         * rule (and enum-constant visibility) matches C. */
        cur_body_seq = g->seq;
        struct scope sc = { 0, 0, 0, 0 };
        struct initbuf ib = { 0, 0, 0 };
        flatten_init(u, &gf, &sc, g->init_expr, g->ty, 0, &ib);
        lower_static_bytes(u, g->line, ty_size(g->ty), ib.v, ib.n,
                           &g->init_bytes, &g->relocs, &g->nrelocs);
        g->init_len = ty_size(g->ty);
        g->init_expr = NULL;
    }
}

/* Register name -> encoding number (0-15). The table type is file scope
 * because EmbCC's own subset (which compiles this file) has no block-scope
 * type definitions. */
struct regname { const char *name; int reg; };
static const struct regname reg_names[] = {
    { "rax", 0 }, { "rbx", 3 }, { "rcx", 1 }, { "rdx", 2 },
    { "rsi", 6 }, { "rdi", 7 }, { "r8", 8 }, { "r9", 9 },
    { "r10", 10 }, { "r11", 11 }, { "r12", 12 }, { "r13", 13 },
    { "r14", 14 }, { "r15", 15 },
};
static int asm_reg_by_name(const char *n)
{
    for (unsigned i = 0; i < sizeof reg_names / sizeof reg_names[0]; i++)
        if (strcmp(n, reg_names[i].name) == 0)
            return reg_names[i].reg;
    return -1;
}

/* The fixed register a constraint pins its operand to (0-15). Output
 * constraints carry a leading '=' (write) or '+' (read-write); '&'
 * (earlyclobber) is accepted and ignored. EmbCC supports the fixed-register
 * letters a/b/c/d/S/D and 'r' bound through a register-asm variable
 * (`register T x __asm__("r10")`) — enough for the int-$0x80 syscall stubs;
 * general 'r' allocation is a seam left open. */
static int asm_resolve_reg(struct unit *u, struct stmt *s,
                           struct asm_operand *op, int is_out)
{
    const char *c = op->constraint;
    if (is_out && *c != '=' && *c != '+')
        diag_fatal(u->file, s->line,
                   "an asm output constraint must start with '=' or '+' "
                   "(got \"%s\")", op->constraint);
    while (*c == '=' || *c == '+' || *c == '&')
        c++;
    switch (*c) {
    case 'a': return 0;
    case 'b': return 3;
    case 'c': return 1;
    case 'd': return 2;
    case 'S': return 6;
    case 'D': return 7;
    case 'r': {
        /* 'r' must name a register-asm variable so EmbCC knows WHICH
         * register — it does no general register allocation. */
        if (op->expr->kind == EXPR_VAR && op->expr->asm_reg) {
            int r = asm_reg_by_name(op->expr->asm_reg);
            if (r >= 0)
                return r;
        }
        diag_fatal(u->file, s->line,
                   "asm 'r' constraint needs a register-asm variable "
                   "(register T x __asm__(\"r10\")) — EmbCC does no general "
                   "register allocation");
        return -1;
    }
    default:
        diag_fatal(u->file, s->line,
                   "asm constraint \"%s\" is not supported "
                   "(EmbCC handles a/b/c/d/S/D and 'r' via a register-asm "
                   "variable)", op->constraint);
        return -1;
    }
}

/* Assemble the asm template into machine bytes. EmbCC has no general
 * text assembler; it recognizes the fixed vocabulary real low-level C
 * needs — today just `int $imm`, the EmbLinkOS syscall trap — and refuses
 * anything else loudly (THE RULE). Templates that need %0/%1 operand
 * substitution are not accepted; the syscall stubs bind operands through
 * constraints, so their template is operand-free. */
static void asm_assemble_template(struct unit *u, struct stmt *s)
{
    const char *p = s->asm_s->tmpl;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    if (p[0] == 'i' && p[1] == 'n' && p[2] == 't' &&
        (p[3] == ' ' || p[3] == '\t')) {
        p += 3;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '$') {
            char *end;
            long imm = strtol(p + 1, &end, 0);
            p = end;
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
                p++;
            if (*p == '\0') {
                if (imm < 0 || imm > 255)
                    diag_fatal(u->file, s->line,
                               "asm 'int' vector %ld out of range [0,255]",
                               imm);
                s->asm_s->code[0] = 0xcd;
                s->asm_s->code[1] = (unsigned char)imm;
                s->asm_s->codelen = 2;
                return;
            }
        }
    }
    diag_fatal(u->file, s->line,
               "asm template instruction not supported: \"%s\" "
               "(EmbCC assembles only 'int $imm')", s->asm_s->tmpl);
}

/* Declarations anywhere in the function share one flat scope, and
 * shadowing is rejected outright. C gives inner blocks their own scope;
 * refusing shadowed names accepts strictly fewer programs than C does,
 * so the subset stays a subset. */
static void check_stmt(struct unit *u, struct func *f, struct scope *sc,
                       struct stmt *s, int in_loop, int in_switch,
                       int at_sw_level)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_BREAK:
            /* break leaves the nearest loop OR switch; continue only
             * ever belongs to a loop. */
            if (!in_loop && !in_switch)
                diag_fatal(u->file, s->line,
                           "'break' outside of a loop or switch");
            break;
        case STMT_CONTINUE:
            if (!in_loop)
                diag_fatal(u->file, s->line, "'continue' outside of a loop");
            break;
        case STMT_CASE:
        case STMT_DEFAULT:
            if (!at_sw_level)
                diag_fatal(u->file, s->line,
                           "'%s' must appear directly in its switch body "
                           "(labels inside a nested block are not "
                           "supported)",
                           s->kind == STMT_CASE ? "case" : "default");
            if (s->kind == STMT_CASE) {
                check_expr(u, f, sc, s->expr);
                need_integer(u, s->expr, "a case label");
                if (!const_fold(s->expr, &s->cval))
                    diag_fatal(u->file, s->line,
                               "a case label must be an integer constant "
                               "expression");
            }
            break;
        case STMT_SWITCH: {
            check_expr(u, f, sc, s->cond);
            need_integer(u, s->cond, "'switch'");
            struct stmt *list = switch_stmts(s->body);
            check_stmt(u, f, sc, list, in_loop, 1, 1);
            /* duplicate labels and a second default are parse-time
             * errors, not a runtime coin flip about which one wins */
            int ndefault = 0;
            for (struct stmt *a = list; a; a = a->next) {
                if (a->kind == STMT_DEFAULT && ++ndefault > 1)
                    diag_fatal(u->file, a->line,
                               "a switch can have only one 'default'");
                if (a->kind != STMT_CASE)
                    continue;
                for (struct stmt *b = a->next; b; b = b->next)
                    if (b->kind == STMT_CASE && b->cval == a->cval)
                        diag_fatal(u->file, b->line,
                                   "duplicate case label %ld", b->cval);
            }
            break;
        }
        case STMT_DO:
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            check_expr(u, f, sc, s->cond);
            need_scalar(u, s->cond, "'do'/'while'");
            break;
        case STMT_DECL:
            if (s->expr && s->dty->kind == TY_ARRAY &&
                s->expr->kind == EXPR_STR) {
                /* char a[] = "..." : an omitted size is the literal's */
                if (s->dty->pointee->kind != TY_CHAR)
                    diag_fatal(u->file, s->line,
                               "only a char array can be initialized "
                               "from a string");
                if (s->dty->count == 0)
                    s->dty = ty_array(s->dty->pointee,
                                      (int)s->expr->num);
            } else if (s->expr && s->expr->kind == EXPR_INITLIST &&
                       s->dty->kind == TY_ARRAY && s->dty->count == 0) {
                /* an omitted array size is the element count */
                s->dty = ty_array(s->dty->pointee, s->expr->nelems);
            }
            /* The name is in scope WITHIN its own initializer (C11
             * 6.2.1p7: scope begins just after the declarator), so the
             * pervasive `T *p = xcalloc(1, sizeof *p)` resolves p. Add
             * it before checking the initializer; a static local's
             * global is wired onto this same entry below. */
            if (scope_find_here(sc, s->name) >= 0)
                diag_fatal(u->file, s->line,
                           "'%s' is already declared in this block",
                           s->name);
            s->var_index = scope_add(sc, s->name, s->dty, NULL);
            sc->vars[s->var_index].asm_reg = s->asm_reg;
            if (s->expr && s->dty->kind != TY_ARRAY &&
                s->dty->kind != TY_STRUCT) {
                check_expr(u, f, sc, s->expr);
                if (s->dty->kind != TY_STRUCT)
                    need_scalar(u, s->expr, "an initializer");
                s->expr = convert_assign(u, s->expr, s->dty,
                                         "initialization");
            }
            if (s->expr && (s->expr->kind == EXPR_INITLIST ||
                            s->dty->kind == TY_ARRAY ||
                            s->dty->kind == TY_STRUCT)) {
                struct initbuf ib = { 0, 0, 0 };
                flatten_init(u, f, sc, s->expr, s->dty, 0, &ib);
                s->inits = ib.v;
                s->ninits = ib.n;
                s->expr = NULL;
            }
            if (s->is_static) {
                /* A static local has static STORAGE and internal
                 * linkage: it becomes a global of its own, named so it
                 * cannot collide with a file-scope name, and its
                 * initializer is lowered to a byte image + relocations
                 * through the same path as any file-scope global. */
                struct global *g = xcalloc(1, sizeof *g);
                size_t n = strlen(f->name) + strlen(s->name) + 8;
                char *nm = xmalloc(n);
                snprintf(nm, n, "%s.%s", f->name, s->name);
                g->name = nm;
                g->line = s->line;
                g->seq = -1;      /* visible from its own function only */
                g->ty = s->dty;
                g->is_static = 1;
                g->defined = 1;
                g->used = 1;
                /* Aggregates arrive pre-flattened in s->inits; a scalar's
                 * value is a single leaf at offset 0. */
                struct initelem one;
                struct initelem *iv = s->inits;
                int in = s->ninits;
                if (!in && s->expr) {
                    one.off = 0;
                    one.ty = s->dty;
                    one.e = s->expr;
                    iv = &one;
                    in = 1;
                }
                if (in) {
                    lower_static_bytes(u, s->line, ty_size(s->dty), iv, in,
                                       &g->init_bytes, &g->relocs,
                                       &g->nrelocs);
                    g->init_len = ty_size(s->dty);
                    g->has_init = 1;
                }
                s->ninits = 0;
                struct global **gt = &u->globals;
                while (*gt)
                    gt = &(*gt)->next;
                *gt = g;
                s->sglob = g;
                s->expr = NULL;   /* the data, not code, carries it */
                sc->vars[s->var_index].g = g; /* wire the global onto the
                                                 entry added above */
                break;
            }
            break;
        case STMT_RETURN:
            if (f->ret_ty->kind == TY_VOID) {
                if (s->expr)
                    diag_fatal(u->file, s->line,
                               "returning a value from void '%s'",
                               f->name);
            } else {
                if (!s->expr)
                    diag_fatal(u->file, s->line,
                               "'%s' returns %s; 'return' needs a value",
                               f->name, ty_name(f->ret_ty));
                check_expr(u, f, sc, s->expr);
                if (s->expr->ty->kind != TY_STRUCT)
                    need_scalar(u, s->expr, "'return'");
                s->expr = convert_assign(u, s->expr, f->ret_ty, "return");
            }
            break;
        case STMT_EXPR:
            check_expr(u, f, sc, s->expr);
            break;
        case STMT_ASM: {
            struct asm_stmt *a = s->asm_s;
            for (int i = 0; i < a->nout; i++) {
                check_expr(u, f, sc, a->out[i].expr);
                if (!is_lvalue(a->out[i].expr))
                    diag_fatal(u->file, s->line,
                               "an asm output operand must be an lvalue");
                a->out[i].reg = asm_resolve_reg(u, s, &a->out[i], 1);
            }
            for (int i = 0; i < a->nin; i++) {
                check_expr(u, f, sc, a->in[i].expr);
                a->in[i].reg = asm_resolve_reg(u, s, &a->in[i], 0);
            }
            asm_assemble_template(u, s);
            break;
        }
        case STMT_IF:
            check_expr(u, f, sc, s->cond);
            need_scalar(u, s->cond, "'if'");
            check_stmt(u, f, sc, s->thn, in_loop, in_switch, 0);
            if (s->els)
                check_stmt(u, f, sc, s->els, in_loop, in_switch, 0);
            break;
        case STMT_WHILE:
            check_expr(u, f, sc, s->cond);
            need_scalar(u, s->cond, "'while'");
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            break;
        case STMT_FOR: {
            /* `for (int i = ...)` scopes i to the loop, so sibling
             * loops may each declare their own. */
            int mark = sc->n, prev = sc->block_start;
            sc->block_start = mark;
            if (s->initdecl)
                check_stmt(u, f, sc, s->initdecl, in_loop, in_switch, 0);
            if (s->init)
                check_expr(u, f, sc, s->init);
            if (s->cond) { /* NULL = forever, left by 'break' */
                check_expr(u, f, sc, s->cond);
                need_scalar(u, s->cond, "'for'");
            }
            if (s->step)
                check_expr(u, f, sc, s->step);
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            for (int i = mark; i < sc->n; i++)
                sc->vars[i].active = 0;
            sc->block_start = prev;
            break;
        }
        case STMT_BLOCK: {
            int mark = sc->n, prev = sc->block_start;
            sc->block_start = mark;
            check_stmt(u, f, sc, s->body, in_loop, in_switch, 0);
            for (int i = mark; i < sc->n; i++)
                sc->vars[i].active = 0; /* the block closed */
            sc->block_start = prev;
            break;
        }
        }
    }
}

/* Conservative all-paths-return. Refusing a maybe-missing return is
 * honest; miscompiling one is not (THE RULE) — but "conservative" must
 * not mean "wrong about ordinary code", so switch and infinite loops
 * are analysed rather than assumed to fall through. */
static int list_returns(struct stmt *s);

/* Does a break leave THIS construct? Breaks inside a nested loop or
 * switch belong to that one, so they do not count. */
static int has_own_break(struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_BREAK:
            return 1;
        case STMT_BLOCK:
            if (has_own_break(s->body))
                return 1;
            break;
        case STMT_IF:
            if (has_own_break(s->thn) ||
                (s->els && has_own_break(s->els)))
                return 1;
            break;
        default:
            break; /* a nested loop/switch captures its own breaks */
        }
    }
    return 0;
}

/* Functions that never return, so a call to one terminates control flow
 * as surely as `return`. The noreturn attribute newlib puts on exit/abort
 * is stripped before EmbCC sees it (EmbCC does not parse __attribute__,
 * and _ATTRIBUTE expands empty for a non-GNU compiler), so the set is
 * recognized by name — sound because these genuinely never return, and
 * the worst case of a same-named user function is a missed diagnostic,
 * never a miscompile. diag_fatal is EmbCC's own, used at many tails. */
static int is_noreturn_call(const struct expr *e)
{
    if (e->kind != EXPR_CALL || !e->name)
        return 0;
    static const char *const nr[] = {
        "exit", "abort", "_Exit", "diag_fatal",
    };
    for (unsigned i = 0; i < sizeof nr / sizeof *nr; i++)
        if (strcmp(e->name, nr[i]) == 0)
            return 1;
    return 0;
}

static int stmt_returns(struct stmt *s)
{
    switch (s->kind) {
    case STMT_RETURN:
        return 1;
    case STMT_EXPR:
        return s->expr && is_noreturn_call(s->expr);
    case STMT_BLOCK:
        return list_returns(s->body);
    case STMT_IF:
        return s->els && stmt_returns(s->thn) && stmt_returns(s->els);
    case STMT_SWITCH: {
        /* Sound: with a default every value matches something, and with
         * no break the only way out is falling off the end — which the
         * last statement returning rules out. Anything reached earlier
         * either returns or falls through toward it. */
        struct stmt *list = switch_stmts(s->body);
        int has_default = 0;
        struct stmt *last = NULL;
        for (struct stmt *a = list; a; a = a->next) {
            if (a->kind == STMT_DEFAULT)
                has_default = 1;
            last = a;
        }
        if (!has_default || has_own_break(list) || !last)
            return 0;
        return stmt_returns(last);
    }
    case STMT_FOR:
        /* `for (;;)` with no break of its own never exits normally. */
        return !s->cond && !has_own_break(s->body);
    case STMT_WHILE: {
        long v;
        return const_fold(s->cond, &v) && v != 0 &&
               !has_own_break(s->body);
    }
    case STMT_DO: {
        long v;
        return (const_fold(s->cond, &v) && v != 0 &&
                !has_own_break(s->body)) || stmt_returns(s->body);
    }
    default:
        return 0;
    }
}

static int list_returns(struct stmt *s)
{
    for (; s; s = s->next)
        if (stmt_returns(s))
            return 1;
    return 0;
}

static void check_func(struct unit *u, struct func *f)
{
    struct scope sc = { 0, 0, 0, 0 };

    for (int i = 0; i < f->nparams; i++) {
        if (scope_find(&sc, f->params[i]) >= 0)
            diag_fatal(u->file, f->line,
                       "duplicate parameter '%s' in '%s'",
                       f->params[i], f->name);
        scope_add(&sc, f->params[i], f->param_tys[i], NULL);
    }

    check_stmt(u, f, &sc, f->body, 0, 0, 0);

    if (f->ret_ty->kind != TY_VOID && !list_returns(f->body))
        diag_fatal(f->file ? f->file : u->file, f->line,
                   "control may reach the end of '%s' — every path must "
                   "end in a return statement", f->name);

    f->nvars = sc.n;
    f->var_tys = xmalloc((size_t)(sc.n ? sc.n : 1) * sizeof *f->var_tys);
    for (int i = 0; i < sc.n; i++)
        /* a static local keeps its scope index but needs no frame
         * storage — give it a pointer's worth and never address it */
        f->var_tys[i] = sc.vars[i].g ? ty_base(TY_LONG, 0)
                                     : sc.vars[i].ty;
    free(sc.vars);
}

/* Merge every later declaration of a name into its first (canonical)
 * node. C's static rule kept exactly: static-then-non-static keeps
 * internal linkage, non-static-then-static is an error (gcc agrees). */
static void merge_decls(struct unit *u)
{
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed)
            continue;
        struct func *canon = find_func(u, f->name);
        if (canon == f) {
            f->has_defn = f->defined;
            continue;
        }
        int match = canon->nparams == f->nparams &&
                    canon->is_varargs == f->is_varargs &&
                    ty_equal(canon->ret_ty, f->ret_ty);
        for (int i = 0; match && i < f->nparams; i++)
            if (!ty_equal(canon->param_tys[i], f->param_tys[i]))
                match = 0;
        if (!match)
            diag_fatal(u->file, f->line,
                       "conflicting declaration of '%s' (earlier one at "
                       "line %d)", f->name, canon->line);
        if (f->is_static && !canon->is_static)
            diag_fatal(u->file, f->line,
                       "static declaration of '%s' follows non-static "
                       "declaration (line %d)", f->name, canon->line);
        if (f->defined) {
            if (canon->has_defn)
                diag_fatal(u->file, f->line, "redefinition of '%s'",
                           f->name);
            canon->has_defn = 1;
            canon->body = f->body;
            for (int i = 0; i < f->nparams; i++)
                canon->params[i] = f->params[i]; /* definition names win */
        }
        f->absorbed = 1;
    }
}

/* Merge later declarations of each global into its canonical node.
 * `int g;` counts as a definition (the tentative-definition subtlety is
 * collapsed); extern declares without defining; at most one
 * initializer. Same static linkage rules as functions. */
static void merge_globals(struct unit *u)
{
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed)
            continue;
        struct global *canon = find_global(u, g->name);
        if (find_func(u, g->name))
            diag_fatal(u->file, g->line,
                       "'%s' is declared as both a function and a "
                       "variable", g->name);
        if (canon == g) {
            g->defined = !g->is_extern;
            continue;
        }
        /* Two declarations of an array are compatible when their element
         * types match and at most one gives a size — `extern T x[];`
         * completed by `T x[N] = …`. The canonical node adopts the
         * complete type so its symbol carries the real size. */
        int compat = ty_equal(canon->ty, g->ty);
        if (!compat && canon->ty->kind == TY_ARRAY &&
            g->ty->kind == TY_ARRAY &&
            ty_equal(canon->ty->pointee, g->ty->pointee) &&
            (canon->ty->count == 0 || g->ty->count == 0 ||
             canon->ty->count == g->ty->count)) {
            compat = 1;
            if (canon->ty->count == 0)
                canon->ty = g->ty;
        }
        if (!compat)
            diag_fatal(u->file, g->line,
                       "conflicting types for '%s': %s here, %s at "
                       "line %d", g->name, ty_name(g->ty),
                       ty_name(canon->ty), canon->line);
        if (g->is_static && !canon->is_static)
            diag_fatal(u->file, g->line,
                       "static declaration of '%s' follows non-static "
                       "declaration (line %d)", g->name, canon->line);
        if (g->has_init) {
            if (canon->has_init)
                diag_fatal(u->file, g->line, "redefinition of '%s'",
                           g->name);
            canon->has_init = 1;
            canon->init = g->init;
            canon->init_expr = g->init_expr;
        }
        canon->defined |= !g->is_extern;
        g->absorbed = 1;
    }
}

void sema_check(struct unit *u)
{
    merge_decls(u);
    merge_globals(u);
    lower_globals(u);

    /* Walk in source order so `declared` mirrors C's rule exactly: a
     * name is usable from its first declaration on, and a body is
     * checked at its DEFINITION's position. */
    for (struct func *f = u->funcs; f; f = f->next) {
        struct func *canon = find_func(u, f->name);
        if (canon == f)
            f->declared = 1;
        if (f->defined) {
            cur_body_seq = f->seq;
            check_func(u, canon);
        }
    }

    /* An undefined non-static is an external: the linker gets a chance.
     * An undefined static has no linker to save it — refuse now instead
     * of emitting an unresolvable object (THE RULE). */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && !f->has_defn && f->is_static && f->used)
            diag_fatal(u->file, f->line,
                       "static function '%s' is called but never defined",
                       f->name);
}
