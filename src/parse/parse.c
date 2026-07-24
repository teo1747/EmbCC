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
    "auto", "goto", "register",
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
           k == TOK_KW_STRUCT || k == TOK_KW_UNION || k == TOK_KW_ENUM ||
           k == TOK_KW_CONST || k == TOK_KW_VOLATILE ||
           k == TOK_KW_FLOAT || k == TOK_KW_DOUBLE;
}

/* const/volatile/restrict are accepted and IGNORED: EmbCC does not
 * enforce const-correctness yet. Documented divergence — it accepts
 * programs gcc rejects, the price of parsing real headers pre-M3. */
static void skip_quals(struct parser *ps)
{
    while (cur(ps)->kind == TOK_KW_CONST ||
           cur(ps)->kind == TOK_KW_VOLATILE ||
           cur(ps)->kind == TOK_KW_RESTRICT)
        advance(ps);
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

static struct type *parse_fn_params(struct parser *ps, struct type *ret);
static struct type *parse_stars(struct parser *ps, struct type *t);
static struct expr *parse_cond(struct parser *ps);
static int size_fold(const struct expr *e, long *out);
static struct type *parse_array_dims(struct parser *ps, struct type *t);
static struct type *parse_type_spec(struct parser *ps, int allow_body);

/* Declarator over a base type: leading stars, then either the function-
 * pointer form '( * [*...] [name] [dims] ) ( params )' or a plain
 * [name] [dims]. name_out is NULL when no name appeared (legal in
 * prototypes and abstract declarators). */
static struct type *parse_declarator(struct parser *ps, struct type *base,
                                     const char **name_out)
{
    base = parse_stars(ps, base);
    *name_out = NULL;
    if (cur(ps)->kind == TOK_LPAREN) {
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind == TOK_STAR) {
            advance(ps);
            int extra = 0;
            while (cur(ps)->kind == TOK_STAR) {
                advance(ps);
                extra++;
            }
            skip_quals(ps);
            if (cur(ps)->kind == TOK_IDENT) {
                *name_out = cur(ps)->text;
                advance(ps);
            }
            int adims[4];
            int nad = 0;
            while (cur(ps)->kind == TOK_LBRACKET) {
                advance(ps);
                int dline = cur(ps)->line;
                struct expr *de = parse_cond(ps);
                long dv;
                if (!size_fold(de, &dv) || dv <= 0)
                    diag_fatal(ps->lx.file, dline,
                               "array size must be a positive constant "
                               "expression");
                if (nad >= 4)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "more than 4 array dimensions");
                adims[nad++] = (int)dv;
                expect(ps, TOK_RBRACKET, "']'");
            }
            expect(ps, TOK_RPAREN, "')'");
            struct type *t = ty_ptr(parse_fn_params(ps, base));
            for (int i = 0; i < extra; i++)
                t = ty_ptr(t);
            for (int i = nad - 1; i >= 0; i--)
                t = ty_array(t, adims[i]);
            return t;
        }
        ps->lx = save; /* not a function-pointer declarator */
    }
    if (cur(ps)->kind == TOK_IDENT) {
        *name_out = cur(ps)->text;
        advance(ps);
    }
    return parse_array_dims(ps, base);
}

/* The '(params)' of a function TYPE (as in a function pointer). */
static struct type *parse_fn_params(struct parser *ps, struct type *ret)
{
    expect(ps, TOK_LPAREN, "'('");
    struct type *pt[MAX_PARAMS];
    int n = 0, varargs = 0;

