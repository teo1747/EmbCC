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

enum expr_kind { EXPR_NUM, EXPR_VAR, EXPR_BINOP, EXPR_CALL, EXPR_ASSIGN,
                 EXPR_NOT, EXPR_NEG, EXPR_BNOT, EXPR_INCDEC };

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
    long num;             /* EXPR_NUM */
    const char *name;     /* EXPR_VAR, EXPR_CALL, EXPR_ASSIGN target */
    int var_index;        /* EXPR_VAR/EXPR_ASSIGN: slot; set by sema */
    enum binop op;        /* EXPR_BINOP */
    struct expr *lhs, *rhs; /* BINOP; NOT/NEG/BNOT/ASSIGN use rhs only */
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
