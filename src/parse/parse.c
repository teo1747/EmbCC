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
           k == TOK_KW_FLOAT || k == TOK_KW_DOUBLE || k == TOK_KW_BOOL;
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
static void parse_static_assert(struct parser *ps);
static struct expr *parse_initializer(struct parser *ps);

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
                if (nad >= 4)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "more than 4 array dimensions");
                if (cur(ps)->kind == TOK_RBRACKET) {
                    /* an omitted size: `(*arr[])(void)` — an incomplete
                     * array, valid for an extern like crt0's brackets. */
                    adims[nad++] = 0;
                    advance(ps);
                    continue;
                }
                int dline = cur(ps)->line;
                struct expr *de = parse_cond(ps);
                long dv;
                if (!size_fold(de, &dv) || dv <= 0)
                    diag_fatal(ps->lx.file, dline,
                               "array size must be a positive constant "
                               "expression");
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

/* An abstract type name (a cast target, a sizeof operand): a declarator
 * with the name omitted — so `void (*)(void)` and `int (*)[4]` parse, not
 * just pointer stars. parse_declarator already allows a missing name. */
static struct type *parse_type_name(struct parser *ps, struct type *base)
{
    const char *unused = NULL;
    return parse_declarator(ps, base, &unused);
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
        /* __builtin_va_list is `char *` — the same representation EmbCC's
         * <stdarg.h> gives va_list — so `typedef __builtin_va_list ...`
         * (as some headers write it) resolves. */
        if (strcmp(cur(ps)->text, "__builtin_va_list") == 0) {
            advance(ps);
            return ty_ptr(ty_base(TY_CHAR, 0));
        }
        struct type *td = find_typedef(ps, cur(ps)->text);
        if (!td)
            return NULL;
        advance(ps);
        return td;
    }
    /* base specifiers in any order: unsigned long int, long unsigned... */
    int uns = -1, nlong = 0, nshort = 0, nchar = 0, nint = 0, nvoid = 0;
    int nfloat = 0, ndouble = 0, nbool = 0;
    int any = 0;
    for (;;) {
        enum tok_kind k = cur(ps)->kind;
        if (k == TOK_KW_FLOAT) nfloat++;
        else if (k == TOK_KW_BOOL) nbool++;
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
    if (nbool) {
        if (any > 1)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "_Bool cannot combine with other specifiers");
        return ty_base(TY_BOOL, 0);
    }
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
/* The unit being parsed, so size_fold can resolve enum constants (which are
 * compile-time integer constants) in constant expressions like array sizes. */
static struct unit *g_fold_unit;

/* The type of a constant-expression subset — enough to fold sizeof(EXPR) in
 * an integer-constant-expression: a cast fixes the type, `->`/`.` reach a
 * member, `*` dereferences. `sizeof(((struct V*)0)->field)` is the shape the
 * kernel uses to bound one struct's storage against another's. */
static struct type *ce_type(const struct expr *e)
{
    switch (e->kind) {
    case EXPR_CAST:
        return e->cast_ty;
    case EXPR_DEREF: {
        struct type *t = ce_type(e->rhs);
        return t && t->kind == TY_PTR ? t->pointee : NULL;
    }
    case EXPR_MEMBER: {
        struct type *bt = ce_type(e->lhs);
        if (!bt)
            return NULL;
        struct type *st = e->is_arrow
                        ? (bt->kind == TY_PTR ? bt->pointee : NULL) : bt;
        if (!st || st->kind != TY_STRUCT || !st->complete)
            return NULL;
        struct member *m = ty_find_member(st, e->name);
        return m ? m->ty : NULL;
    }
    default:
        return NULL;
    }
}