    if (cur(ps)->kind == TOK_KW_VOID) {
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind != TOK_RPAREN)
            ps->lx = save;
    }
    if (cur(ps)->kind != TOK_RPAREN) {
        for (;;) {
            if (cur(ps)->kind == TOK_ELLIPSIS) {
                varargs = 1;
                advance(ps);
                break;
            }
            struct type *spec = parse_type_spec(ps, 0);
            if (!spec)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a parameter type before %s",
                           tok_describe(cur(ps)));
            const char *dummy;
            struct type *t = parse_declarator(ps, spec, &dummy);
            if (t->kind == TY_ARRAY)
                t = ty_ptr(t->pointee); /* C's adjustment */
            if (t->kind == TY_FUNC)
                t = ty_ptr(t);
            if (t->kind == TY_VOID)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "a parameter cannot have type void");
            if (n >= MAX_PARAMS)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "too many parameters in a function type");
            pt[n++] = t;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')'");
    return ty_func(ret, pt, n, varargs);
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
    skip_quals(ps);
    /* struct/union/enum first (cannot mix with other specifiers) */
    if (cur(ps)->kind == TOK_KW_STRUCT || cur(ps)->kind == TOK_KW_UNION ||
        cur(ps)->kind == TOK_KW_ENUM) {
        enum tag_kind k = cur(ps)->kind == TOK_KW_STRUCT ? TAG_STRUCT :
                          cur(ps)->kind == TOK_KW_UNION ? TAG_UNION :
                          TAG_ENUM;
        int line = cur(ps)->line;
        advance(ps);
        return parse_tagged(ps, k, allow_body, line);
    }
    /* a typedef name, when no specifier has appeared */
    if (cur(ps)->kind == TOK_IDENT) {
        struct type *td = find_typedef(ps, cur(ps)->text);
        if (!td)
            return NULL;
        advance(ps);
        return td;
    }
    /* base specifiers in any order: unsigned long int, long unsigned... */
    int uns = -1, nlong = 0, nshort = 0, nchar = 0, nint = 0, nvoid = 0;
    int nfloat = 0, ndouble = 0;
    int any = 0;
    for (;;) {
        enum tok_kind k = cur(ps)->kind;
        if (k == TOK_KW_FLOAT) nfloat++;
        else if (k == TOK_KW_DOUBLE) ndouble++;
        else if (k == TOK_KW_UNSIGNED) uns = 1;
        else if (k == TOK_KW_SIGNED) uns = 0;
        else if (k == TOK_KW_LONG) nlong++;
        else if (k == TOK_KW_SHORT) nshort++;
        else if (k == TOK_KW_CHAR) nchar++;
        else if (k == TOK_KW_INT) nint++;
        else if (k == TOK_KW_VOID) nvoid++;
        else if (k == TOK_KW_CONST || k == TOK_KW_VOLATILE ||
                 k == TOK_KW_RESTRICT) { advance(ps); continue; }
        else break;
        any++;
        advance(ps);
    }
    if (!any)
        return NULL;
    if (nfloat || ndouble) {
        if (uns != -1 || nchar || nshort || nint || nvoid ||
            (nfloat && ndouble))
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "invalid type specifier combination");
        /* 'long double' is accepted AS double — there is no 80-bit
         * type here, and saying so beats pretending. */
        return ty_base(nfloat ? TY_FLOAT : TY_DOUBLE, 0);
    }
    if (nvoid) {
        if (any > 1)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "void cannot combine with other specifiers");
        return ty_base(TY_VOID, 0);
    }
    if (nlong > 2 || (nshort && nlong) || (nchar && (nshort || nlong)) ||
        (nchar && nint))
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "invalid type specifier combination");
    enum ty_kind kind = nchar ? TY_CHAR :
                        nshort ? TY_SHORT :
                        nlong ? TY_LONG : TY_INT;
    return ty_base(kind, uns == 1);
}

static struct type *parse_stars(struct parser *ps, struct type *t)
{
    for (;;) {
        skip_quals(ps); /* char * const p, const char *p, ... */
        if (cur(ps)->kind != TOK_STAR)
            return t;
        t = ty_ptr(t);
        advance(ps);
    }
}

/* Folds an array-size expression at PARSE time. It can evaluate
 * sizeof(type) because the parser owns the typedef and tag tables —
 * which is what real headers need: newlib's fd_set is
 * `__fds_bits[_howmany(FD_SETSIZE, _NFDBITS)]`, and _NFDBITS expands to
 * ((int)sizeof(__fd_mask) * 8). Anything it cannot evaluate (an
 * identifier, sizeof of an expression) is refused by name. */
static int size_fold(const struct expr *e, long *out)
{
    long a, b;

    switch (e->kind) {
    case EXPR_NUM:
        *out = e->num;
        return 1;
    case EXPR_SIZEOF:
        if (!e->cast_ty)
            return 0; /* sizeof(expr) needs types this pass lacks */
        *out = ty_size(e->cast_ty);
        return 1;
    case EXPR_CAST:
        return size_fold(e->rhs, out);
    case EXPR_NEG:
        if (!size_fold(e->rhs, &a)) return 0;
        *out = -a;
        return 1;
    case EXPR_BNOT:
        if (!size_fold(e->rhs, &a)) return 0;
        *out = ~a;
        return 1;
    case EXPR_NOT:
        if (!size_fold(e->rhs, &a)) return 0;
        *out = !a;
        return 1;
    case EXPR_BINOP:
        if (!size_fold(e->lhs, &a) || !size_fold(e->rhs, &b))
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
        case B_LT:  *out = a < b; return 1;
        case B_GT:  *out = a > b; return 1;
        case B_LE:  *out = a <= b; return 1;
        case B_GE:  *out = a >= b; return 1;
        case B_EQ:  *out = a == b; return 1;
        case B_NE:  *out = a != b; return 1;
        default: return 0;
        }
    default:
        return 0;
    }
}

