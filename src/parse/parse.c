#include "parse.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../lex/lex.h"
#include "../sema/type.h"

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

/* C keywords the subset does not implement lex as identifiers; naming
 * them here turns "'struct' is not declared" into an honest "not
 * supported yet". Grows emptier as M2 proceeds. */
static const char *const reserved_unsupported[] = {
    "auto", "case", "const", "default", "do", "double", "enum", "extern",
    "float", "goto", "register", "struct", "switch", "typedef", "union",
    "volatile",
};

static void reject_reserved(struct parser *ps, const char *name, int line)
{
    for (size_t i = 0;
         i < sizeof reserved_unsupported / sizeof reserved_unsupported[0];
         i++)
        if (strcmp(reserved_unsupported[i], name) == 0)
            diag_fatal(ps->lx.file, line,
                       "'%s' is not supported yet (see docs/ROADMAP.md M2)",
                       name);
}

/* ---- types ---- */

static int tok_is_type_start(enum tok_kind k)
{
    return k == TOK_KW_INT || k == TOK_KW_CHAR || k == TOK_KW_SHORT ||
           k == TOK_KW_LONG || k == TOK_KW_UNSIGNED ||
           k == TOK_KW_SIGNED || k == TOK_KW_VOID;
}

/* Consumes a type specifier if one starts here, else returns NULL with
 * nothing consumed. "unsigned"/"signed" alone mean int, as in C. */
static struct type *parse_type_spec(struct parser *ps)
{
    int uns = -1;
    enum ty_kind kind = TY_INT;

    if (cur(ps)->kind == TOK_KW_UNSIGNED) {
        uns = 1;
        advance(ps);
    } else if (cur(ps)->kind == TOK_KW_SIGNED) {
        uns = 0;
        advance(ps);
    }
    switch (cur(ps)->kind) {
    case TOK_KW_CHAR:
        kind = TY_CHAR;
        advance(ps);
        break;
    case TOK_KW_SHORT:
        kind = TY_SHORT;
        advance(ps);
        if (cur(ps)->kind == TOK_KW_INT)
            advance(ps);
        break;
    case TOK_KW_INT:
        kind = TY_INT;
        advance(ps);
        break;
    case TOK_KW_LONG:
        kind = TY_LONG;
        advance(ps);
        if (cur(ps)->kind == TOK_KW_LONG) /* long long == long in LP64 */
            advance(ps);
        if (cur(ps)->kind == TOK_KW_INT)
            advance(ps);
        break;
    case TOK_KW_VOID:
        if (uns != -1)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "'void' cannot be signed or unsigned");
        kind = TY_VOID;
        advance(ps);
        break;
    default:
        if (uns == -1)
            return NULL; /* not a type; nothing consumed */
        break; /* bare unsigned/signed -> int */
    }
    return ty_base(kind, uns == 1);
}

static struct type *parse_stars(struct parser *ps, struct type *t)
{
    while (cur(ps)->kind == TOK_STAR) {
        t = ty_ptr(t);
        advance(ps);
    }
    return t;
}

/* ---- expressions ---- */

static struct expr *new_expr(enum expr_kind kind, int line)
{
    struct expr *e = xcalloc(1, sizeof *e);
    e->kind = kind;
    e->line = line;
    return e;
}

static struct expr *parse_expr(struct parser *ps);
static struct expr *parse_unary(struct parser *ps);
static struct expr *binop(enum binop op, struct expr *lhs,
                          struct expr *rhs);

static struct expr *parse_primary(struct parser *ps)
{
    struct token *t = cur(ps);
    struct expr *e;

    switch (t->kind) {
    case TOK_NUM:
        e = new_expr(EXPR_NUM, t->line);
        e->num = t->num;
        e->ty = ty_base(t->num_long ? TY_LONG : TY_INT, t->num_uns);
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
        reject_reserved(ps, name, line);
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
                               "(register args only for now)",
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
    default:
        diag_fatal(ps->lx.file, t->line, "expected an expression, got %s",
                   tok_describe(t));
        return NULL;
    }
}

static struct expr *incdec(struct parser *ps, struct expr *target,
                           int line, int is_post, int delta)
{
    if (target->kind != EXPR_VAR)
        diag_fatal(ps->lx.file, line,
                   "++/-- needs a plain variable (not through a pointer "
                   "yet)");
    struct expr *e = new_expr(EXPR_INCDEC, line);
    e->name = target->name;
    e->is_post = is_post;
    e->delta = delta;
    return e;
}

