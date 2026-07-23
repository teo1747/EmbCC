/* Name resolution and type checking. Sema's output contract: every
 * expression node carries a type, and every implicit conversion C
 * would perform is materialized as an explicit EXPR_CAST node — irgen
 * never guesses about widths or signedness, it just reads the tree.
 */
#include "sema.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "type.h"

struct vardef {
    const char *name;
    struct type *ty;
};

struct scope {
    struct vardef *vars;
    int n, cap;
};

static int scope_find(struct scope *sc, const char *name)
{
    for (int i = 0; i < sc->n; i++)
        if (strcmp(sc->vars[i].name, name) == 0)
            return i;
    return -1;
}

static int scope_add(struct scope *sc, const char *name, struct type *ty)
{
    if (sc->n == sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 8;
        sc->vars = xrealloc(sc->vars, (size_t)sc->cap * sizeof *sc->vars);
    }
    sc->vars[sc->n].name = name;
    sc->vars[sc->n].ty = ty;
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

/* Usual arithmetic conversions, LP64: ranks are int(32) and long(64);
 * long can represent every unsigned int, so mixed int/long keeps the
 * long's signedness. */
static struct type *arith_common(struct type *a, struct type *b)
{
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

/* The conversions assignment performs (also used for arguments and
 * return values). Explicit casts are looser; this is the implicit set. */
static struct expr *convert_assign(struct unit *u, struct expr *rhs,
                                   struct type *to, const char *ctx)
{
    if (ty_is_integer(to) && ty_is_integer(rhs->ty))
        return mk_cast(rhs, to);
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

/* ---- expression checking ---- */

static void check_expr(struct unit *u, struct func *f, struct scope *sc,
                       struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
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
        if (e->lhs->ty->kind == TY_STRUCT)
            diag_fatal(u->file, e->line,
                       "struct assignment is not supported yet — copy "
                       "the members, or memcpy through pointers");
        check_expr(u, f, sc, e->rhs);
        need_scalar(u, e->rhs, "assignment");
        e->rhs = convert_assign(u, e->rhs, e->lhs->ty, "assignment");
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
        } else if (!ty_is_integer(e->ty)) {
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
    case EXPR_BNOT:
        check_expr(u, f, sc, e->rhs);
        need_integer(u, e->rhs, e->kind == EXPR_NEG ? "unary '-'" : "'~'");
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
        need_scalar(u, e->rhs, "a cast");
        if (!ty_is_scalar(e->cast_ty))
            diag_fatal(u->file, e->line, "cannot cast to %s",
                       ty_name(e->cast_ty));
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
        if (ty_is_integer(a) && ty_is_integer(b)) {
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
        } else {
            diag_fatal(u->file, e->line,
                       "'?:' branches have incompatible types "
                       "(%s vs %s)", ty_name(a), ty_name(b));
        }
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
                need_integer(u, e->lhs, "arithmetic");
                need_integer(u, e->rhs, "arithmetic");
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
                need_integer(u, e->lhs, "comparison");
                need_integer(u, e->rhs, "comparison");
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
        default: /* MUL DIV MOD AND OR XOR */
            need_integer(u, e->lhs, "arithmetic");
            need_integer(u, e->rhs, "arithmetic");
            e->ty = arith_common(lt, rt);
            e->lhs = mk_cast(e->lhs, e->ty);
            e->rhs = mk_cast(e->rhs, e->ty);
            break;
        }
        break;
    }
    case EXPR_CALL: {
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
            need_scalar(u, e->args[i], "an argument");
            if (i < ft->nptypes)
                e->args[i] = convert_assign(u, e->args[i],
                                            ft->ptypes[i], "argument");
            else /* variadic tail: default argument promotions */
                e->args[i] = mk_cast(e->args[i],
                                     promote(e->args[i]->ty));
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
            if (s->expr) {
                check_expr(u, f, sc, s->expr);
                need_scalar(u, s->expr, "an initializer");
                s->expr = convert_assign(u, s->expr, s->dty,
                                         "initialization");
            }
            if (scope_find(sc, s->name) >= 0)
                diag_fatal(u->file, s->line,
                           "'%s' is already declared in '%s' (one flat "
                           "scope per function for now — rename it)",
                           s->name, f->name);
            s->var_index = scope_add(sc, s->name, s->dty);
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
                need_scalar(u, s->expr, "'return'");
                s->expr = convert_assign(u, s->expr, f->ret_ty, "return");
            }
            break;
        case STMT_EXPR:
            check_expr(u, f, sc, s->expr);
            break;
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
        case STMT_FOR:
            if (s->init)
                check_expr(u, f, sc, s->init);
            if (s->cond) { /* NULL = forever, left by 'break' */
                check_expr(u, f, sc, s->cond);
                need_scalar(u, s->cond, "'for'");
            }
            if (s->step)
                check_expr(u, f, sc, s->step);
            check_stmt(u, f, sc, s->body, 1, in_switch, 0);
            break;
        case STMT_BLOCK:
            check_stmt(u, f, sc, s->body, in_loop, in_switch, 0);
            break;
        }
    }
}

/* Conservative all-paths-return: a list returns if any statement in it
 * guarantees a return; if/else guarantees one only when both arms do;
 * loops never do. Refusing a maybe-missing return is honest —
 * miscompiling one is not (THE RULE). */
static int list_returns(struct stmt *s);

static int stmt_returns(struct stmt *s)
{
    switch (s->kind) {
    case STMT_RETURN:
        return 1;
    case STMT_BLOCK:
        return list_returns(s->body);
    case STMT_IF:
        return s->els && stmt_returns(s->thn) && stmt_returns(s->els);
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
    struct scope sc = { 0, 0, 0 };

    for (int i = 0; i < f->nparams; i++) {
        if (scope_find(&sc, f->params[i]) >= 0)
            diag_fatal(u->file, f->line,
                       "duplicate parameter '%s' in '%s'",
                       f->params[i], f->name);
        scope_add(&sc, f->params[i], f->param_tys[i]);
    }

    check_stmt(u, f, &sc, f->body, 0, 0, 0);

    if (f->ret_ty->kind != TY_VOID && !list_returns(f->body))
        diag_fatal(u->file, f->line,
                   "control may reach the end of '%s' — every path must "
                   "end in a return statement", f->name);

    f->nvars = sc.n;
    f->var_tys = xmalloc((size_t)(sc.n ? sc.n : 1) * sizeof *f->var_tys);
    for (int i = 0; i < sc.n; i++)
        f->var_tys[i] = sc.vars[i].ty;
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
        if (g->ty->kind == TY_PTR && g->has_init && g->init != 0)
            diag_fatal(u->file, g->line,
                       "a pointer global can only be initialized to 0 "
                       "for now");
        struct global *canon = find_global(u, g->name);
        if (find_func(u, g->name))
            diag_fatal(u->file, g->line,
                       "'%s' is declared as both a function and a "
                       "variable", g->name);
        if (canon == g) {
            g->defined = !g->is_extern;
            continue;
        }
        if (!ty_equal(canon->ty, g->ty))
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
        }
        canon->defined |= !g->is_extern;
        g->absorbed = 1;
    }
}

void sema_check(struct unit *u)
{
    merge_decls(u);
    merge_globals(u);

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