/* Shared by locals, globals, and members: trailing [N]([M]...) turns t
 * into (nested) array types. Sizes are constant expressions. */
static struct type *parse_array_dims(struct parser *ps, struct type *t)
{
    int dims[4];
    int ndims = 0;
    while (cur(ps)->kind == TOK_LBRACKET) {
        advance(ps);
        int dim = 0; /* [] : legal for params (adjusts to a pointer);
                        elsewhere caught as an incomplete type */
        if (cur(ps)->kind != TOK_RBRACKET) {
            int dline = cur(ps)->line;
            struct expr *de = parse_cond(ps);
            long dv;
            if (!size_fold(de, &dv))
                diag_fatal(ps->lx.file, dline,
                           "array size must be a constant expression");
            if (dv < 0)
                diag_fatal(ps->lx.file, dline,
                           "array size cannot be negative");
            dim = (int)dv; /* 0 is the extern/flexible form */
        }
        if (ndims >= 4)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "more than 4 array dimensions");
        dims[ndims++] = dim;
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
        /* allow_body: nested struct/union definitions are legal C */
        struct type *spec = parse_type_spec(ps, 1);
        if (!spec)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a member type before %s",
                       tok_describe(cur(ps)));
        for (;;) { /* declarators share the base: int a, *b, c[4]; */
            int mline = cur(ps)->line;
            const char *mname;
            struct type *mty = parse_declarator(ps, spec, &mname);
            if (mty->kind == TY_VOID)
                diag_fatal(ps->lx.file, mline,
                           "a member cannot have type void");
            if (mty->kind == TY_FUNC)
                diag_fatal(ps->lx.file, mline,
                           "a member cannot be a function — use a "
                           "function pointer");
            if (!mname)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a member name before %s",
                           tok_describe(cur(ps)));
            if (ty_size(mty) == 0)
                diag_fatal(ps->lx.file, mline,
                           "member '%s' has incomplete type %s",
                           mname, ty_name(mty));
            for (int i = 0; i < n; i++)
                if (strcmp(ms[i].name, mname) == 0)
                    diag_fatal(ps->lx.file, mline,
                               "duplicate member '%s'", mname);
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                ms = xrealloc(ms, (size_t)cap * sizeof *ms);
            }
            ms[n].name = mname;
            ms[n].ty = mty;
            ms[n].off = 0;
            n++;
            if (cur(ps)->kind == TOK_COMMA) {
                advance(ps);
                continue;
            }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
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
static struct expr *parse_comma(struct parser *ps);
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
    case TOK_FNUM:
        e = new_expr(EXPR_FNUM, t->line);
        e->fnum = t->fnum;
        e->ty = ty_base(t->fnum_is_float ? TY_FLOAT : TY_DOUBLE, 0);
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
        e = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        return e;
    case TOK_IDENT: {
        reject_reserved(ps, t->text, t->line);
        e = new_expr(EXPR_VAR, t->line);
        e->name = t->text;
        advance(ps);
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
    (void)ps;
    struct expr *e = new_expr(EXPR_INCDEC, line);
    e->lhs = target;
    e->is_post = is_post;
    e->delta = delta;
    return e;
}