static struct expr *parse_postfix(struct parser *ps)
{
    struct expr *e = parse_primary(ps);
    for (;;) {
        if (cur(ps)->kind == TOK_LBRACKET) {
            /* p[i] is sugar for *(p + i); the scaling by the pointee
             * size happens in irgen off the types. */
            int line = cur(ps)->line;
            advance(ps);
            struct expr *idx = parse_expr(ps);
            expect(ps, TOK_RBRACKET, "']'");
            struct expr *d = new_expr(EXPR_DEREF, line);
            d->rhs = binop(B_ADD, e, idx);
            e = d;
        } else if (cur(ps)->kind == TOK_PLUSPLUS ||
                   cur(ps)->kind == TOK_MINUSMINUS) {
            int delta = cur(ps)->kind == TOK_PLUSPLUS ? 1 : -1;
            int line = cur(ps)->line;
            advance(ps);
            e = incdec(ps, e, line, 1, delta);
        } else {
            return e;
        }
    }
}

static struct expr *parse_unary(struct parser *ps)
{
    struct token *t = cur(ps);
    struct expr *e;

    switch (t->kind) {
    case TOK_BANG:
        e = new_expr(EXPR_NOT, t->line);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_MINUS:
        e = new_expr(EXPR_NEG, t->line);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_TILDE:
        e = new_expr(EXPR_BNOT, t->line);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_STAR:
        e = new_expr(EXPR_DEREF, t->line);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_AMP:
        e = new_expr(EXPR_ADDR, t->line);
        advance(ps);
        e->rhs = parse_unary(ps);
        return e;
    case TOK_PLUSPLUS:
    case TOK_MINUSMINUS: {
        int delta = t->kind == TOK_PLUSPLUS ? 1 : -1;
        int line = t->line;
        advance(ps);
        return incdec(ps, parse_unary(ps), line, 0, delta);
    }
    case TOK_KW_SIZEOF: {
        int line = t->line;
        advance(ps);
        e = new_expr(EXPR_SIZEOF, line);
        if (cur(ps)->kind == TOK_LPAREN) {
            struct lexer save = ps->lx;
            advance(ps);
            if (tok_is_type_start(cur(ps)->kind)) {
                e->cast_ty = parse_stars(ps, parse_type_spec(ps));
                expect(ps, TOK_RPAREN, "')'");
                return e;
            }
            ps->lx = save; /* sizeof (expr) */
        }
        e->rhs = parse_unary(ps);
        return e;
    }
    case TOK_LPAREN: {
        /* a cast, or a parenthesized expression — peek one token */
        struct lexer save = ps->lx;
        advance(ps);
        if (tok_is_type_start(cur(ps)->kind)) {
            struct type *ct = parse_stars(ps, parse_type_spec(ps));
            expect(ps, TOK_RPAREN, "')'");
            e = new_expr(EXPR_CAST, t->line);
            e->cast_ty = ct;
            e->rhs = parse_unary(ps);
            return e;
        }
        ps->lx = save;
        return parse_postfix(ps);
    }
    default:
        return parse_postfix(ps);
    }
}

static struct expr *binop(enum binop op, struct expr *lhs, struct expr *rhs)
{
    struct expr *e = new_expr(EXPR_BINOP, lhs->line);
    e->op = op;
    e->lhs = lhs;
    e->rhs = rhs;
    return e;
}

/* One binary precedence level: while the current token maps to an op in
 * the table, consume it and parse the next-tighter level. */
struct oplevel {
    enum tok_kind tok;
    enum binop op;
};

static struct expr *parse_level(struct parser *ps,
                                const struct oplevel *ops, int nops,
                                struct expr *(*tighter)(struct parser *))
{
    struct expr *e = tighter(ps);
    for (;;) {
        int i;
        for (i = 0; i < nops; i++)
            if (cur(ps)->kind == ops[i].tok)
                break;
        if (i == nops)
            return e;
        advance(ps);
        e = binop(ops[i].op, e, tighter(ps));
    }
}

#define LEVEL(name, tighter, ...)                                        \
    static struct expr *name(struct parser *ps)                          \
    {                                                                    \
        static const struct oplevel ops[] = { __VA_ARGS__ };             \
        return parse_level(ps, ops,                                      \
                           (int)(sizeof ops / sizeof ops[0]), tighter);  \
    }

/* C's precedence ladder, loosest at the bottom. */
LEVEL(parse_mul, parse_unary, { TOK_STAR, B_MUL }, { TOK_SLASH, B_DIV },
      { TOK_PERCENT, B_MOD })
LEVEL(parse_add, parse_mul, { TOK_PLUS, B_ADD }, { TOK_MINUS, B_SUB })
LEVEL(parse_shift, parse_add, { TOK_SHL, B_SHL }, { TOK_SHR, B_SHR })
LEVEL(parse_rel, parse_shift, { TOK_LT, B_LT }, { TOK_LE, B_LE },
      { TOK_GT, B_GT }, { TOK_GE, B_GE })
