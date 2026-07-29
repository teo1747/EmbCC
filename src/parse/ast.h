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

/* MAX_PARAMS (the declaration/call arity cap) is defined in type.h, which
 * this header includes, so struct type's ptypes[] and the AST arrays here
 * stay the same size. */

enum expr_kind { EXPR_NUM, EXPR_FNUM, EXPR_STR, EXPR_VAR, EXPR_BINOP, EXPR_CALL,
                 EXPR_ASSIGN, EXPR_NOT, EXPR_NEG, EXPR_BNOT, EXPR_INCDEC,
                 EXPR_DEREF, EXPR_ADDR, EXPR_CAST, EXPR_SIZEOF,
                 EXPR_MEMBER, EXPR_COND, EXPR_COMMA,
                 EXPR_COMPOUND, EXPR_INITLIST, EXPR_VA_ARG, EXPR_COMPLIT,
                 EXPR_GENERIC, EXPR_STMTEXPR };

struct stmt;   /* a statement expression `({ ... })` carries a block */

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
    /* EXPR_COMPLIT: `(type){ init }`. cast_ty is the type, lhs the
     * EXPR_INITLIST; sema allocates var_index (a synthesized local) and
     * flattens the initializer into inits/ninits for irgen to place. */
    struct initelem *inits;
    int ninits;
    /* EXPR_GENERIC: `_Generic(lhs, T1: e1, ..., default: eN)`. gtypes[i] is
     * an association's type (NULL for the `default` case) and gexprs[i] its
     * expression; sema picks the one matching lhs's type and becomes it. */
    struct type **gtypes;
    struct expr **gexprs;
    int ngen;
    /* EXPR_STMTEXPR: the `({ ... })` block; its value is the last statement
     * when that is an expression statement, else void. */
    struct stmt *body;
    const char *desig_field; /* an initlist element's .field designator,
                              * NULL when it is positional */
    int desig_index;         /* an initlist element's [index] designator,
                              * -1 when it is positional */
    int desig_index_hi;      /* GNU range `[lo ... hi]`: the high index, else
                              * -1 (a plain `[index]` or positional element) */
    const char *asm_reg;  /* EXPR_VAR: a register-asm binding propagated
                           * from the variable's declaration, else NULL */
};

/* An aggregate initializer, flattened by sema into (offset, type,
 * value) triples so irgen never has to re-walk the type. A bitfield leaf
 * additionally carries its position within the storage unit at `off`
 * (bit_width 0 means an ordinary, non-bitfield leaf). */
struct initelem {
    int off;
    struct type *ty;
    struct expr *e;
    int bit_off;
    int bit_width;
};

/* A relocation inside a static object's byte image: a pointer-typed slot
 * whose value is an address the linker fills in — a string literal, an
 * &global, or a function address (a vtable of function pointers). */
struct greloc {
    int off;              /* byte offset within the object */
    const char *str;      /* a string-literal target (NULL if a global/func) */
    int str_len;          /* including its NUL */
    int str_off;          /* driver: the target's offset inside .rodata */
    struct global *gtarget; /* an &global target, else NULL */
    struct func *ftarget; /* a function-address target, else NULL */
    long addend;
};

enum stmt_kind { STMT_RETURN, STMT_DECL, STMT_EXPR, STMT_IF, STMT_WHILE,
                 STMT_FOR, STMT_BLOCK, STMT_BREAK, STMT_CONTINUE,
                 STMT_DO, STMT_SWITCH, STMT_CASE, STMT_DEFAULT, STMT_ASM,
                 STMT_LABEL, STMT_GOTO };

/* One operand of an extended-asm statement: a constraint string and the C
 * expression it binds. Output constraints begin with '=' (or '+') and name
 * an lvalue; input constraints name any expression. EmbCC supports the
 * fixed-register letters a/b/c/d/S/D and 'r' (bound via a register-asm
 * variable) — enough for EmbLinkOS's int-$0x80 syscall stubs. */
struct asm_operand {
    const char *name;    /* a `[name]` symbolic operand, referenced as %[name] */
    const char *constraint;
    struct expr *expr;
    int reg;              /* the fixed register (0-15), resolved by sema */
};

