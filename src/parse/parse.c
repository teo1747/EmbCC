#include "parse.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../lex/lex.h"
#include "../sema/type.h"

/* Tags (struct/union/enum) live in their own namespace; typedef names
 * live in the ordinary one and must be known DURING parsing (the
 * classic C ambiguity), so both tables belong to the parser. */
enum tag_kind { TAG_STRUCT, TAG_UNION, TAG_ENUM };

struct tagdef {
    const char *tag;
    enum tag_kind kind;
    struct type *ty;      /* struct/union node; NULL for enums */
    struct tagdef *next;
};

struct typedefent {
    const char *name;
    struct type *ty;
    struct typedefent *next;
};

struct parser {
    struct lexer lx;
    struct unit *unit;
    struct tagdef *tags;
    struct typedefent *typedefs;
    struct econst **econst_tail;
    int seq;              /* current top-level item, for econst seq */
};

static struct token *cur(struct parser *ps) { return &ps->lx.tok; }
static void advance(struct parser *ps) { lex_next(&ps->lx); }

static struct tagdef *find_tag(struct parser *ps, const char *tag)
{
    for (struct tagdef *t = ps->tags; t; t = t->next)
        if (strcmp(t->tag, tag) == 0)
            return t;
    return NULL;
}

static struct type *find_typedef(struct parser *ps, const char *name)
{
    for (struct typedefent *t = ps->typedefs; t; t = t->next)
        if (strcmp(t->name, name) == 0)
            return t->ty;
    return NULL;
}

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
    "auto", "case", "const", "default", "do", "double",
    "float", "goto", "register", "switch", "volatile",
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
           k == TOK_KW_SIGNED || k == TOK_KW_VOID ||
           k == TOK_KW_STRUCT || k == TOK_KW_UNION || k == TOK_KW_ENUM;
}

/* Does a type begin at the current token — including typedef names,
 * which only the parser's table can decide. */
static int at_type_start(struct parser *ps)
{
    if (tok_is_type_start(cur(ps)->kind))
        return 1;
    return cur(ps)->kind == TOK_IDENT &&
           find_typedef(ps, cur(ps)->text) != NULL;
}

static struct type *parse_struct_body(struct parser *ps, struct type *t);
static void parse_enum_body(struct parser *ps);

/* struct/union/enum specifier, after the keyword was consumed. */
static struct type *parse_tagged(struct parser *ps, enum tag_kind kind,
                                 int allow_body, int line)
{
    const char *tag = NULL;
    if (cur(ps)->kind == TOK_IDENT) {
        tag = cur(ps)->text;
        advance(ps);
    }

    if (cur(ps)->kind == TOK_LBRACE) {
        if (!allow_body)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "define %s at file scope (block-scope type "
                       "definitions are not supported)",
                       kind == TAG_ENUM ? "enums" : "structs/unions");
        struct type *t = NULL;
        if (tag) {
            struct tagdef *td = find_tag(ps, tag);
            if (td) {
                if (td->kind != kind)
                    diag_fatal(ps->lx.file, line,
                               "'%s' is a different kind of tag", tag);
                if (kind != TAG_ENUM && td->ty->complete)
                    diag_fatal(ps->lx.file, line,
                               "redefinition of '%s'", tag);
                t = td->ty;
            } else {
                td = xcalloc(1, sizeof *td);
                td->tag = tag;
                td->kind = kind;
                if (kind != TAG_ENUM)
                    td->ty = ty_struct(tag, kind == TAG_UNION);
                td->next = ps->tags;
                ps->tags = td;
                t = td->ty;
            }
        } else if (kind != TAG_ENUM) {
            t = ty_struct(NULL, kind == TAG_UNION);
        }
        if (kind == TAG_ENUM) {
            parse_enum_body(ps);
            return ty_base(TY_INT, 0);
        }
        return parse_struct_body(ps, t);
    }

    if (!tag)
        diag_fatal(ps->lx.file, line, "%s needs a tag or a body",
                   kind == TAG_ENUM ? "enum" :
                   kind == TAG_UNION ? "union" : "struct");
    struct tagdef *td = find_tag(ps, tag);
    if (td) {
        if (td->kind != kind)
            diag_fatal(ps->lx.file, line,
                       "'%s' is a different kind of tag", tag);
        return kind == TAG_ENUM ? ty_base(TY_INT, 0) : td->ty;
    }
    if (kind == TAG_ENUM)
        diag_fatal(ps->lx.file, line, "unknown enum '%s'", tag);
    /* Forward reference: an incomplete struct, fine behind a pointer */
    td = xcalloc(1, sizeof *td);
    td->tag = tag;
    td->kind = kind;
    td->ty = ty_struct(tag, kind == TAG_UNION);
    td->next = ps->tags;
    ps->tags = td;
    return td->ty;
}