static int size_fold(const struct expr *e, long *out)
{
    long a, b;

    switch (e->kind) {
    case EXPR_NUM:
        *out = e->num;
        return 1;
    case EXPR_VAR:
        /* an enumerator is an integer constant expression: `int a[N];` */
        for (struct econst *ec = g_fold_unit ? g_fold_unit->econsts : NULL;
             ec; ec = ec->next)
            if (strcmp(ec->name, e->name) == 0) {
                *out = ec->val;
                return 1;
            }
        return 0;
    case EXPR_SIZEOF: {
        struct type *t = e->cast_ty ? e->cast_ty : ce_type(e->rhs);
        if (!t || ty_size(t) == 0)
            return 0;   /* sizeof(expr) whose type this pass cannot resolve */
        *out = ty_size(t);
        return 1;
    }
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
        case B_LAND: *out = a && b; return 1;
        case B_LOR:  *out = a || b; return 1;
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

/* The GNU attributes EmbCC honors; everything else is parsed and dropped. */
struct attrs { int packed; int aligned; int weak; int noreturn; };

/* Match `name`, `__name`, or `__name__` against a base attribute name. */
static int attr_is(const char *n, const char *base)
{
    if (strcmp(n, base) == 0)
        return 1;
    size_t bl = strlen(base);
    return strncmp(n, "__", 2) == 0 &&
           strncmp(n + 2, base, bl) == 0 &&
           strcmp(n + 2 + bl, "__") == 0;
}

/* Consume a run of `__attribute__((...))`. packed / aligned(N) (struct
 * layout) and weak (symbol binding) are recorded in `out`; every other
 * attribute is skipped along with its balanced parenthesized arguments.
 * Callers pass out=NULL where no attribute is meaningful (member/param). */
static void parse_attributes(struct parser *ps, struct attrs *out)
{
    while (cur(ps)->kind == TOK_KW_ATTRIBUTE) {
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after __attribute__");
        expect(ps, TOK_LPAREN, "a second '(' after __attribute__");
        while (cur(ps)->kind != TOK_RPAREN && cur(ps)->kind != TOK_EOF) {
            const char *name = cur(ps)->kind == TOK_IDENT ? cur(ps)->text
                                                          : NULL;
            advance(ps);
            long arg = -1;
            if (cur(ps)->kind == TOK_LPAREN) {
                advance(ps);
                if (cur(ps)->kind == TOK_NUM)
                    arg = cur(ps)->num;
                int depth = 1;
                while (depth > 0 && cur(ps)->kind != TOK_EOF) {
                    if (cur(ps)->kind == TOK_LPAREN) depth++;
                    else if (cur(ps)->kind == TOK_RPAREN) depth--;
                    advance(ps);
                }
            }
            if (name && out) {
                if (attr_is(name, "packed")) out->packed = 1;
                else if (attr_is(name, "weak")) out->weak = 1;
                else if (attr_is(name, "noreturn")) out->noreturn = 1;
                else if (attr_is(name, "aligned"))
                    out->aligned = arg > 0 ? (int)arg : 16;
            }
            if (cur(ps)->kind == TOK_COMMA)
                advance(ps);
            else
                break;
        }
        expect(ps, TOK_RPAREN, "')'");
        expect(ps, TOK_RPAREN, "a second ')' to close __attribute__");
    }
}

static struct type *parse_struct_body(struct parser *ps, struct type *t)
{
    expect(ps, TOK_LBRACE, "'{'");
    struct member *ms = NULL;
    int n = 0, cap = 0;

    while (cur(ps)->kind != TOK_RBRACE) {
        /* a `_Static_assert` among the members: checked, contributes none */
        if (cur(ps)->kind == TOK_KW_STATIC_ASSERT) {
            parse_static_assert(ps);
            continue;
        }
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
            /* A bitfield: `T name : width` or an anonymous `T : width`
             * (padding) / `T : 0` (a separator forcing the next field to a
             * storage-unit boundary). Only integer types may be bitfields. */
            int is_bf = 0, bit_width = 0;
            if (cur(ps)->kind == TOK_COLON) {
                advance(ps);
                if (!ty_is_integer(mty))
                    diag_fatal(ps->lx.file, mline,
                               "a bitfield must have integer type, not %s",
                               ty_name(mty));
                struct expr *we = parse_cond(ps);
                long wv;
                if (!size_fold(we, &wv) || wv < 0)
                    diag_fatal(ps->lx.file, mline,
                               "a bitfield width must be a constant >= 0");
                if (wv > 8 * (long)ty_size(mty))
                    diag_fatal(ps->lx.file, mline,
                               "bitfield '%s' width %ld exceeds its type %s",
                               mname ? mname : "<anon>", wv, ty_name(mty));
                if (wv == 0 && mname)
                    diag_fatal(ps->lx.file, mline,
                               "a named bitfield '%s' cannot have width 0",
                               mname);
                is_bf = 1;
                bit_width = (int)wv;
            }
            if (mty->kind == TY_VOID)
                diag_fatal(ps->lx.file, mline,
                           "a member cannot have type void");
            if (mty->kind == TY_FUNC)
                diag_fatal(ps->lx.file, mline,
                           "a member cannot be a function — use a "
                           "function pointer");
            /* An anonymous struct/union member (`struct { ... };` with no
             * declarator) is legal C11 — its members are reached as if they
             * belonged to the enclosing type. A nameless non-aggregate is
             * still an error. */
            if (!mname && !is_bf && mty->kind != TY_STRUCT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a member name before %s",
                           tok_describe(cur(ps)));
            if (!is_bf && ty_size(mty) == 0)
                diag_fatal(ps->lx.file, mline,
                           "member '%s' has incomplete type %s",
                           mname, ty_name(mty));
            if (mname)
                for (int i = 0; i < n; i++)
                    if (ms[i].name && strcmp(ms[i].name, mname) == 0)
                        diag_fatal(ps->lx.file, mline,
                                   "duplicate member '%s'", mname);
            if (n == cap) {
                cap = cap ? cap * 2 : 8;
                ms = xrealloc(ms, (size_t)cap * sizeof *ms);
            }
            parse_attributes(ps, NULL); /* member attributes: ignored */
            ms[n].name = mname;
            ms[n].ty = mty;
            ms[n].off = 0;
            ms[n].is_bitfield = is_bf;
            ms[n].bit_off = 0;
            ms[n].bit_width = bit_width;
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
    struct attrs at = { 0, 0, 0, 0 };
    parse_attributes(ps, &at);   /* struct {...} __attribute__((packed)) */
    if (n == 0)
        diag_fatal(ps->lx.file, cur(ps)->line,
                   "a struct/union needs at least one member");
    ty_struct_layout(t, ms, n, at.packed, at.aligned);
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
    e->desig_index = -1;      /* positional unless a [i] designator sets it */
    e->desig_index_hi = -1;
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
    case TOK_KW_GENERIC: {
        /* _Generic(controlling, T1: e1, ..., default: eN) — a compile-time
         * type-directed selection; sema picks the matching arm. */
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after _Generic");
        e = new_expr(EXPR_GENERIC, t->line);
        e->lhs = parse_expr(ps);          /* the controlling expression */
        int cap = 0;
        while (cur(ps)->kind == TOK_COMMA) {
            advance(ps);
            struct type *at = NULL;
            if (cur(ps)->kind == TOK_KW_DEFAULT)
                advance(ps);              /* the default association */
            else
                at = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_COLON, "':' in a _Generic association");
            struct expr *ae = parse_expr(ps);
            if (e->ngen == cap) {
                cap = cap ? cap * 2 : 4;
                e->gtypes = xrealloc(e->gtypes, (size_t)cap * sizeof *e->gtypes);
                e->gexprs = xrealloc(e->gexprs, (size_t)cap * sizeof *e->gexprs);
            }
            e->gtypes[e->ngen] = at;
            e->gexprs[e->ngen] = ae;
            e->ngen++;
        }
        expect(ps, TOK_RPAREN, "')' to close _Generic");
        return e;
    }
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
    case TOK_STR: {
        /* Adjacent string literals concatenate (C translation phase 6):
         * "foo" "bar" is one literal "foobar". num counts the NUL, so
         * each join drops the running string's terminator and appends
         * the next literal's bytes (including its NUL). */
        e = new_expr(EXPR_STR, t->line);
        size_t len = (size_t)t->num;
        char *bytes = xmalloc(len);
        memcpy(bytes, t->text, len);
        advance(ps);
        while (cur(ps)->kind == TOK_STR) {
            size_t add = (size_t)cur(ps)->num;
            char *nb = xmalloc(len - 1 + add);
            memcpy(nb, bytes, len - 1);
            memcpy(nb + len - 1, cur(ps)->text, add);
            bytes = nb;
            len = len - 1 + add;
            advance(ps);
        }
        e->name = bytes;
        e->num = (long)len;
        return e;
    }
    case TOK_LPAREN:
        advance(ps);
        e = parse_comma(ps);
        expect(ps, TOK_RPAREN, "')'");
        return e;
    case TOK_IDENT: {
        /* va_arg(ap, type) -> __builtin_va_arg((ap), type): a special form,
         * because its second argument is a TYPE, not an expression. lhs
         * holds ap; cast_ty holds the type read. */
        if (strcmp(t->text, "__builtin_va_arg") == 0) {
            int line = t->line;
            advance(ps);
            expect(ps, TOK_LPAREN, "'(' after __builtin_va_arg");
            e = new_expr(EXPR_VA_ARG, line);
            e->lhs = parse_expr(ps);
            expect(ps, TOK_COMMA, "',' before the va_arg type");
            e->cast_ty = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_RPAREN, "')' to close __builtin_va_arg");
            return e;
        }
        /* __builtin_offsetof(type, member-designator) — the byte offset of a
         * member, folded to a size_t constant right here so it is usable in an
         * integer-constant-expression (a _Static_assert, an array size). The
         * designator may descend through `.field` and `[index]`. This is what
         * <stddef.h>'s offsetof expands to. */
        if (strcmp(t->text, "__builtin_offsetof") == 0) {
            int line = t->line;
            advance(ps);
            expect(ps, TOK_LPAREN, "'(' after __builtin_offsetof");
            struct type *ty = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_COMMA, "',' before the member designator");
            long off = 0;
            for (;;) {
                if (ty->kind != TY_STRUCT || !ty->complete)
                    diag_fatal(ps->lx.file, line,
                               "offsetof needs a complete struct/union type");
                if (cur(ps)->kind != TOK_IDENT)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "expected a member name in offsetof");
                struct member *m2 = ty_find_member(ty, cur(ps)->text);
                if (!m2)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "%s has no member '%s'", ty_name(ty),
                               cur(ps)->text);
                off += m2->off;
                ty = m2->ty;
                advance(ps);
                while (cur(ps)->kind == TOK_LBRACKET) {
                    advance(ps);
                    long iv;
                    if (!size_fold(parse_cond(ps), &iv))
                        diag_fatal(ps->lx.file, line,
                                   "offsetof array index must be constant");
                    expect(ps, TOK_RBRACKET, "']'");
                    if (ty->kind != TY_ARRAY)
                        diag_fatal(ps->lx.file, line,
                                   "offsetof indexed a non-array member");
                    off += iv * ty_size(ty->pointee);
                    ty = ty->pointee;
                }
                if (cur(ps)->kind == TOK_DOT) { advance(ps); continue; }
                break;
            }
            expect(ps, TOK_RPAREN, "')' to close __builtin_offsetof");
            e = new_expr(EXPR_NUM, line);
            e->num = off;
            e->ty = ty_base(TY_LONG, 1);   /* size_t */
            return e;
        }
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