LEVEL(parse_eq, parse_rel, { TOK_EQEQ, B_EQ }, { TOK_NEQ, B_NE })
LEVEL(parse_band, parse_eq, { TOK_AMP, B_AND })
LEVEL(parse_bxor, parse_band, { TOK_CARET, B_XOR })
LEVEL(parse_bor, parse_bxor, { TOK_PIPE, B_OR })
LEVEL(parse_land, parse_bor, { TOK_ANDAND, B_LAND })
LEVEL(parse_lor, parse_land, { TOK_OROR, B_LOR })

/* Assignment is right-associative; the target must be a variable or a
 * dereference. Compound forms desugar to 'a = a op (b)' and stay
 * variable-only: through a pointer the desugaring would evaluate the
 * address twice, which is observable once addresses have side effects. */
static const struct {
    enum tok_kind tok;
    enum binop op;
} compound_assign[] = {
    { TOK_PLUSEQ, B_ADD },   { TOK_MINUSEQ, B_SUB },
    { TOK_STAREQ, B_MUL },   { TOK_SLASHEQ, B_DIV },
    { TOK_PERCENTEQ, B_MOD },{ TOK_AMPEQ, B_AND },
    { TOK_PIPEEQ, B_OR },    { TOK_CARETEQ, B_XOR },
    { TOK_SHLEQ, B_SHL },    { TOK_SHREQ, B_SHR },
};

static struct expr *parse_expr(struct parser *ps)
{
    struct expr *e = parse_lor(ps);
    enum tok_kind k = cur(ps)->kind;
    int line = cur(ps)->line;

    int comp = -1;
    for (size_t i = 0;
         i < sizeof compound_assign / sizeof compound_assign[0]; i++)
        if (k == compound_assign[i].tok)
            comp = (int)i;

    if (k != TOK_ASSIGN && comp < 0)
        return e;
    if (e->kind != EXPR_VAR && e->kind != EXPR_DEREF)
        diag_fatal(ps->lx.file, line,
                   "assignment target must be a variable or *pointer");
    if (comp >= 0 && e->kind != EXPR_VAR)
        diag_fatal(ps->lx.file, line,
                   "compound assignment through a pointer is not "
                   "supported yet; write it out as *p = *p op x");
    advance(ps);

    struct expr *a = new_expr(EXPR_ASSIGN, line);
    a->lhs = e;
    if (comp < 0) {
        a->rhs = parse_expr(ps);
    } else {
        struct expr *lhs_copy = new_expr(EXPR_VAR, line);
        lhs_copy->name = e->name;
        a->rhs = binop(compound_assign[comp].op, lhs_copy, parse_expr(ps));
    }
    return a;
}

/* ---- statements ---- */

static struct stmt *new_stmt(enum stmt_kind kind, int line)
{
    struct stmt *s = xcalloc(1, sizeof *s);
    s->kind = kind;
    s->line = line;
    return s;
}

static struct stmt *parse_stmt(struct parser *ps, int allow_decl);

/* The statement controlled by if/while/for: C99 does not allow a bare
 * declaration there, and neither do we — that keeps the subset strict. */
static struct stmt *parse_controlled(struct parser *ps)
{
    return parse_stmt(ps, 0);
}

static struct stmt *parse_block(struct parser *ps)
{
    struct stmt *s = new_stmt(STMT_BLOCK, cur(ps)->line);
    expect(ps, TOK_LBRACE, "'{'");
    struct stmt **tail = &s->body;
    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind == TOK_EOF)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "unexpected end of file inside a block");
        *tail = parse_stmt(ps, 1);
        tail = &(*tail)->next;
    }
    advance(ps); /* '}' */
    return s;
}

static struct stmt *parse_stmt(struct parser *ps, int allow_decl)
{
    struct token *t = cur(ps);
    struct stmt *s;