static struct expr *parse_postfix(struct parser *ps)
{
    struct expr *e = parse_primary(ps);
    for (;;) {
        if (cur(ps)->kind == TOK_LPAREN) {
            /* a call — through a name or any pointer-valued expression */
            int line = cur(ps)->line;
            advance(ps);
            struct expr *call = new_expr(EXPR_CALL, line);
            call->lhs = e;
            if (e->kind == EXPR_VAR)
                call->name = e->name;
            if (cur(ps)->kind != TOK_RPAREN) {
                for (;;) {
                    if (call->nargs >= MAX_PARAMS)
                        diag_fatal(ps->lx.file, cur(ps)->line,
                                   "more than %d call arguments",
                                   MAX_PARAMS);
                    call->args[call->nargs++] = parse_expr(ps);
                    if (cur(ps)->kind != TOK_COMMA)
                        break;
                    advance(ps);
                }
            }
            expect(ps, TOK_RPAREN, "')'");
            e = call;
        } else if (cur(ps)->kind == TOK_LBRACKET) {
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

/* Full expressions (statements, parens, conditions) allow the comma
 * operator; argument lists and initializers use parse_expr, where a
 * comma separates. */
static struct expr *parse_comma(struct parser *ps)
{
    struct expr *e = parse_expr(ps);
    while (cur(ps)->kind == TOK_COMMA) {
        struct expr *c = new_expr(EXPR_COMMA, cur(ps)->line);
        advance(ps);
        c->lhs = e;
        c->rhs = parse_expr(ps);
        e = c;
    }
    return e;
}

static struct expr *parse_cond(struct parser *ps)
{
    struct expr *e = parse_lor(ps);
    if (cur(ps)->kind != TOK_QUESTION)
        return e;
    struct expr *r = new_expr(EXPR_COND, cur(ps)->line);
    advance(ps);
    r->args[0] = e;
    r->nargs = 1;
    r->lhs = parse_comma(ps); /* the then-branch is a FULL expression */
    expect(ps, TOK_COLON, "':'");
    r->rhs = parse_cond(ps); /* right-associative, as in C */
    return r;
}

static struct expr *parse_expr(struct parser *ps)
{
    struct expr *e = parse_cond(ps);
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
    advance(ps);

    if (comp < 0) {
        struct expr *a = new_expr(EXPR_ASSIGN, line);
        a->lhs = e;
        a->rhs = parse_expr(ps);
        return a;
    }
    /* `x op= y` keeps its own node rather than desugaring to
     * `x = x op y`: through a pointer or a member the address must be
     * evaluated ONCE, and the desugared form evaluates it twice. */
    struct expr *a = new_expr(EXPR_COMPOUND, line);
    a->op = compound_assign[comp].op;
    a->lhs = e;
    a->rhs = parse_expr(ps);
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
        while (*tail) /* a declaration may be a chain: int a, b; */
            tail = &(*tail)->next;
    }
    advance(ps); /* '}' */
    return s;
}

static struct stmt *parse_stmt(struct parser *ps, int allow_decl)
{
    struct token *t = cur(ps);
    struct stmt *s;

    if (t->kind == TOK_KW_STATIC || at_type_start(ps)) {
        int local_static = 0;
        if (t->kind == TOK_KW_STATIC) {
            local_static = 1;
            advance(ps);
            if (!at_type_start(ps))
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a type after 'static'");
        }
        if (!allow_decl)
            diag_fatal(ps->lx.file, t->line,
                       "a declaration cannot be the body of if/while/for "
                       "(C99 forbids it too); wrap it in braces");
        struct type *base = parse_type_spec(ps, 0);
        struct stmt *head = NULL, **dtail = &head;
        for (;;) {
            s = new_stmt(STMT_DECL, t->line);
            const char *dname;
            s->dty = parse_declarator(ps, base, &dname);
            if (s->dty->kind == TY_VOID || s->dty->kind == TY_FUNC)
                diag_fatal(ps->lx.file, t->line,
                           "a variable cannot have type %s",
                           ty_name(s->dty));
            if (!dname)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a variable name before %s",
                           tok_describe(cur(ps)));
            s->name = dname;
            s->is_static = local_static;
            int was_array = s->dty->kind == TY_ARRAY;
            if (was_array && cur(ps)->kind == TOK_ASSIGN) {
                /* the one array initializer that matters here:
                 * char buf[] = "..." (and char buf[N] = "...") */
                advance(ps);
                if (cur(ps)->kind != TOK_STR)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "only string literals may initialize an "
                               "array");
                s->expr = parse_primary(ps);
            }
            if (cur(ps)->kind == TOK_ASSIGN) {
                advance(ps);
                s->expr = parse_expr(ps);
            }
            /* checked AFTER the initializer, because `char a[] = "..."`
             * takes its size from the literal */
            if (ty_size(s->dty) == 0 &&
                !(s->expr && s->expr->kind == EXPR_STR))
                diag_fatal(ps->lx.file, s->line,
                           "'%s' has incomplete type %s", s->name,
                           ty_name(s->dty));
            *dtail = s;
            dtail = &s->next;
            if (cur(ps)->kind == TOK_COMMA) {
                advance(ps);
                continue;
            }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
        return head;
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
        if (at_type_start(ps)) {
            /* `for (int i = 0; ...)`. One flat scope per function means
             * the variable simply outlives the loop — which ACCEPTS
             * fewer programs than C (a second loop reusing the name is
             * refused), never more. */
            s->initdecl = parse_stmt(ps, 1); /* consumes its own ';' */
        } else {
            if (cur(ps)->kind != TOK_SEMI)
                s->init = parse_comma(ps);
            expect(ps, TOK_SEMI, "';'");
        }
        if (cur(ps)->kind != TOK_SEMI) /* NULL cond = forever; break exits */
            s->cond = parse_expr(ps);
        expect(ps, TOK_SEMI, "';'");
        if (cur(ps)->kind != TOK_RPAREN)
            s->step = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_DO:
        s = new_stmt(STMT_DO, t->line);
        advance(ps);
        s->body = parse_controlled(ps);
        if (cur(ps)->kind != TOK_KW_WHILE)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected 'while' after a do-body, got %s",
                       tok_describe(cur(ps)));
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        expect(ps, TOK_SEMI, "';'");
        return s;
    case TOK_KW_SWITCH:
        s = new_stmt(STMT_SWITCH, t->line);
        advance(ps);
        expect(ps, TOK_LPAREN, "'('");
        s->cond = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        s->body = parse_controlled(ps);
        return s;
    case TOK_KW_CASE:
        /* a position MARKER in the switch body's list, not a wrapper —
         * C's fallthrough is what forces that shape */
        s = new_stmt(STMT_CASE, t->line);
        advance(ps);
        s->expr = parse_cond(ps); /* folded to a constant by sema */
        expect(ps, TOK_COLON, "':'");
        return s;
    case TOK_KW_DEFAULT:
        s = new_stmt(STMT_DEFAULT, t->line);
        advance(ps);
        expect(ps, TOK_COLON, "':'");
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
    case TOK_FNUM:
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
        s->expr = parse_comma(ps);
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
    /* `extern T x[];` is legal: the definition, and the size, live in
     * another translation unit. Nothing is emitted for it here. */
    if (ty_size(g->ty) == 0 && !is_extern)
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
    return g; /* caller handles ',' and ';' */
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
        for (;;) {
            const char *tname;
            struct type *tt = parse_declarator(ps, tbase, &tname);
            if (!tname)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "typedef needs a name, got %s",
                           tok_describe(cur(ps)));
            struct type *prev = find_typedef(ps, tname);
            if (prev && !ty_equal(prev, tt))
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "redefinition of typedef '%s'", tname);
            /* identical redefinition: headers do it; harmless */
            struct typedefent *te = xcalloc(1, sizeof *te);
            te->name = tname;
            te->ty = tt;
            te->next = ps->typedefs;
            ps->typedefs = te;
            if (cur(ps)->kind == TOK_COMMA) {
                advance(ps);
                continue;
            }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
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
    struct lexer fork = ps->lx;
    struct type *ty = parse_stars(ps, base);
    const char *name = NULL;
    int line = cur(ps)->line;
    if (cur(ps)->kind == TOK_IDENT) {
        name = cur(ps)->text;
        advance(ps);
    }
    if (!name || cur(ps)->kind != TOK_LPAREN) {
        /* not a function: rewind and parse global declarators */
        ps->lx = fork;
        for (;;) {
            const char *gname;
            int gline = cur(ps)->line;
            struct type *gt = parse_declarator(ps, base, &gname);
            if (!gname)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a name before %s",
                           tok_describe(cur(ps)));
            if (gt->kind == TY_FUNC)
                diag_fatal(ps->lx.file, gline,
                           "a variable cannot have a function type — "
                           "did you mean a function pointer (*)?");
            struct global *g = parse_global(ps, gt, gname, gline,
                                            is_static, is_extern);
            g->seq = seq;
            **gtail = g;
            *gtail = &g->next;
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
        expect(ps, TOK_SEMI, "';'");
        (void)u;
        return;
    }

    struct func *f = xcalloc(1, sizeof *f);
    /* 'extern' on a function is the default linkage — accept, ignore */
    f->is_static = is_static;
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
            struct type *spec = parse_type_spec(ps, 0);
            if (!spec)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a parameter type before %s",
                           tok_describe(cur(ps)));
            const char *pname;
            struct type *pt = parse_declarator(ps, spec, &pname);
            if (pt->kind == TY_ARRAY)
                pt = ty_ptr(pt->pointee); /* C's adjustment */
            if (pt->kind == TY_FUNC)
                pt = ty_ptr(pt);
            if (pt->kind == TY_VOID)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "a parameter cannot have type void");
            if (f->nparams >= MAX_PARAMS)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "more than %d parameters", MAX_PARAMS);
            f->param_tys[f->nparams] = pt;
            f->params[f->nparams] = pname; /* NULL fine in prototypes */
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