/* Applies postfix operators (call, [], ., ->, ++/--) to an already-parsed
 * primary/compound-literal seed. Split out so a compound literal can take
 * postfix too: `(struct P){...}.x`, `(int[]){1,2,3}[0]`. */
static struct expr *parse_postfix_ops(struct parser *ps, struct expr *e)
{
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

static struct expr *parse_postfix(struct parser *ps)
{
    return parse_postfix_ops(ps, parse_primary(ps));
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
    case TOK_PLUS:
        /* unary plus: identity on an arithmetic operand (the surrounding
         * context applies the usual promotions). Just yield the operand. */
        advance(ps);
        return parse_unary(ps);
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
                e->cast_ty = parse_type_name(ps, parse_type_spec(ps, 0));
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
            struct type *ct = parse_type_name(ps, parse_type_spec(ps, 0));
            expect(ps, TOK_RPAREN, "')'");
            /* `(type){ ... }` is a compound literal (an unnamed object),
             * not a cast — it can even take postfix operators. */
            if (cur(ps)->kind == TOK_LBRACE) {
                e = new_expr(EXPR_COMPLIT, t->line);
                e->cast_ty = ct;
                e->lhs = parse_initializer(ps);
                return parse_postfix_ops(ps, e);
            }
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

/* The element count an initializer list implies for an unsized array:
 * the highest index reached, where a `[i] =` designator repositions the
 * running index and each element then advances it by one. */
int initlist_array_count(const struct expr *il)
{
    int idx = 0, max = 0;
    for (int i = 0; i < il->nelems; i++) {
        if (il->elems[i]->desig_index >= 0)
            idx = il->elems[i]->desig_index;
        if (il->elems[i]->desig_index_hi >= 0)  /* `[lo ... hi]` ends at hi */
            idx = il->elems[i]->desig_index_hi;
        idx++;
        if (idx > max)
            max = idx;
    }
    return max;
}

/* An initializer: either an ordinary expression or a brace list, which
 * may nest. Sema matches it against the target type. */
static struct expr *parse_initializer(struct parser *ps)
{
    if (cur(ps)->kind != TOK_LBRACE)
        return parse_expr(ps);

