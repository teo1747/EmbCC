/* Name resolution and the checks the subset needs. Everything is int,
 * so there is no type inference to do — what remains is exactly the set
 * of ways a program could silently lie: unknown names, arity mismatches,
 * calls that would need a relocation (externals), and control flow
 * falling off the end of a function.
 */
#include "sema.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

struct scope {
    const char **names;
    int n, cap;
};

static int scope_find(struct scope *sc, const char *name)
{
    for (int i = 0; i < sc->n; i++)
        if (strcmp(sc->names[i], name) == 0)
            return i;
    return -1;
}

static int scope_add(struct scope *sc, const char *name)
{
    if (sc->n == sc->cap) {
        sc->cap = sc->cap ? sc->cap * 2 : 8;
        sc->names = xrealloc(sc->names, (size_t)sc->cap * sizeof *sc->names);
    }
    sc->names[sc->n] = name;
    return sc->n++;
}

/* Returns the canonical node for a name: the first declaration, into
 * which any later definition has been merged. */
static struct func *find_func(struct unit *u, const char *name)
{
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && strcmp(f->name, name) == 0)
            return f;
    return NULL;
}

static void check_expr(struct unit *u, struct func *f, struct scope *sc,
                       struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
        break;
    case EXPR_VAR: {
        int i = scope_find(sc, e->name);
        if (i < 0) {
            if (find_func(u, e->name))
                diag_fatal(u->file, e->line,
                           "'%s' is a function; taking its address is "
                           "not supported yet", e->name);
            diag_fatal(u->file, e->line,
                       "'%s' is not declared in '%s'", e->name, f->name);
        }
        e->var_index = i;
        break;
    }
    case EXPR_ASSIGN: {
        int i = scope_find(sc, e->name);
        if (i < 0)
            diag_fatal(u->file, e->line,
                       "assignment to '%s', which is not declared in '%s'",
                       e->name, f->name);
        e->var_index = i;
        check_expr(u, f, sc, e->rhs);
        break;
    }
    case EXPR_NOT:
    case EXPR_NEG:
    case EXPR_BNOT:
        check_expr(u, f, sc, e->rhs);
        break;
    case EXPR_INCDEC: {
        int i = scope_find(sc, e->name);
        if (i < 0)
            diag_fatal(u->file, e->line,
                       "++/-- on '%s', which is not declared in '%s'",
                       e->name, f->name);
        e->var_index = i;
        break;
    }
    case EXPR_BINOP:
        check_expr(u, f, sc, e->lhs);
        check_expr(u, f, sc, e->rhs);
        break;
    case EXPR_CALL: {
        struct func *callee = find_func(u, e->name);
        if (!callee)
            diag_fatal(u->file, e->line,
                       "call to '%s', which is not declared — add a "
                       "prototype ('int %s(...);') or define it first",
                       e->name, e->name);
        /* C99 has no implicit declarations; a declaration (prototype
         * or definition) must precede the call, or the program is not
         * the strict C99 subset the golden tests hold us to. */
        if (!callee->declared)
            diag_fatal(u->file, e->line,
                       "call to '%s' before its declaration — declare "
                       "or define functions before their callers",
                       e->name);
        if (e->nargs != callee->nparams)
            diag_fatal(u->file, e->line,
                       "'%s' takes %d argument%s, called with %d",
                       e->name, callee->nparams,
                       callee->nparams == 1 ? "" : "s", e->nargs);
        e->callee = callee;
        callee->used = 1;
        for (int i = 0; i < e->nargs; i++)
            check_expr(u, f, sc, e->args[i]);
        break;
    }
    }
}

/* Declarations anywhere in the function share one flat scope, and
 * shadowing is rejected outright. C gives inner blocks their own scope;
 * refusing shadowed names accepts strictly fewer programs than C does,
 * so the subset stays a subset. Real block scoping arrives with sema's
 * M2 growth. */
static void check_stmt(struct unit *u, struct func *f, struct scope *sc,
                       struct stmt *s, int in_loop)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_BREAK:
        case STMT_CONTINUE:
            if (!in_loop)
                diag_fatal(u->file, s->line,
                           "'%s' outside of a loop",
                           s->kind == STMT_BREAK ? "break" : "continue");
            break;
        case STMT_DECL:
            if (s->expr)
                check_expr(u, f, sc, s->expr);
            if (scope_find(sc, s->name) >= 0)
                diag_fatal(u->file, s->line,
                           "'%s' is already declared in '%s' (one flat "
                           "scope per function for now — rename it)",
                           s->name, f->name);
            s->var_index = scope_add(sc, s->name);
            break;
        case STMT_RETURN:
        case STMT_EXPR:
            check_expr(u, f, sc, s->expr);
            break;
        case STMT_IF:
            check_expr(u, f, sc, s->cond);
            check_stmt(u, f, sc, s->thn, in_loop);
            if (s->els)
                check_stmt(u, f, sc, s->els, in_loop);
            break;
        case STMT_WHILE:
            check_expr(u, f, sc, s->cond);
            check_stmt(u, f, sc, s->body, 1);
            break;
        case STMT_FOR:
            if (s->init)
                check_expr(u, f, sc, s->init);
            if (s->cond) /* NULL = forever, left by 'break' */
                check_expr(u, f, sc, s->cond);
            if (s->step)
                check_expr(u, f, sc, s->step);
            check_stmt(u, f, sc, s->body, 1);
            break;
        case STMT_BLOCK:
            check_stmt(u, f, sc, s->body, in_loop);
            break;
        }
    }
}

/* Conservative all-paths-return: a list returns if any statement in it
 * guarantees a return (whatever follows is unreachable); if/else
 * guarantees one only when both arms do; loops never do (the condition
 * may be false on entry). Refusing a maybe-missing return is honest —
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
        scope_add(&sc, f->params[i]);
    }

    check_stmt(u, f, &sc, f->body, 0);

    if (!list_returns(f->body))
        diag_fatal(u->file, f->line,
                   "control may reach the end of '%s' — every path must "
                   "end in a return statement", f->name);

    f->nvars = sc.n;
    free(sc.names);
}

/* Merge every later declaration of a name into its first (canonical)
 * node, so calls resolve to one place whether the definition came
 * before or after them. C's static rule is kept exactly: static-then-
 * non-static keeps internal linkage, non-static-then-static is an
 * error (matching gcc, so the subset stays strict). */
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
        if (canon->nparams != f->nparams)
            diag_fatal(u->file, f->line,
                       "'%s' declared with %d parameter%s but %d earlier "
                       "(line %d)", f->name, f->nparams,
                       f->nparams == 1 ? "" : "s", canon->nparams,
                       canon->line);
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

void sema_check(struct unit *u)
{
    merge_decls(u);

    /* Walk in source order so `declared` mirrors C's rule exactly: a
     * name is usable from its first declaration on, and a body is
     * checked at its DEFINITION's position — everything declared above
     * the definition is in scope inside it, prototype or not. */
    for (struct func *f = u->funcs; f; f = f->next) {
        struct func *canon = find_func(u, f->name);
        if (canon == f)
            f->declared = 1;
        if (f->defined)
            check_func(u, canon);
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