/* Consumes a type specifier if one starts here, else returns NULL with
 * nothing consumed. "unsigned"/"signed" alone mean int, as in C.
 * allow_body: may a struct/union/enum BODY appear here (file scope). */
static struct type *parse_type_spec(struct parser *ps, int allow_body)
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
    if (cur(ps)->kind == TOK_KW_STRUCT || cur(ps)->kind == TOK_KW_UNION ||
        cur(ps)->kind == TOK_KW_ENUM) {
        if (uns != -1)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "tagged types cannot be signed or unsigned");
        enum tag_kind k = cur(ps)->kind == TOK_KW_STRUCT ? TAG_STRUCT :
                          cur(ps)->kind == TOK_KW_UNION ? TAG_UNION :
                          TAG_ENUM;
        int line = cur(ps)->line;
        advance(ps);
        return parse_tagged(ps, k, allow_body, line);
    }
    if (uns == -1 && cur(ps)->kind == TOK_IDENT) {
        struct type *td = find_typedef(ps, cur(ps)->text);
        if (!td)
            return NULL;
        advance(ps);
        return td;
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

/* Shared by locals, globals, and members: trailing [N]([M]...) turns t
 * into (nested) array types. Sizes are positive integer literals. */
static struct type *parse_array_dims(struct parser *ps, struct type *t)
{
    int dims[4];
    int ndims = 0;
    while (cur(ps)->kind == TOK_LBRACKET) {
        advance(ps);
        if (cur(ps)->kind != TOK_NUM || cur(ps)->num <= 0)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "array size must be a positive integer literal");
        if (ndims >= 4)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "more than 4 array dimensions");
        dims[ndims++] = (int)cur(ps)->num;
        advance(ps);
        expect(ps, TOK_RBRACKET, "']'");
    }
    for (int i = ndims - 1; i >= 0; i--)
        t = ty_array(t, dims[i]);
    return t;
}

static struct type *parse_struct_body(struct parser *ps, struct type *t)
{
    expect(ps, TOK_LBRACE, "'{'");
    struct member *ms = NULL;
    int n = 0, cap = 0;

    while (cur(ps)->kind != TOK_RBRACE) {
        struct type *spec = parse_type_spec(ps, 0);
        if (!spec)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a member type before %s",
                       tok_describe(cur(ps)));
        struct type *mty = parse_stars(ps, spec);
        if (mty->kind == TY_VOID)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "a member cannot have type void");
        if (cur(ps)->kind != TOK_IDENT)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a member name before %s",
                       tok_describe(cur(ps)));
        const char *mname = cur(ps)->text;
        int mline = cur(ps)->line;
        advance(ps);
        mty = parse_array_dims(ps, mty);
        if (ty_size(mty) == 0)
            diag_fatal(ps->lx.file, mline,
                       "member '%s' has incomplete type %s",
                       mname, ty_name(mty));
        for (int i = 0; i < n; i++)
            if (strcmp(ms[i].name, mname) == 0)
                diag_fatal(ps->lx.file, mline,
                           "duplicate member '%s'", mname);
        expect(ps, TOK_SEMI, "';'");
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            ms = xrealloc(ms, (size_t)cap * sizeof *ms);
        }
        ms[n].name = mname;
        ms[n].ty = mty;
        ms[n].off = 0;
        n++;
    }
    advance(ps); /* '}' */
    if (n == 0)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "a struct/union needs at least one member");
    ty_struct_layout(t, ms, n);
    return t;
}

