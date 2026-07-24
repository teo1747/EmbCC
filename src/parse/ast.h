/* AST for the M1 subset.
 *
 * The subset, stated so nothing outside it is implied: functions of int
 * taking int parameters (at most 6 — the SysV register args; more is a
 * loud error until someone needs stack args), local int declarations
 * with optional initializer, return, +, -, *, parentheses, integer
 * constants, and calls to functions defined in the same file. Everything
 * else is rejected at parse or sema with a diagnostic naming it.
 */
#ifndef EMBCC_PARSE_AST_H
#define EMBCC_PARSE_AST_H

#include "../sema/type.h"

/* Prototypes in real headers declare more parameters than fit in
 * registers (newlib's _dtoa_r takes seven), so the DECLARATION limit is
 * generous; what is actually refused is a CALL needing more registers
 * than SysV provides — checked in sema, where the classes are known. */
#define MAX_PARAMS 12

enum expr_kind { EXPR_NUM, EXPR_FNUM, EXPR_STR, EXPR_VAR, EXPR_BINOP, EXPR_CALL,
                 EXPR_ASSIGN, EXPR_NOT, EXPR_NEG, EXPR_BNOT, EXPR_INCDEC,
                 EXPR_DEREF, EXPR_ADDR, EXPR_CAST, EXPR_SIZEOF,
                 EXPR_MEMBER, EXPR_COND, EXPR_COMMA,
                 EXPR_COMPOUND, EXPR_INITLIST };

/* B_LAND/B_LOR are short-circuit: irgen lowers them to branches, they
 * never reach codegen as plain binops. Comparisons yield 0/1 ints.
 * Division/modulo truncate toward zero and >> is arithmetic — int is
 * the only type, and signed is what idiv/sar give. */
enum binop {
    B_ADD, B_SUB, B_MUL, B_DIV, B_MOD,
    B_AND, B_OR, B_XOR, B_SHL, B_SHR,
    B_EQ, B_NE, B_LT, B_LE, B_GT, B_GE,
    B_LAND, B_LOR
};

struct expr {
    enum expr_kind kind;
    int line;
    struct type *ty;      /* set by sema on every node */
    struct type *undecayed; /* sema: original array type when ty is the
                             * decayed pointer (sizeof needs it) */
    long num;             /* EXPR_NUM; EXPR_STR: byte length incl NUL */
    double fnum;          /* EXPR_FNUM */
    const char *name;     /* EXPR_VAR, EXPR_CALL, EXPR_INCDEC target;
                           * EXPR_STR: the bytes */
    int var_index;        /* EXPR_VAR/EXPR_INCDEC: slot; set by sema */
    struct global *gref;  /* EXPR_VAR/EXPR_INCDEC: the global, when the
                           * name is not a local (sema) */
    struct func *fref;    /* EXPR_VAR: a function used as a value —
                           * decays to pointer-to-function (sema) */
    int str_index;        /* EXPR_STR: unit string table slot (irgen) */
    enum binop op;        /* EXPR_BINOP */
    struct expr *lhs, *rhs; /* BINOP + ASSIGN(lhs=target);
                             * NOT/NEG/BNOT/DEREF/ADDR/CAST use rhs only */
    struct type *cast_ty; /* EXPR_CAST target; EXPR_SIZEOF(type) */
    int is_post, delta;   /* EXPR_INCDEC: x++/x-- vs ++x/--x, +1/-1 */
    /* EXPR_MEMBER: lhs is the base, name the member; is_arrow for ->.
     * memb is resolved by sema (offset + type). */
    int is_arrow;
    struct member *memb;
    struct expr *args[MAX_PARAMS]; /* EXPR_CALL; lhs is the callee
                           * expression (a VAR for direct calls) */
    int nargs;
    struct func *callee;  /* EXPR_CALL: direct target (sema), or NULL
                           * for a call through a function pointer */
    struct expr **elems;  /* EXPR_INITLIST */
    int nelems;
};

/* An aggregate initializer, flattened by sema into (offset, type,
 * value) triples so irgen never has to re-walk the type. */
struct initelem {
    int off;
    struct type *ty;
    struct expr *e;
};

