/* The type representation — the single most load-bearing structure in
 * the compiler (every phase downstream takes its shape from it).
 *
 * Types I scope: void, the integer types char/short/int/long with
 * unsigned variants, and pointers. LP64 throughout (TARGET_ABI): long
 * and pointers are 8 bytes. Arrays, structs, enums, and function types
 * are later M2 increments — code that would need them must refuse.
 *
 * Value representation contract (codegen relies on it): expression
 * temporaries always hold PROMOTED values — 32-bit class for
 * char/short/int (C's integer promotions do this anyway) or 64-bit
 * class for long/pointers. Only variables in memory have the narrow
 * widths; loads extend, stores truncate.
 */
#ifndef EMBCC_SEMA_TYPE_H
#define EMBCC_SEMA_TYPE_H

enum ty_kind { TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_PTR };

struct type {
    enum ty_kind kind;
    int is_unsigned;        /* integers only */
    struct type *pointee;   /* TY_PTR only */
};

/* Base types are interned singletons — pointer equality works for
 * them; ty_equal() works for everything. */
struct type *ty_base(enum ty_kind kind, int is_unsigned);
struct type *ty_ptr(struct type *pointee);

int ty_size(const struct type *t);          /* bytes; void has none */
int ty_equal(const struct type *a, const struct type *b);
int ty_is_integer(const struct type *t);
int ty_is_scalar(const struct type *t);     /* integer or pointer */
int ty_wide(const struct type *t);          /* 1 = 64-bit value class */
int ty_signed_int(const struct type *t);    /* signed integer? */

/* Diagnostic spelling, e.g. "unsigned char **". Static buffer. */
const char *ty_name(const struct type *t);

#endif
