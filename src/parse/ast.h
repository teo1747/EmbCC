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

#define MAX_PARAMS 6

enum expr_kind { EXPR_NUM, EXPR_VAR, EXPR_BINOP, EXPR_CALL };

struct expr {
    enum expr_kind kind;
    int line;
    long num;             /* EXPR_NUM */
    const char *name;     /* EXPR_VAR, EXPR_CALL */
    int var_index;        /* EXPR_VAR: slot in the function; set by sema */
    int op;               /* EXPR_BINOP: '+', '-', '*' */
    struct expr *lhs, *rhs;
    struct expr *args[MAX_PARAMS]; /* EXPR_CALL */
    int nargs;
    struct func *callee;  /* EXPR_CALL: resolved by sema */
};

enum stmt_kind { STMT_RETURN, STMT_DECL };

struct stmt {
    enum stmt_kind kind;
    int line;
    const char *name;     /* STMT_DECL */
    int var_index;        /* STMT_DECL: set by sema */
    struct expr *expr;    /* return value / initializer (DECL: may be NULL) */
    struct stmt *next;
};

struct func {
    const char *name;
    int line;
    int is_static;
    int nparams;
    const char *params[MAX_PARAMS];
    struct stmt *body;
    struct func *next;    /* unit list, source order */

    int nvars;            /* params + locals; set by sema */
    int declared;         /* sema: definition has been reached */
    /* codegen bookkeeping: position inside .text */
    int code_off, code_len;
};

struct unit {
    const char *file;
    struct func *funcs;
};

#endif