/* An extended-asm statement (VISION_LONGTERM: first-class fixed-register
 * constraints, so the syscall header's gcc branch compiles). The template
 * is assembled by a tiny fixed vocabulary (int $imm today); clobbers are
 * parsed and ignored, sound because EmbCC keeps every value in a stack
 * slot, never a register, across statements. */
struct asm_stmt {
    const char *tmpl;
    struct asm_operand *out;
    int nout;
    struct asm_operand *in;
    int nin;
    /* Clobbered registers (e.g. "rdx"), kept so the operand allocator can
     * EXCLUDE them — an allocatable "r" operand must never land in a register
     * the template destroys. "cc"/"memory" are stored too and simply don't
     * name a GPR. */
    const char **clob;
    int nclob;
    int is_volatile;
};

struct stmt {
    enum stmt_kind kind;
    int line;
    const char *name;     /* STMT_DECL */
    struct type *dty;     /* STMT_DECL: declared type */
    int is_static;        /* STMT_DECL: a static local -> its own global */
    int is_extern;        /* STMT_DECL: block-scope extern -> a unit global/func */
    struct initelem *inits; /* STMT_DECL: flattened aggregate init */
    int ninits;
    struct global *sglob; /* STMT_DECL: the global a static local became */
    int var_index;        /* STMT_DECL: set by sema */
    const char *asm_reg;  /* STMT_DECL: a register-asm binding, `register T
                           * x __asm__("r10")` — NULL for an ordinary local */
    int user_align;       /* STMT_DECL: __attribute__((aligned(N))); 0 = none */
    struct asm_stmt *asm_s; /* STMT_ASM */
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
    int def_seq;          /* source order of the DEFINING declaration (the
                           * one with the initializer) — its initializer may
                           * reference names declared before it, not merely
                           * before the first (extern) declaration */
    struct type *ty;
    int is_static;
    int is_extern;        /* THIS declaration was 'extern' */
    int is_weak;          /* __attribute__((weak)) */
    int has_init;
    long init;            /* constant initializer value (scalar) */
    struct expr *init_expr; /* aggregate/relocatable initializer, lowered
                             * by sema into init_bytes + relocs */
    const char *init_bytes; /* the constant byte image (string or aggregate) */
    int init_len;
    struct greloc *relocs;  /* pointer slots the linker resolves */
    int nrelocs;
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
    int is_weak;          /* __attribute__((weak)) */
    int is_noreturn;      /* __attribute__((noreturn)) / _Noreturn */
    int is_varargs;       /* declared with a trailing ", ..." */
    struct type *ret_ty;
    int nparams;
    const char *params[MAX_PARAMS]; /* names; NULL in unnamed prototypes */
    struct type *param_tys[MAX_PARAMS];
    struct type **var_tys;          /* sema: type of every var slot */
    int *var_aligns;                /* sema: __attribute__((aligned(N))) per
                                     * var slot (0 = natural); parallels var_tys */
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

/* A label a file-scope asm block defines, and a relocation it needs. */
struct asmsym {
    const char *name;
    int off;             /* offset within .text (filled at emission) */
    int is_global;       /* named by .global/.globl */
};
struct asmrel {
    int off;             /* offset within .text of the rel32 field */
    const char *target;  /* symbol the call/jmp resolves to */
    long addend;
};

/* A file-scope `__asm__("...")` block (crt0's _start stub, and its kind).
 * Assembled by a tiny fixed vocabulary — .global/.globl, labels, and
 * $imm/call/jmp/ret — into .text bytes with symbols and relocations. */
struct topasm {
    const char *tmpl;
    const char *file;
    int line;
    unsigned char *code;
    int codelen;
    struct asmsym *syms;
    int nsyms;
    struct asmrel *rels;
    int nrels;
    int text_off;        /* where the bytes landed in .text (driver) */
    struct topasm *next;
};

struct unit {
    const char *file;
    struct func *funcs;
    struct global *globals;
    struct econst *econsts;
    struct topasm *topasm;
};

/* Element count an EXPR_INITLIST implies for an unsized array, honoring
 * `[i] =` designators (defined in parse.c, used there and in sema). */
int initlist_array_count(const struct expr *il);

#endif