    struct expr *e = new_expr(EXPR_INITLIST, cur(ps)->line);
    int cap = 0;
    advance(ps);
    while (cur(ps)->kind != TOK_RBRACE) {
        if (cur(ps)->kind == TOK_EOF)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "unterminated initializer");
        /* A designator: struct field `.name =` or array element `[i] =`.
         * One level only (no `[i].f =` chains — no EmbCC source needs it). */
        const char *field = NULL;
        long index = -1, index_hi = -1;
        if (cur(ps)->kind == TOK_LBRACKET) {
            int iline = cur(ps)->line;
            advance(ps);
            struct expr *ie = parse_cond(ps);
            if (!size_fold(ie, &index) || index < 0)
                diag_fatal(ps->lx.file, iline,
                           "an array designator [index] must be a constant "
                           ">= 0");
            if (cur(ps)->kind == TOK_ELLIPSIS) {  /* GNU range `[lo ... hi]` */
                advance(ps);
                struct expr *he = parse_cond(ps);
                if (!size_fold(he, &index_hi) || index_hi < index)
                    diag_fatal(ps->lx.file, iline,
                               "an array range [lo ... hi] must have "
                               "constant hi >= lo");
            }
            expect(ps, TOK_RBRACKET, "']'");
            expect(ps, TOK_ASSIGN, "'=' after an array designator");
        } else if (cur(ps)->kind == TOK_DOT) {
            advance(ps);
            if (cur(ps)->kind != TOK_IDENT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a field name after '.'");
            field = cur(ps)->text;
            advance(ps);
            expect(ps, TOK_ASSIGN, "'=' after a field designator");
        }
        if (e->nelems == cap) {
            cap = cap ? cap * 2 : 8;
            e->elems = xrealloc(e->elems,
                                (size_t)cap * sizeof *e->elems);
        }
        struct expr *el = parse_initializer(ps);
        el->desig_field = field;
        el->desig_index = index >= 0 ? (int)index : -1;
        el->desig_index_hi = (int)index_hi;
        e->elems[e->nelems++] = el;
        if (cur(ps)->kind != TOK_COMMA)
            break;
        advance(ps); /* a trailing comma before '}' is legal C */
    }
    expect(ps, TOK_RBRACE, "'}'");
    return e;
}

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

/* A string literal (a run of adjacent ones concatenated), as a plain
 * NUL-terminated C string — for asm templates, constraints, and clobbers. */
static char *parse_str_literal(struct parser *ps, const char *what)
{
    if (cur(ps)->kind != TOK_STR)
        diag_fatal(ps->lx.file, cur(ps)->line, "expected %s", what);
    struct token *t = cur(ps);
    size_t len = (size_t)t->num;            /* includes the NUL */
    char *bytes = xmalloc(len);
    memcpy(bytes, t->text, len);
    advance(ps);
    while (cur(ps)->kind == TOK_STR) {
        size_t add = (size_t)cur(ps)->num;
        char *nb = xmalloc(len - 1 + add);
        memcpy(nb, bytes, len - 1);
        memcpy(nb + len - 1, cur(ps)->text, add);
        bytes = nb;
        len = len - 1 + add;
        advance(ps);
    }
    return bytes;
}

/* A ':'-delimited operand list of `"constraint"(expr)` items, empty when the
 * next token is ':' or ')'. */
static void parse_asm_operands(struct parser *ps, struct asm_operand **out,
                               int *nout)
{
    int cap = 0;
    while (cur(ps)->kind != TOK_COLON && cur(ps)->kind != TOK_RPAREN) {
        if (*nout == cap) {
            cap = cap ? cap * 2 : 4;
            *out = xrealloc(*out, (size_t)cap * sizeof **out);
        }
        struct asm_operand *op = &(*out)[(*nout)++];
        op->constraint = parse_str_literal(ps, "an asm constraint");
        expect(ps, TOK_LPAREN, "'(' after an asm constraint");
        op->expr = parse_expr(ps);
        expect(ps, TOK_RPAREN, "')' after an asm operand");
        if (cur(ps)->kind != TOK_COMMA)
            break;
        advance(ps);
    }
}

/* GCC extended asm: asm [volatile] ( template
 *     [ : outputs [ : inputs [ : clobbers ] ] ] ) ;  */
static struct stmt *parse_asm_stmt(struct parser *ps)
{
    struct stmt *s = new_stmt(STMT_ASM, cur(ps)->line);
    struct asm_stmt *a = xcalloc(1, sizeof *a);
    s->asm_s = a;
    advance(ps); /* 'asm' / '__asm__' */
    if (cur(ps)->kind == TOK_KW_VOLATILE) {
        a->is_volatile = 1;
        advance(ps);
    }
    expect(ps, TOK_LPAREN, "'(' after asm");
    a->tmpl = parse_str_literal(ps, "an asm template string");
    if (cur(ps)->kind == TOK_COLON) {
        advance(ps);
        parse_asm_operands(ps, &a->out, &a->nout);
    }
    if (cur(ps)->kind == TOK_COLON) {
        advance(ps);
        parse_asm_operands(ps, &a->in, &a->nin);
    }
    if (cur(ps)->kind == TOK_COLON) {
        /* clobbers: string literals, parsed and discarded — EmbCC keeps
         * every value in a stack slot, so a clobbered register holds no
         * live value to preserve. */
        advance(ps);
        while (cur(ps)->kind == TOK_STR) {
            (void)parse_str_literal(ps, "a clobber");
            if (cur(ps)->kind != TOK_COMMA)
                break;
            advance(ps);
        }
    }
    expect(ps, TOK_RPAREN, "')' to close asm");
    expect(ps, TOK_SEMI, "';'");
    return s;
}

static struct stmt *parse_stmt(struct parser *ps, int allow_decl)
{
    struct token *t = cur(ps);
    struct stmt *s;

