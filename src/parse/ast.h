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

#define MAX_PARAMS 6

enum expr_kind { EXPR_NUM, EXPR_VAR, EXPR_BINOP, EXPR_CALL, EXPR_ASSIGN,
                 EXPR_NOT, EXPR_NEG, EXPR_BNOT, EXPR_INCDEC,
                 EXPR_DEREF, EXPR_ADDR, EXPR_CAST, EXPR_SIZEOF };

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
    long num;             /* EXPR_NUM */
    const char *name;     /* EXPR_VAR, EXPR_CALL, EXPR_INCDEC target */
    int var_index;        /* EXPR_VAR/EXPR_INCDEC: slot; set by sema */
    enum binop op;        /* EXPR_BINOP */
    struct expr *lhs, *rhs; /* BINOP + ASSIGN(lhs=target);
                             * NOT/NEG/BNOT/DEREF/ADDR/CAST use rhs only */
    struct type *cast_ty; /* EXPR_CAST target; EXPR_SIZEOF(type) */
    int is_post, delta;   /* EXPR_INCDEC: x++/x-- vs ++x/--x, +1/-1 */
    struct expr *args[MAX_PARAMS]; /* EXPR_CALL */
    int nargs;
    struct func *callee;  /* EXPR_CALL: resolved by sema */
};

enum stmt_kind { STMT_RETURN, STMT_DECL, STMT_EXPR, STMT_IF, STMT_WHILE,
                 STMT_FOR, STMT_BLOCK, STMT_BREAK, STMT_CONTINUE };

struct stmt {
    enum stmt_kind kind;
    int line;
    const char *name;     /* STMT_DECL */
    struct type *dty;     /* STMT_DECL: declared type */
    int var_index;        /* STMT_DECL: set by sema */
    struct expr *expr;    /* RETURN/EXPR value; DECL initializer (or NULL) */
    struct expr *cond;    /* IF/WHILE/FOR */
    struct expr *init, *step; /* FOR: either may be NULL */
    struct stmt *thn, *els;   /* IF: els may be NULL */
    struct stmt *body;    /* WHILE/FOR: the controlled statement;
                           * BLOCK: the child list */
    struct stmt *next;
};

struct func {
    const char *name;
    int line;
    int is_static;
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
    int sym_ndx;          /* driver: UNDEF symbol index (externals only) */
};

struct unit {
    const char *file;
    struct func *funcs;
};

#endif