static void parse_enum_body(struct parser *ps)
{
    expect(ps, TOK_LBRACE, "'{'");
    long val = 0;

    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind != TOK_IDENT)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected an enumerator name before %s",
                       tok_describe(cur(ps)));
        const char *name = cur(ps)->text;
        int line = cur(ps)->line;
        advance(ps);
        if (cur(ps)->kind == TOK_ASSIGN) {
            advance(ps);
            int neg = 0;
            if (cur(ps)->kind == TOK_MINUS) {
                neg = 1;
                advance(ps);
            }
            if (cur(ps)->kind != TOK_NUM)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "an enumerator value must be an integer "
                           "literal for now");
            val = neg ? -cur(ps)->num : cur(ps)->num;
            advance(ps);
        }
        for (struct econst *ec = ps->unit->econsts; ec; ec = ec->next)
            if (strcmp(ec->name, name) == 0)
                diag_fatal(ps->lx.file, line,
                           "duplicate enumerator '%s'", name);
        struct econst *ec = xcalloc(1, sizeof *ec);
        ec->name = name;
        ec->val = val++;
        ec->seq = ps->seq;
        *ps->econst_tail = ec;
        ps->econst_tail = &ec->next;
        if (cur(ps)->kind != TOK_COMMA)
            break;
        advance(ps); /* trailing comma before '}' is fine, as in C99 */
    }
    expect(ps, TOK_RBRACE, "'}'");
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
    case TOK_STR:
        e = new_expr(EXPR_STR, t->line);
        e->name = t->text;
        e->num = t->num;
        advance(ps);
        if (cur(ps)->kind == TOK_STR)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "adjacent string literal concatenation is not "
                       "supported yet");
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
        } else if (cur(ps)->kind == TOK_DOT ||
                   cur(ps)->kind == TOK_ARROW) {
            int is_arrow = cur(ps)->kind == TOK_ARROW;
            int line = cur(ps)->line;
            advance(ps);
            if (cur(ps)->kind != TOK_IDENT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a member name before %s",
                           tok_describe(cur(ps)));
            struct expr *m = new_expr(EXPR_MEMBER, line);
            m->lhs = e;
            m->name = cur(ps)->text;
            m->is_arrow = is_arrow;
            advance(ps);
            e = m;
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
            if (at_type_start(ps)) {
                e->cast_ty = parse_stars(ps, parse_type_spec(ps, 0));
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
        if (at_type_start(ps)) {
            struct type *ct = parse_stars(ps, parse_type_spec(ps, 0));
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
    if (e->kind != EXPR_VAR && e->kind != EXPR_DEREF &&
        e->kind != EXPR_MEMBER)
        diag_fatal(ps->lx.file, line,
                   "assignment target must be a variable, *pointer, or "
                   "member");
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

    if (at_type_start(ps)) {
        if (!allow_decl)
            diag_fatal(ps->lx.file, t->line,
                       "a declaration cannot be the body of if/while/for "
                       "(C99 forbids it too); wrap it in braces");
        s = new_stmt(STMT_DECL, t->line);
        struct type *base = parse_type_spec(ps, 0);
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
        int was_array = cur(ps)->kind == TOK_LBRACKET;
        s->dty = parse_array_dims(ps, s->dty);
        if (ty_size(s->dty) == 0)
            diag_fatal(ps->lx.file, s->line,
                       "'%s' has incomplete type %s", s->name,
                       ty_name(s->dty));
        if (was_array && cur(ps)->kind == TOK_ASSIGN)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "array initializers are not supported yet");
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
        if (at_type_start(ps))
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

/* ---- top level: functions and globals ---- */

/* A file-scope variable, after the declarator name has been consumed. */
static struct global *parse_global(struct parser *ps, struct type *ty,
                                   const char *name, int line,
                                   int is_static, int is_extern)
{
    struct global *g = xcalloc(1, sizeof *g);
    g->name = name;
    g->line = line;
    g->is_static = is_static;
    g->is_extern = is_extern;
    g->ty = ty;

    if (g->ty->kind == TY_VOID)
        diag_fatal(ps->lx.file, line, "a variable cannot have type void");
    g->ty = parse_array_dims(ps, g->ty);
    if (ty_size(g->ty) == 0)
        diag_fatal(ps->lx.file, line,
                   "'%s' has incomplete type %s", name, ty_name(g->ty));
    if (cur(ps)->kind == TOK_ASSIGN) {
        advance(ps);
        if (is_extern)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "'extern' with an initializer");
        if (g->ty->kind == TY_ARRAY)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "array initializers are not supported yet");
        /* Constant initializers only: a literal, optionally negated —
         * constant folding arrives with the preprocessor era. */
        int neg = 0;
        if (cur(ps)->kind == TOK_MINUS) {
            neg = 1;
            advance(ps);
        }
        if (cur(ps)->kind != TOK_NUM)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "a global initializer must be an integer literal "
                       "for now");
        g->has_init = 1;
        g->init = neg ? -cur(ps)->num : cur(ps)->num;
        advance(ps);
    }
    if (cur(ps)->kind == TOK_COMMA)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "one declarator per declaration, please");
    expect(ps, TOK_SEMI, "';'");
    return g;
}