    if (t->kind == TOK_KW_ASM)
        return parse_asm_stmt(ps);

    /* A block-scope `_Static_assert`: checked now, emits nothing. */
    if (t->kind == TOK_KW_STATIC_ASSERT) {
        parse_static_assert(ps);
        return new_stmt(STMT_BLOCK, t->line);
    }

    /* Block-scope `typedef` and `extern`: declarations that emit no code and
     * take no local storage. Handled here at the parser level -- a local
     * typedef registers a type name; a local extern registers the external
     * global/function the reference resolves to. Both are unit-visible (the
     * one flat namespace EmbCC keeps), which admits a superset of C's block
     * scoping -- fine, and the same trade-off tags/typedefs already make. */
    if (t->kind == TOK_KW_TYPEDEF || t->kind == TOK_KW_EXTERN) {
        if (!allow_decl)
            diag_fatal(ps->lx.file, t->line,
                       "a declaration cannot be the body of if/while/for; "
                       "wrap it in braces");
        int is_td = t->kind == TOK_KW_TYPEDEF;
        advance(ps);
        struct type *base = parse_type_spec(ps, 1);
        if (!base)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a type after '%s'", is_td ? "typedef" : "extern");
        /* extern declarators become STMT_DECL nodes with is_extern set; sema
         * registers the unit global/function (appending to those lists at
         * PARSE time would race parse_top's own list tails). typedef registers
         * a name right here (its list is a prepend-list -- no tail to race). */
        struct stmt *ehead = NULL, **etail = &ehead;
        for (;;) {
            if (is_td) {
                const char *tname;
                struct type *tt = parse_declarator(ps, base, &tname);
                if (!tname)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "typedef needs a name, got %s", tok_describe(cur(ps)));
                struct type *prev = find_typedef(ps, tname);
                if (prev && !ty_equal(prev, tt))
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "redefinition of typedef '%s'", tname);
                struct typedefent *te = xcalloc(1, sizeof *te);
                te->name = tname; te->ty = tt;
                te->next = ps->typedefs; ps->typedefs = te;
            } else {
                struct type *dty = parse_stars(ps, base);
                if (cur(ps)->kind != TOK_IDENT)
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "expected a name before %s", tok_describe(cur(ps)));
                const char *dname = cur(ps)->text;
                int dline = cur(ps)->line;
                advance(ps);
                /* a function type is `name(params)`, else it's a variable */
                struct type *ety = cur(ps)->kind == TOK_LPAREN
                                 ? parse_fn_params(ps, dty)
                                 : parse_array_dims(ps, dty);
                struct stmt *sd = new_stmt(STMT_DECL, dline);
                sd->dty = ety; sd->name = dname; sd->is_extern = 1;
                *etail = sd; etail = &sd->next;
            }
            if (cur(ps)->kind == TOK_COMMA) { advance(ps); continue; }
            break;
        }
        expect(ps, TOK_SEMI, "';'");
        if (ehead)
            return ehead;                    /* extern declarations (sema-handled) */
        s = new_stmt(STMT_BLOCK, t->line);   /* typedef only -> no code */
        return s;
    }

    /* `register` is otherwise an ignored storage hint, but it carries the
     * `register T x __asm__("r10")` binding EmbCC needs to place an asm 'r'
     * operand — so accept it as a qualifier before the type. */
    int is_register = t->kind == TOK_IDENT &&
                      strcmp(t->text, "register") == 0;
    if (t->kind == TOK_KW_STATIC || is_register || at_type_start(ps)) {
        int local_static = 0;
        if (t->kind == TOK_KW_STATIC) {
            local_static = 1;
            advance(ps);
        } else if (is_register) {
            advance(ps);
        }
        while (cur(ps)->kind == TOK_KW_INLINE) /* accepted, ignored */
            advance(ps);
        if ((t->kind == TOK_KW_STATIC || is_register) && !at_type_start(ps))
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a type after the storage specifier");
        if (!allow_decl)
            diag_fatal(ps->lx.file, t->line,
                       "a declaration cannot be the body of if/while/for "
                       "(C99 forbids it too); wrap it in braces");
        /* allow_body=1: a block-scope struct/union/enum DEFINITION is legal C
         * (`union { double d; uint64_t u; } v;` inside a function). Tags share
         * the one flat tag namespace EmbCC keeps -- fine for the anonymous
         * types real code uses here; a same-named tag in two scopes is the
         * documented limitation, not a miscompile. */
        struct type *base = parse_type_spec(ps, 1);
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
            /* An optional `__asm__("reg")` register binding follows the
             * declarator: `register long r10 __asm__("r10") = a4;`. */
            if (cur(ps)->kind == TOK_KW_ASM) {
                advance(ps);
                expect(ps, TOK_LPAREN, "'(' after __asm__ register binding");
                s->asm_reg = parse_str_literal(ps, "a register name");
                expect(ps, TOK_RPAREN, "')' after the register name");
            }
            /* `T x[16] __attribute__((aligned(16)))` — a trailing attribute
             * on a local declarator (accepted; alignment is not yet honored
             * for a stack slot). */
            parse_attributes(ps, NULL);
            int was_array = s->dty->kind == TY_ARRAY;
            (void)was_array;
            if (cur(ps)->kind == TOK_ASSIGN) {
                advance(ps);
                s->expr = parse_initializer(ps);
            }
            /* checked AFTER the initializer, because `char a[] = "..."`
             * takes its size from the literal */
            /* an omitted array size is filled in by sema from the
             * initializer, so only an UNINITIALIZED one is incomplete */
            if (ty_size(s->dty) == 0 && !s->expr)
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

    /* A label: `IDENT ':'` prefixes a statement (a goto target). Peek one
     * token past the identifier; if it is ':' this is a label, else restore
     * and fall through to the expression-statement path. */
    if (t->kind == TOK_IDENT) {
        const char *lname = t->text;
        int lline = t->line;
        struct lexer save = ps->lx;
        advance(ps);
        if (cur(ps)->kind == TOK_COLON) {
            advance(ps);                       /* consume ':' */
            s = new_stmt(STMT_LABEL, lline);
            s->name = lname;
            s->body = parse_stmt(ps, allow_decl);
            return s;
        }
        ps->lx = save;                         /* not a label */
    }

    switch (t->kind) {
    case TOK_SEMI:                             /* the null statement */
        s = new_stmt(STMT_BLOCK, t->line);     /* an empty block does nothing */
        advance(ps);
        return s;
    case TOK_KW_GOTO:
        s = new_stmt(STMT_GOTO, t->line);
        advance(ps);
        if (cur(ps)->kind != TOK_IDENT)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected a label name after 'goto'");
        s->name = cur(ps)->text;
        advance(ps);
        expect(ps, TOK_SEMI, "';'");
        return s;
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
    g->file = ps->lx.file;
    g->line = line;
    g->is_static = is_static;
    g->is_extern = is_extern;
    g->ty = ty;

    if (g->ty->kind == TY_VOID)
        diag_fatal(ps->lx.file, line, "a variable cannot have type void");
    g->ty = parse_array_dims(ps, g->ty);
    int has_init = cur(ps)->kind == TOK_ASSIGN;
    /* `extern T x[];` is legal: the definition, and the size, live in
     * another translation unit. An omitted array size is legal when an
     * initializer follows — it supplies the count — so defer that check. */
    int size_from_init = g->ty->kind == TY_ARRAY && g->ty->count == 0 &&
                         has_init;
    if (ty_size(g->ty) == 0 && !is_extern && !size_from_init)
        diag_fatal(ps->lx.file, line,
                   "'%s' has incomplete type %s", name, ty_name(g->ty));
    if (has_init) {
        advance(ps);
        if (is_extern)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "'extern' with an initializer");
        if (g->ty->kind == TY_ARRAY || g->ty->kind == TY_STRUCT) {
            /* Aggregates (and any string/relocation content) are lowered
             * by sema: it flattens the initializer against the type and
             * const-folds each leaf into the object's byte image. */
            g->init_expr = parse_initializer(ps);
            if (g->ty->kind == TY_ARRAY && g->ty->count == 0) {
                struct expr *ie = g->init_expr;
                if (ie->kind == EXPR_STR &&
                    g->ty->pointee->kind == TY_CHAR)
                    g->ty = ty_array(g->ty->pointee, (int)ie->num);
                else if (ie->kind == EXPR_INITLIST)
                    g->ty = ty_array(g->ty->pointee,
                                     initlist_array_count(ie));
                else
                    diag_fatal(ps->lx.file, cur(ps)->line,
                               "'%s' needs a brace or string initializer "
                               "to supply its size", name);
            }
            g->has_init = 1;
        } else {
            /* A scalar/pointer global: a constant expression — an integer
             * literal, sizeof arithmetic, a string-literal address for a
             * pointer, 0 for a null pointer. sema flattens and folds it
             * through the same path as an aggregate leaf. */
            g->init_expr = parse_cond(ps);
            g->has_init = 1;
        }
    }
    return g; /* caller handles ',' and ';' */
}

