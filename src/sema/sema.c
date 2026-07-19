/* Name resolution and the checks the M1 subset needs. Everything is
 * int, so there is no type inference to do — what remains is exactly
 * the set of ways a program could silently lie: unknown names, arity
 * mismatches, calls that would need a relocation (externals), and
 * control flow falling off the end of a function.
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

static struct func *find_func(struct unit *u, const char *name)
{
    for (struct func *f = u->funcs; f; f = f->next)
        if (strcmp(f->name, name) == 0)
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
                           "not supported (M1 subset)", e->name);
            diag_fatal(u->file, e->line,
                       "'%s' is not declared in '%s'", e->name, f->name);
        }
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
                       "call to '%s', which is not defined in this file — "
                       "external calls need relocations and arrive after "
                       "M1 (see docs/ROADMAP.md)", e->name);
        /* The M1 subset must stay a strict subset of C99, or programs
         * EmbCC accepts stop compiling under gcc and every golden
         * comparison breaks. C99 has no implicit declarations, and M1
         * has no prototypes — so definition must precede use. */
        if (!callee->declared)
            diag_fatal(u->file, e->line,
                       "call to '%s' before its definition — M1 has no "
                       "prototypes; define functions before their callers",
                       e->name);
        if (e->nargs != callee->nparams)
            diag_fatal(u->file, e->line,
                       "'%s' takes %d argument%s, called with %d",
                       e->name, callee->nparams,
                       callee->nparams == 1 ? "" : "s", e->nargs);
        e->callee = callee;
        for (int i = 0; i < e->nargs; i++)
            check_expr(u, f, sc, e->args[i]);
        break;
    }
    }
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

    struct stmt *last = NULL;
    for (struct stmt *s = f->body; s; s = s->next) {
        last = s;
        switch (s->kind) {
        case STMT_DECL:
            if (s->expr)
                check_expr(u, f, &sc, s->expr);
            if (scope_find(&sc, s->name) >= 0)
                diag_fatal(u->file, s->line,
                           "'%s' is already declared in '%s'",
                           s->name, f->name);
            s->var_index = scope_add(&sc, s->name);
            break;
        case STMT_RETURN:
            check_expr(u, f, &sc, s->expr);
            break;
        }
    }

    /* All functions return int; falling off the end would return
     * whatever is in eax. Refuse rather than miscompile (THE RULE). */
    if (!last || last->kind != STMT_RETURN)
        diag_fatal(u->file, f->line,
                   "control reaches the end of '%s' — every function "
                   "must end in a return statement (M1 subset)", f->name);

    f->nvars = sc.n;
    free(sc.names);
}

void sema_check(struct unit *u)
{
    for (struct func *f = u->funcs; f; f = f->next)
        for (struct func *g = f->next; g; g = g->next)
            if (strcmp(f->name, g->name) == 0)
                diag_fatal(u->file, g->line, "redefinition of '%s'", f->name);

    for (struct func *f = u->funcs; f; f = f->next) {
        /* Declared before its own body is checked: recursion is legal,
         * exactly as in C, where the declarator precedes the body. */
        f->declared = 1;
        check_func(u, f);
    }
}