/* Parses one top-level item into the unit: a function (prototype or
 * definition) or a global variable. */
static void parse_top(struct parser *ps, struct unit *u,
                      struct func ***ftail, struct global ***gtail,
                      int seq)
{
    int is_static = 0, is_extern = 0;
    ps->seq = seq;

    if (cur(ps)->kind == TOK_KW_STATIC) {
        is_static = 1;
        advance(ps);
    } else if (cur(ps)->kind == TOK_KW_EXTERN) {
        is_extern = 1;
        advance(ps);
    }
    if (cur(ps)->kind == TOK_KW_TYPEDEF) {
        if (is_static || is_extern)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "typedef cannot be static or extern");
        advance(ps);
        struct type *tbase = parse_type_spec(ps, 1);
        if (!tbase)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a type after 'typedef'");
        struct type *tt = parse_stars(ps, tbase);
        if (cur(ps)->kind != TOK_IDENT)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "typedef needs a name, got %s",
                       tok_describe(cur(ps)));
        const char *tname = cur(ps)->text;
        if (find_typedef(ps, tname))
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "redefinition of typedef '%s'", tname);
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        struct typedefent *te = xcalloc(1, sizeof *te);
        te->name = tname;
        te->ty = tt;
        te->next = ps->typedefs;
        ps->typedefs = te;
        return;
    }
    if (cur(ps)->kind == TOK_IDENT)
        reject_reserved(ps, cur(ps)->text, cur(ps)->line);
    struct type *base = parse_type_spec(ps, 1);
    if (!base)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected a type before %s", tok_describe(cur(ps)));
    if (cur(ps)->kind == TOK_SEMI) {
        /* bare declaration: 'struct X { ... };', 'enum { ... };' */
        advance(ps);
        return;
    }
    struct type *ty = parse_stars(ps, base);
    if (cur(ps)->kind != TOK_IDENT)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "expected a name before %s", tok_describe(cur(ps)));
    const char *name = cur(ps)->text;
    int line = cur(ps)->line;
    advance(ps);

    if (cur(ps)->kind != TOK_LPAREN) {
        struct global *g = parse_global(ps, ty, name, line,
                                        is_static, is_extern);
        g->seq = seq;
        **gtail = g;
        *gtail = &g->next;
        (void)u;
        return;
    }

    struct func *f = xcalloc(1, sizeof *f);
    /* 'extern' on a function is the default linkage — accept, ignore */
    f->is_static = is_static;
    if (ty->kind == TY_STRUCT)
        diag_fatal(ps->lx.file, line,
                   "returning a struct/union by value is not supported "
                   "yet (SysV classification) — return a pointer");
    f->ret_ty = ty;
    f->name = name;
    f->line = line;
    f->seq = seq;
    advance(ps); /* '(' */

    if (cur(ps)->kind == TOK_KW_VOID) {
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
            if (cur(ps)->kind == TOK_ELLIPSIS) {
                if (f->nparams == 0)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "'...' needs at least one named "
                               "parameter before it");
                f->is_varargs = 1;
                advance(ps);
                break;
            }
            if (cur(ps)->kind == TOK_IDENT)
                reject_reserved(ps, cur(ps)->text, cur(ps)->line);
            struct type *pt = parse_type_spec(ps, 0);
            if (!pt)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a parameter type before %s",
                           tok_describe(cur(ps)));
            pt = parse_stars(ps, pt);
            if (pt->kind == TY_VOID)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "a parameter cannot have type void");
            if (pt->kind == TY_STRUCT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "struct/union parameters by value are not "
                           "supported yet (SysV classification) — "
                           "pass a pointer");
            if (f->nparams >= MAX_PARAMS)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "more than %d parameters "
                           "(register args only for now)", MAX_PARAMS);
            f->param_tys[f->nparams] = pt;
            /* The name is optional in a prototype; a definition with a
             * nameless parameter is rejected below. */
            if (cur(ps)->kind == TOK_IDENT) {
                f->params[f->nparams] = cur(ps)->text;
                advance(ps);
            } else {
                f->params[f->nparams] = NULL;
            }
            /* C adjusts an array parameter to a pointer to its element;
             * the size, if given, is documentation. */
            if (cur(ps)->kind == TOK_LBRACKET) {
                advance(ps);
                if (cur(ps)->kind == TOK_NUM)
                    advance(ps);
                expect(ps, TOK_RBRACKET, "']'");
                f->param_tys[f->nparams] =
                    ty_ptr(f->param_tys[f->nparams]);
            }
            f->nparams++;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')'");

    if (cur(ps)->kind == TOK_SEMI) {
        advance(ps); /* prototype */
    } else {
        if (cur(ps)->kind != TOK_LBRACE)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected '{' or ';' before %s",
                       tok_describe(cur(ps)));
        if (f->is_varargs)
            diag_fatal(ps->lx.file, f->line,
                       "defining a variadic function is not supported "
                       "yet (no va_list); only calls to external "
                       "variadic functions work");
        for (int i = 0; i < f->nparams; i++)
            if (!f->params[i])
                diag_fatal(ps->lx.file, f->line,
                           "parameter %d of '%s' needs a name in a "
                           "definition", i + 1, f->name);
        struct stmt *blk = parse_block(ps);
        f->body = blk->body;
        f->defined = 1;
    }
    **ftail = f;
    *ftail = &f->next;
}

struct unit *parse_unit(const char *file, const char *src)
{
    struct parser ps;
    struct unit *u = xcalloc(1, sizeof *u);
    u->file = file;

    ps.unit = u;
    ps.tags = NULL;
    ps.typedefs = NULL;
    ps.econst_tail = &u->econsts;
    ps.seq = 0;
    lex_init(&ps.lx, file, src);
    struct func **ftail = &u->funcs;
    struct global **gtail = &u->globals;
    int seq = 0;
    while (cur(&ps)->kind != TOK_EOF)
        parse_top(&ps, u, &ftail, &gtail, seq++);
    if (!u->funcs)
        diag_fatal(file, 0, "no functions in file");
    return u;
}