enum stmt_kind { STMT_RETURN, STMT_DECL, STMT_EXPR, STMT_IF, STMT_WHILE,
                 STMT_FOR, STMT_BLOCK, STMT_BREAK, STMT_CONTINUE,
                 STMT_DO, STMT_SWITCH, STMT_CASE, STMT_DEFAULT };

struct stmt {
    enum stmt_kind kind;
    int line;
    const char *name;     /* STMT_DECL */
    struct type *dty;     /* STMT_DECL: declared type */
    int is_static;        /* STMT_DECL: a static local -> its own global */
    struct initelem *inits; /* STMT_DECL: flattened aggregate init */
    int ninits;
    const char *sbytes;   /* STMT_DECL: a static local's constant bytes */
    struct global *sglob; /* STMT_DECL: the global a static local became */
    int var_index;        /* STMT_DECL: set by sema */
    struct expr *expr;    /* RETURN/EXPR value; DECL initializer (or NULL) */
    struct expr *cond;    /* IF/WHILE/FOR */
    struct expr *init, *step; /* FOR: either may be NULL */
    struct stmt *initdecl;    /* FOR: `for (int i = 0; ...)` */
    struct stmt *thn, *els;   /* IF: els may be NULL */
    struct stmt *body;    /* WHILE/FOR: the controlled statement;
                           * BLOCK: the child list */
    struct stmt *next;
    /* STMT_CASE: the label's constant value, folded by sema. Case and
     * default are position MARKERS in the switch body's statement list —
     * C's fallthrough means they cannot be nested nodes. */
    long cval;
    int label;            /* irgen: the marker's label id */
};

/* A file-scope variable. Like functions, later declarations merge into
 * the first (canonical) node; `int g;` is a definition (the tentative-
 * definition subtlety is collapsed — strictly simpler than C, and any
 * program we accept means the same thing to gcc). */
struct global {
    const char *name;
    const char *file;     /* where THIS declaration was written — a
                           * header, usually, and not the unit's name */
    int line;
    int seq;              /* source order, shared counter with funcs —
                           * enforces declare-before-use across kinds */
    struct type *ty;
    int is_static;
    int is_extern;        /* THIS declaration was 'extern' */
    int has_init;
    long init;            /* constant initializer value */
    const char *init_bytes; /* string initializer (char arrays) */
    int init_len;
    struct global *next;

    int defined;          /* sema, canonical: some declaration defines it */
    int absorbed;         /* sema: merged into an earlier node */
    int used;
    int in_bss;           /* driver: zero-valued -> .bss, else .data */
    int off;              /* driver: offset inside its section */
    int sym_ndx;          /* driver: symbol index */
};

struct func {
    const char *name;
    const char *file;     /* see struct global */
    int line;
    int seq;              /* source order (see struct global) */
    int is_static;
    int is_varargs;       /* declared with a trailing ", ..." */
    struct type *ret_ty;
    int nparams;
    const char *params[MAX_PARAMS]; /* names; NULL in unnamed prototypes */
    struct type *param_tys[MAX_PARAMS];
    struct type **var_tys;          /* sema: type of every var slot */
    struct stmt *body;
    int defined;          /* parse: THIS node syntactically had a body
                           * (may be NULL even so: "{ }" — sema rejects
                           * it for missing return like any other path) */
    int has_defn;         /* sema, canonical node: a definition exists
                           * somewhere in the unit */
    struct func *next;    /* unit list, source order */

    int nvars;            /* params + locals; set by sema */
    int declared;         /* sema: declaration has been reached */
    int absorbed;         /* sema: merged into an earlier node — skip */
    int used;             /* sema: at least one call resolves here */
    /* codegen bookkeeping: position inside .text (defined funcs only) */
    int code_off, code_len;
    int sym_ndx;          /* driver: symbol index (defined or UNDEF) */
};

/* An enumerator: a named int constant at file scope. */
struct econst {
    const char *name;
    long val;
    int seq;
    struct econst *next;
};

struct unit {
    const char *file;
    struct func *funcs;
    struct global *globals;
    struct econst *econsts;
};

#endif