/* Parses one top-level item into the unit: a function (prototype or
 * definition) or a global variable. */
/* `_Static_assert ( constant-expression , "message" ) ;` — evaluated now,
 * at parse time (like an array size). A false assertion is a fatal error
 * naming the message; a true one produces nothing. The message is optional
 * (C23 relaxed C11's requirement), which real headers rely on. Legal at
 * file scope, in a block, and in a struct/union body. */
static void parse_static_assert(struct parser *ps)
{
    int line = cur(ps)->line;
    advance(ps); /* _Static_assert */
    expect(ps, TOK_LPAREN, "'(' after _Static_assert");
    struct expr *ce = parse_cond(ps);
    long v;
    if (!size_fold(ce, &v))
        diag_fatal(ps->lx.file, line,
                   "_Static_assert needs a constant integer expression");
    const char *msg = NULL;
    if (cur(ps)->kind == TOK_COMMA) {
        advance(ps);
        msg = parse_str_literal(ps, "a _Static_assert message string");
    }
    expect(ps, TOK_RPAREN, "')' to close _Static_assert");
    expect(ps, TOK_SEMI, "';'");
    if (v == 0)
        diag_fatal(ps->lx.file, line, "static assertion failed: %s",
                   msg ? msg : "(no message)");
}

static void parse_top(struct parser *ps, struct unit *u,
                      struct func ***ftail, struct global ***gtail,
                      int seq)
{
    if (cur(ps)->kind == TOK_KW_STATIC_ASSERT) {
        parse_static_assert(ps);
        return;
    }

    int is_static = 0, is_extern = 0;
    struct attrs at = { 0, 0, 0, 0 };
    ps->seq = seq;

    /* A file-scope `__asm__("...")` block (crt0's _start stub). Basic asm
     * only — a template, no operands — assembled later by topasm.c. */
    if (cur(ps)->kind == TOK_KW_ASM) {
        int line = cur(ps)->line;
        advance(ps);
        expect(ps, TOK_LPAREN, "'(' after a file-scope asm");
        struct topasm *ta = xcalloc(1, sizeof *ta);
        ta->tmpl = parse_str_literal(ps, "an asm template string");
        ta->file = ps->lx.file;
        ta->line = line;
        expect(ps, TOK_RPAREN, "')' to close the asm block");
        expect(ps, TOK_SEMI, "';'");
        struct topasm **t = &u->topasm;
        while (*t)
            t = &(*t)->next;
        *t = ta;
        return;
    }

    /* Storage/function specifiers in any order; `inline` is accepted and
     * ignored — EmbCC emits an inline function as an ordinary one. */
    for (;;) {
        if (cur(ps)->kind == TOK_KW_STATIC) {
            is_static = 1;
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_EXTERN) {
            is_extern = 1;
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_INLINE) {
            advance(ps);
        } else if (cur(ps)->kind == TOK_KW_ATTRIBUTE) {
            parse_attributes(ps, &at); /* leading __attribute__((weak)) etc. */
        } else {
            break;
        }
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
            parse_attributes(ps, &at); /* int x __attribute__((weak)) = ... */
            struct global *g = parse_global(ps, gt, gname, gline,
                                            is_static, is_extern);
            parse_attributes(ps, &at); /* trailing: T x[] __attribute__((weak)) */
            g->is_weak = at.weak;
            g->seq = seq;
            g->def_seq = seq;
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
    f->is_weak = at.weak;   /* leading __attribute__((weak)) */
    f->is_noreturn = at.noreturn;
    f->ret_ty = ty;
    f->name = name;
    f->file = ps->lx.file;
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

    /* trailing attributes: void f(void) __attribute__((noreturn/weak)) */
    parse_attributes(ps, &at);
    f->is_weak = at.weak;

    if (cur(ps)->kind == TOK_SEMI) {
        advance(ps); /* prototype */
    } else if (cur(ps)->kind == TOK_COMMA) {
        /* The first declarator was a function PROTOTYPE and more declarators
         * share the same base type: `extern double f(double), g(double), x;`.
         * A comma can only follow a prototype, never a definition. */
        **ftail = f;
        *ftail = &f->next;
        while (cur(ps)->kind == TOK_COMMA) {
            advance(ps);
            struct type *dty = parse_stars(ps, base);
            if (cur(ps)->kind != TOK_IDENT)
                diag_fatal(ps->lx.file, cur(ps)->line,
                           "expected a name before %s", tok_describe(cur(ps)));
            const char *dname = cur(ps)->text;
            int dline = cur(ps)->line;
            advance(ps);
            if (cur(ps)->kind == TOK_LPAREN) {
                /* a sibling function prototype: `g(double)` */
                struct type *fty = parse_fn_params(ps, dty);
                struct func *g = xcalloc(1, sizeof *g);
                g->is_static = is_static;
                g->ret_ty = fty->ret;
                g->name = dname;
                g->file = ps->lx.file;
                g->line = dline;
                g->seq = seq;
                g->nparams = fty->nptypes;
                for (int i = 0; i < fty->nptypes; i++) {
                    g->param_tys[i] = fty->ptypes[i];
                    g->params[i] = NULL;  /* unnamed prototype parameters */
                }
                g->is_varargs = fty->is_varargs;
                **ftail = g;
                *ftail = &g->next;
            } else {
                /* a sibling variable: `x`, `*p`, `a[10]`, with optional init */
                struct type *vty = parse_array_dims(ps, dty);
                parse_attributes(ps, &at);
                struct global *g = parse_global(ps, vty, dname, dline,
                                                is_static, is_extern);
                parse_attributes(ps, &at);
                g->is_weak = at.weak;
                g->seq = seq;
                g->def_seq = seq;
                **gtail = g;
                *gtail = &g->next;
            }
        }
        expect(ps, TOK_SEMI, "';'");
        (void)u;
        return;
    } else {
        if (cur(ps)->kind != TOK_LBRACE)
            diag_fatal(ps->lx.file, cur(ps)->line,
                       "expected '{' or ';' before %s",
                       tok_describe(cur(ps)));
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
    g_fold_unit = u;          /* size_fold resolves this unit's enum constants */
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
    /* A translation unit of only data (a table of globals, no functions) is
     * valid C — EmbCC's own predef macro table is exactly that. An entirely
     * empty unit is legal too (a header-only .c, a fully #if'd-out file);
     * it yields a valid, empty object. */
    return u;
}