    if (tok_is_type_start(t->kind)) {
        if (!allow_decl)
            diag_fatal(ps->lx.file, t->line,
                       "a declaration cannot be the body of if/while/for "
                       "(C99 forbids it too); wrap it in braces");
        s = new_stmt(STMT_DECL, t->line);
        struct type *base = parse_type_spec(ps);
        s->dty = parse_stars(ps, base);
        if (s->dty->kind == TY_VOID)
            diag_fatal(ps->lx.file, t->line,
                       "a variable cannot have type void");
        if (cur(ps)->kind != TOK_IDENT)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a variable name before %s",
                       tok_describe(cur(ps)));
        s->name = cur(ps)->text;
        advance(ps);
        if (cur(ps)->kind == TOK_ASSIGN) {
            advance(ps);
            s->expr = parse_expr(ps);
        }
        expect(ps, TOK_SEMI, "';'");
        return s;
    }

    switch (t->kind) {
    case TOK_LBRACE:
        return parse_block(ps);
    case TOK_KW_RETURN:
        s = new_stmt(STMT_RETURN, t->line);
        advance(ps);
        if (cur(ps)->kind != TOK_SEMI)
            s->expr = parse_expr(ps); /* NULL = bare return (void) */
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_IF:
        s = new_stmt(STMT_IF, t->line);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_expr(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->thn = parse_controlled(ps);
        if (cur(ps)->kind == TOK_KW_ELSE) {
            advance(ps);
            s->els = parse_controlled(ps);
        }
        return s;
    case TOK_KW_WHILE:
        s = new_stmt(STMT_WHILE, t->line);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_expr(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_FOR:
        s = new_stmt(STMT_FOR, t->line);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        if (tok_is_type_start(cur(ps)->kind))
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "declarations in for-init are not supported yet; "
                       "declare the variable before the loop");
        if (cur(ps)->kind != TOK_SEMI)
            s->init = parse_expr(ps);
        expect(ps, TOK_SEMI, "';'");
        if (cur(ps)->kind != TOK_SEMI) /* NULL cond = forever; break exits */
            s->cond = parse_expr(ps);
        expect(ps, TOK_SEMI, "';'");
        if (cur(ps)->kind != TOK_RPAREN)
            s->step = parse_expr(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_BREAK:
        s = new_stmt(STMT_BREAK, t->line);
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_CONTINUE:
        s = new_stmt(STMT_CONTINUE, t->line);
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_IDENT:
    case TOK_NUM:
    case TOK_LPAREN:
    case TOK_BANG:
    case TOK_MINUS:
    case TOK_TILDE:
    case TOK_STAR:  /* *p = ...; */
    case TOK_AMP:
    case TOK_PLUSPLUS:
    case TOK_MINUSMINUS:
        /* expression statement: assignment, call, or ++/-- */
        if (t->kind == TOK_IDENT)
            reject_reserved(ps, t->text, t->line);
        s = new_stmt(STMT_EXPR, t->line);
        s->expr = parse_expr(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
    default:
        diag_fatal(ps->lx.file, t->line,
                   "expected a statement, got %s", tok_describe(t));
        return NULL;
    }
}

/* ---- functions ---- */

static struct func *parse_func(struct parser *ps)
{
    struct func *f = xcalloc(1, sizeof *f);

    if (cur(ps)->kind == TOK_KW_STATIC) {
        f->is_static = 1;
        advance(ps);
    }
    if (cur(ps)->kind == TOK_IDENT)
        reject_reserved(ps, cur(ps)->text, cur(ps)->line);
    struct type *rt = parse_type_spec(ps);
    if (!rt)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected a return type before %s",
                   tok_describe(cur(ps)));
    f->ret_ty = parse_stars(ps, rt);
    if (cur(ps)->kind != TOK_IDENT)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected a function name before %s",
                   tok_describe(cur(ps)));
    f->name = cur(ps)->text;
    f->line = cur(ps)->line;
    advance(ps);
    expect(ps, TOK_LPAREN, "'('");

    if (cur(ps)->kind == TOK_KW_VOID && ps->lx.tok.kind == TOK_KW_VOID) {
        /* "(void)" means no parameters; "(void *x)" is a parameter */
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind == TOK_RPAREN) {
            /* fall through to the closing paren below */
        } else {
            ps->lx = save;
        }
    }
    if (cur(ps)->kind != TOK_RPAREN) {
        for (;;) {
            if (cur(ps)->kind == TOK_IDENT)
                reject_reserved(ps, cur(ps)->text, cur(ps)->line);
            struct type *pt = parse_type_spec(ps);
            if (!pt)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a parameter type before %s",
                           tok_describe(cur(ps)));
            pt = parse_stars(ps, pt);
            if (pt->kind == TY_VOID)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "a parameter cannot have type void");
            if (f->nparams >= MAX_PARAMS)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "more than %d parameters "
                           "(register args only for now)", MAX_PARAMS);
            f->param_tys[f->nparams] = pt;
            /* The name is optional in a prototype; a definition with a
             * nameless parameter is rejected below. */
            if (cur(ps)->kind == TOK_IDENT) {
                f->params[f->nparams++] = cur(ps)->text;
                advance(ps);
            } else {
                f->params[f->nparams++] = NULL;
            }
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')'");

    if (cur(ps)->kind == TOK_SEMI) {
        advance(ps);
        return f; /* prototype */
    }
    if (cur(ps)->kind != TOK_LBRACE)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected '{' or ';' before %s", tok_describe(cur(ps)));
    for (int i = 0; i < f->nparams; i++)
        if (!f->params[i])
            diag_fatal(ps->lx.file, f->line,
                       "parameter %d of '%s' needs a name in a "
                       "definition", i + 1, f->name);
    struct stmt *blk = parse_block(ps);
    f->body = blk->body;
    f->defined = 1;
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
