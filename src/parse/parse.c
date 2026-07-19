#include "parse.h"

#include <stdlib.h>

#include "../driver/util.h"
#include "../lex/lex.h"

struct parser {
    struct lexer lx;
};

static struct token *cur(struct parser *ps) { return &ps->lx.tok; }
static void advance(struct parser *ps) { lex_next(&ps->lx); }

static void expect(struct parser *ps, enum tok_kind kind, const char *what)
{
    if (cur(ps)->kind != kind)
        diag_fatal(ps->lx.file, cur(ps)->line, "expected %s before %s",
                   what, tok_describe(cur(ps)));
    advance(ps);
}

static struct expr *new_expr(enum expr_kind kind, int line)
{
    struct expr *e = xcalloc(1, sizeof *e);
    e->kind = kind;
    e->line = line;
    return e;
}

static struct expr *parse_expr(struct parser *ps);

static struct expr *parse_primary(struct parser *ps)
{
    struct token *t = cur(ps);
    struct expr *e;

    switch (t->kind) {
    case TOK_NUM:
        e = new_expr(EXPR_NUM, t->line);
        e->num = t->num;
        advance(ps);
        return e;
    case TOK_LPAREN:
        advance(ps);
        e = parse_expr(ps);
        expect(ps, TOK_RPAREN, "')'");
        return e;
    case TOK_IDENT: {
        const char *name = t->text;
        int line = t->line;
        advance(ps);
        if (cur(ps)->kind != TOK_LPAREN) {
            e = new_expr(EXPR_VAR, line);
            e->name = name;
            return e;
        }
        advance(ps); /* '(' */
        e = new_expr(EXPR_CALL, line);
        e->name = name;
        if (cur(ps)->kind != TOK_RPAREN) {
            for (;;) {
                if (e->nargs >= MAX_PARAMS)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "more than %d call arguments "
                               "(M1 subset: register args only)",
                               MAX_PARAMS);
                e->args[e->nargs++] = parse_expr(ps);
                if (cur(ps)->kind != TOK_COMMA)
                    break;
                advance(ps);
            }
        }
        expect(ps, TOK_RPAREN, "')'");
        return e;
    }
    case TOK_MINUS:
        diag_fatal(ps->lx.file, t->line,
                   "unary minus is not supported (M1 subset); "
                   "write '0 - x'");
        return NULL;
    default:
        diag_fatal(ps->lx.file, t->line, "expected an expression, got %s",
                   tok_describe(t));
        return NULL;
    }
}

static struct expr *parse_mul(struct parser *ps)
{
    struct expr *e = parse_primary(ps);
    while (cur(ps)->kind == TOK_STAR) {
        struct expr *b = new_expr(EXPR_BINOP, cur(ps)->line);
        advance(ps);
        b->op = '*';
        b->lhs = e;
        b->rhs = parse_primary(ps);
        e = b;
    }
    return e;
}

static struct expr *parse_expr(struct parser *ps)
{
    struct expr *e = parse_mul(ps);
    while (cur(ps)->kind == TOK_PLUS || cur(ps)->kind == TOK_MINUS) {
        struct expr *b = new_expr(EXPR_BINOP, cur(ps)->line);
        b->op = cur(ps)->kind == TOK_PLUS ? '+' : '-';
        advance(ps);
        b->lhs = e;
        b->rhs = parse_mul(ps);
        e = b;
    }
    return e;
}

static struct stmt *parse_stmt(struct parser *ps)
{
    struct token *t = cur(ps);
    struct stmt *s = xcalloc(1, sizeof *s);
    s->line = t->line;

    switch (t->kind) {
    case TOK_KW_RETURN:
        advance(ps);
        s->kind = STMT_RETURN;
        s->expr = parse_expr(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_INT:
        advance(ps);
        if (cur(ps)->kind == TOK_STAR)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "pointers are not supported (M1 subset)");
        if (cur(ps)->kind != TOK_IDENT)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a variable name before %s",
                       tok_describe(cur(ps)));
        s->kind = STMT_DECL;
        s->name = cur(ps)->text;
        advance(ps);
        if (cur(ps)->kind == TOK_ASSIGN) {
            advance(ps);
            s->expr = parse_expr(ps);
        }
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_IDENT:
        /* Covers both keywords outside the subset (if, while, for, ...)
         * and assignments — neither exists in M1, and both must be
         * refused, not guessed at. */
        diag_fatal(ps->lx.file, t->line,
                   "statement starting with '%s' is not supported "
                   "(M1 subset: 'int' declarations and 'return' only)",
                   t->text);
        return NULL;
    default:
        diag_fatal(ps->lx.file, t->line,
                   "expected a statement, got %s", tok_describe(t));
        return NULL;
    }
}

static struct func *parse_func(struct parser *ps)
{
    struct func *f = xcalloc(1, sizeof *f);

    if (cur(ps)->kind == TOK_KW_STATIC) {
        f->is_static = 1;
        advance(ps);
    }
    if (cur(ps)->kind != TOK_KW_INT)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected 'int' before %s "
                   "(M1 subset: int is the only type)",
                   tok_describe(cur(ps)));
    advance(ps);
    if (cur(ps)->kind == TOK_STAR)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "pointers are not supported (M1 subset)");
    if (cur(ps)->kind != TOK_IDENT)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected a function name before %s",
                   tok_describe(cur(ps)));
    f->name = cur(ps)->text;
    f->line = cur(ps)->line;
    advance(ps);
    expect(ps, TOK_LPAREN, "'('");

    if (cur(ps)->kind == TOK_KW_VOID) {
        advance(ps);
    } else if (cur(ps)->kind != TOK_RPAREN) {
        for (;;) {
            if (cur(ps)->kind != TOK_KW_INT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected 'int' parameter before %s "
                           "(M1 subset: int is the only type)",
                           tok_describe(cur(ps)));
            advance(ps);
            if (cur(ps)->kind != TOK_IDENT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a parameter name before %s",
                           tok_describe(cur(ps)));
            if (f->nparams >= MAX_PARAMS)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "more than %d parameters "
                           "(M1 subset: register args only)", MAX_PARAMS);
            f->params[f->nparams++] = cur(ps)->text;
            advance(ps);
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')'");

    if (cur(ps)->kind == TOK_SEMI)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "function declarations without a body are not "
                   "supported (M1: every called function is defined "
                   "in this file)");
    expect(ps, TOK_LBRACE, "'{'");

    struct stmt **tail = &f->body;
    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind == TOK_EOF)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "unexpected end of file inside '%s'", f->name);
        *tail = parse_stmt(ps);
        tail = &(*tail)->next;
    }
    advance(ps); /* '}' */
    return f;
}

struct unit *parse_unit(const char *file, const char *src)
{
    struct parser ps;
    struct unit *u = xcalloc(1, sizeof *u);
    u->file = file;

    lex_init(&ps.lx, file, src);
    struct func **tail = &u->funcs;
    while (cur(&ps)->kind != TOK_EOF) {
        *tail = parse_func(&ps);
        tail = &(*tail)->next;
    }
    if (!u->funcs)
        diag_fatal(file, 0, "no functions in file");
    return u;
}
