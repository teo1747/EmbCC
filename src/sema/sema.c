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

/* IEEE-754 +infinity / quiet-NaN as doubles, built from their bit patterns so
 * the value is exact regardless of the host EmbCC runs on. Used to lower the
 * __builtin_inf/huge_val/nan family to a float constant. */
static double ieee_inf(void)
{
    union { unsigned long u; double d; } v;
    v.u = 0x7ff0000000000000UL;
    return v.d;
}
static double ieee_nan(void)
{
    union { unsigned long u; double d; } v;
    v.u = 0x7ff8000000000000UL;
    return v.d;
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
static struct expr *convert_assign(struct unit *u, struct expr *rhs,
                                   struct type *to, const char *ctx)
{
    if (to->kind == TY_STRUCT || rhs->ty->kind == TY_STRUCT) {
        if (!ty_equal(to, rhs->ty))
            diag_fatal(u->file, rhs->line, "%s: cannot convert %s to %s",
                       ctx, ty_name(rhs->ty), ty_name(to));
        return rhs; /* same struct type: passed/returned as its bytes */
    }
    /* Any scalar converts to _Bool implicitly (C99 6.3.1.2): the result is
     * 0 or 1. This includes pointers, which otherwise need an explicit cast
     * to an integer type. */
    if (to->kind == TY_BOOL && ty_is_scalar(rhs->ty))
        return mk_cast(rhs, to);
    if (ty_is_arith(to) && ty_is_arith(rhs->ty))
        return mk_cast(rhs, to);
    if (to->kind == TY_PTR) {
        if (rhs->ty->kind == TY_PTR &&
            (ty_equal(rhs->ty, to) || to->pointee->kind == TY_VOID ||
             rhs->ty->pointee->kind == TY_VOID))
            return mk_cast(rhs, to);
        /* Same-size integer pointees differing only in signedness
         * (`char *` vs `unsigned char *`): gcc warns but allows it, and real
         * code — string/buffer routines especially — relies on it. */
        if (rhs->ty->kind == TY_PTR &&
            ty_is_integer(to->pointee) && ty_is_integer(rhs->ty->pointee) &&
            ty_size(to->pointee) == ty_size(rhs->ty->pointee))
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
           e->kind == EXPR_MEMBER || e->kind == EXPR_COMPLIT;
}

static int const_fold(const struct expr *e, long *out);

/* A growing (offset, type, value) list of flattened initializer leaves —
 * defined here so both check_expr (compound literals) and check_stmt
 * (declarations) can build one. */
struct initbuf {
    struct initelem *v;
    int n, cap;
};
static void flatten_init(struct unit *u, struct func *f, struct scope *sc,
                         struct expr *init, struct type *ty, int off,
                         struct initbuf *out);
static void lower_static_bytes(struct unit *u, int line, int size,
                               struct initelem *v, int n,
                               const char **out_bytes,
                               struct greloc **out_rel, int *out_nrel);

/* Nonzero while lowering a static initializer (a file-scope global or a
 * static local). A compound literal met here has static storage: it becomes
 * an anonymous global rather than a stack slot. */
static int g_in_static_init;

/* Resolve `name` as a member of `base`, descending into any anonymous
 * struct/union members (C11 6.7.2.1p13: their members are reached as if
 * they belonged to the enclosing type). On success fills *out with the leaf
 * member — a copy, its offset made cumulative from `base` — and returns 1. */
static int find_member_deep(struct type *base, const char *name,
                            struct member *out, int base_off)
{
    for (int i = 0; i < base->nmembers; i++) {
        struct member *m = &base->members[i];
        if (m->name && strcmp(m->name, name) == 0) {
            *out = *m;
            out->off += base_off;
            return 1;
        }
        if (!m->name && !m->is_bitfield && m->ty->kind == TY_STRUCT &&
            find_member_deep(m->ty, name, out, base_off + m->off))
            return 1;
    }
    return 0;
}

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
            /* '<=' not '<': an enum is COMPLETE before the declarator that
             * uses it in the same declaration (`enum { A, B } g = B;`), so a
             * same-item (same seq) reference is legal -- the same reasoning the
             * global self-reference below uses. A truly earlier use (a lower
             * seq than the enum's) is still rejected. */
            if (ec && ec->seq <= cur_body_seq) {
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
            /* '<=' not '<': a global's own name is in scope within its
             * initializer (C11 6.2.1p7), so `void *p = &p` is legal; seqs
             * are unique, so this only ever admits that self-reference. */
            if (g && g->seq <= cur_body_seq) {
                e->gref = g;
                g->used = 1;
                e->ty = g->ty;
            } else if (g) {
                diag_fatal(u->file, e->line,
                           "'%s' is used before its declaration "
                           "(line %d)", e->name, g->line);
            } else if (find_func(u, e->name)) {
                struct func *fd = find_func(u, e->name);
                /* seq-based, not the ordered-walk `declared` flag: a function
                 * used as a value in a static initializer (a vtable) is
                 * lowered before that walk runs, but is still legal if the
                 * function was declared earlier in the source. */
                if (fd->seq > cur_body_seq)
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
        if (e->rhs->undecayed) {
            /* &arr yields a pointer to the whole ARRAY object, T(*)[N]; its
             * value is the array's address, which gen_addr already gives. */
            e->ty = ty_ptr(e->rhs->undecayed);
            break;
        }
        if (e->rhs->kind == EXPR_MEMBER && e->rhs->memb &&
            e->rhs->memb->is_bitfield)
            diag_fatal(u->file, e->line,
                       "cannot take the address of bitfield '%s'",
                       e->rhs->memb->name);
        e->ty = ty_ptr(e->rhs->ty);
        break;
    case EXPR_COMPLIT: {
        /* `(type){ init }` — an unnamed object with automatic storage in
         * this block. Give it a synthesized local slot and flatten the
         * initializer into it exactly like a declared aggregate; the
         * literal is an lvalue of that type (an array decays, a struct is
         * carried by address, a scalar is loaded). */
        struct type *ty = e->cast_ty;
        if (ty->kind == TY_VOID || ty->kind == TY_FUNC)
            diag_fatal(u->file, e->line,
                       "a compound literal cannot have type %s",
                       ty_name(ty));
        if (ty->kind == TY_STRUCT && !ty->complete)
            diag_fatal(u->file, e->line,
                       "compound literal of incomplete type %s",
                       ty_name(ty));
        /* `(int[]){...}` takes its size from the initializer, as `int a[]`
         * does. */
        if (ty->kind == TY_ARRAY && ty->count == 0 &&
            e->lhs->kind == EXPR_INITLIST)
            ty = ty_array(ty->pointee, initlist_array_count(e->lhs));
        e->cast_ty = ty;
        if (g_in_static_init) {
            /* Static storage: the literal is an anonymous global, and this
             * node becomes a reference to it (so `&(T){...}` in a static
             * initializer lowers to a relocation like any `&global`). */
            struct initbuf ib = { 0, 0, 0 };
            flatten_init(u, f, sc, e->lhs, ty, 0, &ib);
            static int anon_seq;
            struct global *g = xcalloc(1, sizeof *g);
            char *nm = xmalloc(24);
            snprintf(nm, 24, ".Lcomplit.%d", anon_seq++);
            g->name = nm;
            g->line = e->line;
            g->seq = -1;
            g->ty = ty;
            g->is_static = 1;
            g->defined = 1;
            g->used = 1;
            g->has_init = 1;   /* it has real bytes -> .data, not .bss */
            lower_static_bytes(u, e->line, ty_size(ty), ib.v, ib.n,
                               &g->init_bytes, &g->relocs, &g->nrelocs);
            g->init_len = ty_size(ty);
            struct global **gt = &u->globals;
            while (*gt) gt = &(*gt)->next;
            *gt = g;
            e->kind = EXPR_VAR;      /* an lvalue naming the anonymous global */
            e->gref = g;
            e->name = g->name;
            e->inits = NULL; e->ninits = 0; e->lhs = NULL;
            if (ty->kind == TY_ARRAY) {
                e->undecayed = ty;
                e->ty = ty_ptr(ty->pointee);
            } else {
                e->ty = ty;
            }
            break;
        }
        e->var_index = scope_add(sc, "<compound literal>", ty, NULL);
        struct initbuf ib = { 0, 0, 0 };
        flatten_init(u, f, sc, e->lhs, ty, 0, &ib);
        e->inits = ib.v;
        e->ninits = ib.n;
        e->lhs = NULL;
        if (ty->kind == TY_ARRAY) {
            e->undecayed = ty;   /* the object; e->ty is the decayed pointer */
            e->ty = ty_ptr(ty->pointee);
        } else {
            e->ty = ty;
        }
        break;
    }
    case EXPR_GENERIC: {
        /* Pick the association whose type matches the controlling
         * expression's (after its lvalue conversion — an array/function
         * operand already carries its decayed type here), else `default`.
         * The controlling expression is not evaluated (C11 6.5.1.1); the
         * node simply becomes the selected expression. */
        check_expr(u, f, sc, e->lhs);
        struct expr *chosen = NULL, *deflt = NULL;
        for (int i = 0; i < e->ngen; i++) {
            if (!e->gtypes[i]) { deflt = e->gexprs[i]; continue; }
            if (ty_equal(e->lhs->ty, e->gtypes[i])) {
                chosen = e->gexprs[i];
                break;
            }
        }
        if (!chosen)
            chosen = deflt;
        if (!chosen)
            diag_fatal(u->file, e->line,
                       "no _Generic association matches type %s",
                       ty_name(e->lhs->ty));
        check_expr(u, f, sc, chosen);
        *e = *chosen;   /* become the selected expression */
        break;
    }
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
        struct member *mm = xcalloc(1, sizeof *mm);
        if (!find_member_deep(base, e->name, mm, 0))
            diag_fatal(u->file, e->line, "%s has no member '%s'",
                       ty_name(base), e->name);
        e->memb = mm;
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
        /* Compiler builtins for the floating specials: lower directly to a
         * constant with the right bit pattern (HUGE_VAL/INFINITY/NAN expand to
         * these). The `f` variants are single precision; nan's string arg is
         * evaluated for side effects only (there are none) and ignored. */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            strncmp(e->lhs->name, "__builtin_", 10) == 0) {
            const char *bn = e->lhs->name + 10;
            int is_inf = strcmp(bn, "inf") == 0 || strcmp(bn, "huge_val") == 0;
            int is_inff = strcmp(bn, "inff") == 0 || strcmp(bn, "huge_valf") == 0;
            int is_nan = strcmp(bn, "nan") == 0;
            int is_nanf = strcmp(bn, "nanf") == 0;
            if (is_inf || is_inff || is_nan || is_nanf) {
                for (int i = 0; i < e->nargs; i++)
                    check_expr(u, f, sc, e->args[i]);
                e->kind = EXPR_FNUM;
                e->fnum = (is_nan || is_nanf) ? ieee_nan() : ieee_inf();
                e->ty = ty_base((is_inff || is_nanf) ? TY_FLOAT : TY_DOUBLE, 0);
                break;
            }
            /* __builtin_memcpy/memmove/memset are the libc functions under a
             * reserved name -- rename and let the ordinary call path resolve
             * them (the program must declare/provide them). */
            if (strcmp(bn, "memcpy") == 0 || strcmp(bn, "memmove") == 0 ||
                strcmp(bn, "memset") == 0)
                e->lhs->name = bn;   /* fall through to normal call handling */
            /* byte swaps -> a single instruction; the result is the argument's
             * width as an unsigned integer. */
            else if (strcmp(bn, "bswap16") == 0 || strcmp(bn, "bswap32") == 0 ||
                     strcmp(bn, "bswap64") == 0) {
                if (e->nargs != 1)
                    diag_fatal(u->file, e->line, "%s takes one argument",
                               e->lhs->name);
                check_expr(u, f, sc, e->args[0]);
                need_integer(u, e->args[0], "__builtin_bswap");
                e->name = e->lhs->name;
                e->ty = ty_base(bn[5] == '1' ? TY_SHORT :
                                bn[5] == '3' ? TY_INT : TY_LONG, 1);
                break;
            }
            /* the value IS the first argument; the hint is discarded */
            else if (strcmp(bn, "expect") == 0) {
                if (e->nargs < 1)
                    diag_fatal(u->file, e->line,
                               "__builtin_expect takes two arguments");
                for (int i = 0; i < e->nargs; i++)
                    check_expr(u, f, sc, e->args[i]);
                *e = *e->args[0];
                break;
            }
            /* control never reaches here -> a trap (ud2) */
            else if (strcmp(bn, "unreachable") == 0) {
                e->name = e->lhs->name;
                e->ty = ty_base(TY_VOID, 0);
                break;
            }
        }
        /* a full memory barrier (not spelled __builtin_) */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            strcmp(e->lhs->name, "__sync_synchronize") == 0) {
            e->name = e->lhs->name;
            e->ty = ty_base(TY_VOID, 0);
            break;
        }
        /* __atomic_load_n / store_n / exchange_n. The first argument is a
         * pointer; x86 makes an aligned scalar load/store atomic on its own,
         * exchange is a locked xchg, and a store gets a trailing fence for
         * seq_cst. The memory-order argument is checked and then ignored. */
        if (e->lhs->kind == EXPR_VAR && e->lhs->name &&
            (strcmp(e->lhs->name, "__atomic_load_n") == 0 ||
             strcmp(e->lhs->name, "__atomic_store_n") == 0 ||
             strcmp(e->lhs->name, "__atomic_exchange_n") == 0 ||
             strcmp(e->lhs->name, "__atomic_fetch_add") == 0 ||
             strcmp(e->lhs->name, "__atomic_fetch_sub") == 0 ||
             strcmp(e->lhs->name, "__atomic_compare_exchange_n") == 0)) {
            for (int i = 0; i < e->nargs; i++)
                check_expr(u, f, sc, e->args[i]);
            if (e->nargs < 1 || e->args[0]->ty->kind != TY_PTR)
                diag_fatal(u->file, e->line,
                           "%s needs a pointer first argument", e->lhs->name);
            e->name = e->lhs->name;
            if (strcmp(e->lhs->name, "__atomic_store_n") == 0)
                e->ty = ty_base(TY_VOID, 0);
            else if (strcmp(e->lhs->name,
                            "__atomic_compare_exchange_n") == 0)
                e->ty = ty_base(TY_BOOL, 0);       /* did the swap happen? */
            else
                e->ty = e->args[0]->ty->pointee;
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
            if (callee->seq > cur_body_seq)
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

/* Fold a FLOATING constant expression to a double, for static initializers of
 * float/double storage (`double g = 1.0/3.0;`). Integer leaves promote to
 * double; a cast to an integer type truncates (C semantics), a cast to float
 * rounds to single precision. Returns 0 (not constant) if it can't reduce. */
static int const_fold_f(const struct expr *e, double *out)
{
    double a, b; long iv;
    switch (e->kind) {
    case EXPR_FNUM:
        *out = e->fnum;
        return 1;
    case EXPR_NUM:
        *out = (double)e->num;      /* an integer constant used where a float is wanted */
        return 1;
    case EXPR_CAST:
        if (ty_is_float(e->ty)) {
            if (!const_fold_f(e->rhs, &a)) return 0;
            *out = (e->ty->kind == TY_FLOAT) ? (double)(float)a : a;
            return 1;
        }
        if (ty_is_integer(e->ty)) {   /* (int)f : evaluate then truncate toward zero */
            if (const_fold_f(e->rhs, &a)) { *out = (double)(long)a; return 1; }
            if (const_fold(e->rhs, &iv)) { *out = (double)iv; return 1; }
        }
        return 0;
    case EXPR_NEG:
        if (!const_fold_f(e->rhs, &a)) return 0;
        *out = -a;
        return 1;
    case EXPR_BINOP:
        if (!const_fold_f(e->lhs, &a) || !const_fold_f(e->rhs, &b))
            return 0;
        switch (e->op) {
        case B_ADD: *out = a + b; return 1;
        case B_SUB: *out = a - b; return 1;
        case B_MUL: *out = a * b; return 1;
        case B_DIV: *out = a / b; return 1;   /* x/0.0 is inf/nan -- valid float result */
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
    b->v[b->n].bit_off = 0;
    b->v[b->n].bit_width = 0;
    b->n++;
}

/* A bitfield leaf: same as init_push but recording where in the storage
 * unit at `off` the value lands, so the lowerings mask and merge it. */
static void init_push_bf(struct initbuf *b, int off, struct type *ty,
                         struct expr *e, int bit_off, int bit_width)
{
    init_push(b, off, ty, e);
    b->v[b->n - 1].bit_off = bit_off;
    b->v[b->n - 1].bit_width = bit_width;
}

static void flatten_init(struct unit *u, struct func *f, struct scope *sc,
                         struct expr *init, struct type *ty, int off,
                         struct initbuf *out)
{
    /* A compound literal used AS an initializer is exactly its brace
     * initializer placed at this offset — `T g = (T){...}` (even at file
     * scope) and a literal nested in another initializer need no separate
     * object. Only an unbraced literal reaches here as `init` (a plain
     * `expr` initializer keeps EXPR_COMPLIT); `&(T){...}` stays an
     * EXPR_ADDR and is lowered through the anonymous-object path instead. */
    if (init->kind == EXPR_COMPLIT) {
        if (init->lhs && init->lhs->kind == EXPR_INITLIST) {
            flatten_init(u, f, sc, init->lhs, ty, off, out);
            return;
        }
    }
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
        /* Positional by default; a `[i] =` designator repositions the
         * running index and initialization continues positionally after it
         * (later writes to the same slot win, matching C). */
        int ai = 0;
        for (int i = 0; i < init->nelems; i++) {
            struct expr *el = init->elems[i];
            if (el->desig_field)
                diag_fatal(u->file, el->line,
                           "field designator '.%s' in an array initializer",
                           el->desig_field);
            if (el->desig_index >= 0)
                ai = el->desig_index;
            if (ty->count && ai >= ty->count)
                diag_fatal(u->file, el->line,
                           "initializer index %d is past the end of an "
                           "array of %d", ai, ty->count);
            flatten_init(u, f, sc, el, ty->pointee, off + ai * esz, out);
            ai++;
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
            /* An unnamed bitfield (padding, or a `:0` separator) takes no
             * initializer — skip past it in positional order. */
            while (mi < ty->nmembers && ty->members[mi].is_bitfield &&
                   !ty->members[mi].name)
                mi++;
            if (mi >= ty->nmembers)
                diag_fatal(u->file, init->line,
                           "too many initializers for %s, which has %d "
                           "members", ty_name(ty), ty->nmembers);
            struct member *m = &ty->members[mi];
            if (m->is_bitfield) {
                /* A bitfield leaf: its value is masked and merged into the
                 * shared storage unit at m->off by both lowerings, so it
                 * carries (bit_off, bit_width) rather than a byte width. */
                check_expr(u, f, sc, el);
                need_scalar(u, el, "a bitfield initializer");
                struct expr *cv = convert_assign(u, el, m->ty,
                                                 "initialization");
                init_push_bf(out, off + m->off, m->ty, cv,
                             m->bit_off, m->bit_width);
                mi++;
                continue;
            }
            flatten_init(u, f, sc, el, m->ty, off + m->off, out);
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
        /* The address of a global: `&g`, or an array/function global that
         * decayed to a pointer (`char **environ = embk_empty_env`). */
        struct global *gt = NULL;
        struct func *ft = NULL;
        if (core && core->kind == EXPR_VAR && core->gref)
            gt = core->gref;
        else if (core && core->kind == EXPR_VAR && core->fref)
            ft = core->fref;             /* a function address (a vtable) */
        else if (core && core->kind == EXPR_ADDR) {
            struct expr *in = core->rhs;
            while (in && in->kind == EXPR_CAST)
                in = in->rhs;
            if (in && in->kind == EXPR_VAR && in->gref)
                gt = in->gref;
            else if (in && in->kind == EXPR_VAR && in->fref)
                ft = in->fref;
        }
        if ((core && core->kind == EXPR_STR && v[k].ty->kind == TY_PTR) ||
            ((gt || ft) && v[k].ty->kind == TY_PTR)) {
            /* a pointer slot: zero bytes stay, the linker writes the address
             * of a string literal, a global, or a function. */
            if (nrel == caprel) {
                caprel = caprel ? caprel * 2 : 4;
                rel = xrealloc(rel, (size_t)caprel * sizeof *rel);
            }
            rel[nrel].off = v[k].off;
            rel[nrel].str = (gt || ft) ? NULL : core->name;
            rel[nrel].str_len = (gt || ft) ? 0 : (int)core->num;
            rel[nrel].gtarget = gt;
            rel[nrel].ftarget = ft;
            rel[nrel].addend = 0;
            nrel++;
            continue;
        }
        int sz = ty_size(v[k].ty);
        /* A float/double slot: fold to the value, store its IEEE-754 bit
         * pattern (4 bytes for float, 8 for double), little-endian. */
        if (ty_is_float(v[k].ty)) {
            double dv;
            if (!const_fold_f(v[k].e, &dv))
                diag_fatal(u->file, line,
                           "a static float initializer must be a constant "
                           "expression");
            unsigned long ubits;
            if (v[k].ty->kind == TY_FLOAT) {
                float fv = (float)dv; unsigned int u32;
                memcpy(&u32, &fv, 4); ubits = u32;
            } else {
                memcpy(&ubits, &dv, 8);
            }
            for (int b = 0; b < sz; b++)
                bytes[v[k].off + b] = (char)(ubits >> (8 * b));
            continue;
        }
        long cv;
        if (!const_fold(v[k].e, &cv))
            diag_fatal(u->file, line,
                       "a static initializer must be a constant, a "
                       "string literal, or the address of a global");
        if (v[k].bit_width) {
            /* merge the field's bits into its storage unit (bytes start
             * zeroed, so OR is enough and neighbours are preserved) */
            unsigned long mask = v[k].bit_width >= 64
                               ? ~0UL : (((unsigned long)1 << v[k].bit_width) - 1);
            unsigned long field = ((unsigned long)cv & mask) << v[k].bit_off;
            for (int b = 0; b < sz; b++)
                bytes[v[k].off + b] |= (char)(field >> (8 * b));
            continue;
        }
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
        cur_body_seq = g->def_seq;
        struct scope sc = { 0, 0, 0, 0 };
        struct initbuf ib = { 0, 0, 0 };
        g_in_static_init++;
        flatten_init(u, &gf, &sc, g->init_expr, g->ty, 0, &ib);
        g_in_static_init--;
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

/* Map a fixed-register constraint letter to its register (0-15), else -1. */
static int asm_fixed_letter(char c)
{
    switch (c) {
    case 'a': return 0;
    case 'b': return 3;
    case 'c': return 1;
    case 'd': return 2;
    case 'S': return 6;
    case 'D': return 7;
    }
    return -1;
}

/* The register a constraint pins its operand to: a fixed one for a/b/c/d/S/D,
 * the bound register of a `register T x __asm__("r10")` variable, or the
 * sentinel -2 for an allocatable class (r/q/g/m/R) that irgen assigns from a
 * free register. Output constraints carry a leading '=' or '+'; '&' is
 * accepted and ignored. */
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
    for (const char *p = c; *p; p++) {           /* a fixed register wins */
        int r = asm_fixed_letter(*p);
        if (r >= 0)
            return r;
    }
    if (op->expr->kind == EXPR_VAR && op->expr->asm_reg) {
        int r = asm_reg_by_name(op->expr->asm_reg);
        if (r >= 0)
            return r;
    }
    for (const char *p = c; *p; p++)             /* else allocate a register */
        if (*p == 'r' || *p == 'q' || *p == 'g' || *p == 'm' || *p == 'R')
            return -2;
    for (const char *p = c; *p; p++)             /* an SSE/XMM ('x') operand */
        if (*p == 'x')
            return -3;                            /* irgen allocates an xmm */
    diag_fatal(u->file, s->line,
               "asm constraint \"%s\" is not supported "
               "(EmbCC handles a/b/c/d/S/D, 'r'/'q'/'g'/'m', 'x', and a "
               "register-asm variable)", op->constraint);
    return -1;
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
            if (s->is_extern) {
                /* block-scope extern: no storage here, external linkage. Register
                 * the unit global/function (safe now -- parsing is done, so the
                 * list tails are no longer live) if not already present, and for
                 * a variable wire a block-scope entry onto it so references
                 * resolve. A function needs no var entry (calls use find_func). */
                if (s->dty->kind == TY_FUNC) {
                    if (!find_func(u, s->name)) {
                        struct func *g = xcalloc(1, sizeof *g);
                        g->name = s->name; g->file = u->file; g->line = s->line;
                        g->seq = f->seq; g->declared = 1;
                        g->ret_ty = s->dty->ret;
                        g->nparams = s->dty->nptypes;
                        for (int k = 0; k < s->dty->nptypes; k++)
                            g->param_tys[k] = s->dty->ptypes[k];
                        g->is_varargs = s->dty->is_varargs;
                        struct func **ft = &u->funcs;
                        while (*ft) ft = &(*ft)->next;
                        *ft = g;
                    }
                } else {
                    struct global *g = find_global(u, s->name);
                    if (!g) {
                        g = xcalloc(1, sizeof *g);
                        g->name = s->name; g->ty = s->dty; g->is_extern = 1;
                        g->file = u->file; g->line = s->line;
                        g->seq = f->seq; g->def_seq = f->seq;
                        struct global **gt = &u->globals;
                        while (*gt) gt = &(*gt)->next;
                        *gt = g;
                    }
                    s->var_index = scope_add(sc, s->name, s->dty, NULL);
                    sc->vars[s->var_index].g = g;
                    s->sglob = g;   /* irgen: this decl carries no local storage */
                }
                break;
            }
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
                /* an omitted array size is the highest index reached */
                s->dty = ty_array(s->dty->pointee,
                                  initlist_array_count(s->expr));
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
            /* A static local has static storage, so a compound literal in its
             * initializer is an anonymous global, not a stack slot. */
            if (s->is_static)
                g_in_static_init++;
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
            if (s->is_static)
                g_in_static_init--;
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
            /* the template is assembled in irgen, once -2 (allocatable)
             * operands have been assigned registers */
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
        case STMT_LABEL:
            /* the labeled statement is checked in the label's own context */
            check_stmt(u, f, sc, s->body, in_loop, in_switch, 0);
            break;
        case STMT_GOTO:
            /* target existence is validated function-wide at codegen */
            break;
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
        "__builtin_unreachable", "__builtin_trap",
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
    case STMT_GOTO:
        /* an unconditional jump: control leaves here, it does not fall off
         * the end of the function at this point. */
        return 1;
    case STMT_LABEL:
        return stmt_returns(s->body);
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
        canon->is_weak |= f->is_weak;  /* weak on any declaration is weak */
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
            canon->def_seq = g->seq;   /* the initializer's real position */
        }
        canon->defined |= !g->is_extern;
        canon->is_weak |= g->is_weak;
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
