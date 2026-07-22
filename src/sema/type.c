#include "type.h"

#include <stdio.h>
#include <string.h>

#include "../driver/util.h"

/* [kind][is_unsigned] — TY_PTR handled separately */
static struct type bases[5][2] = {
    { { TY_VOID, 0, 0 }, { TY_VOID, 0, 0 } },
    { { TY_CHAR, 0, 0 }, { TY_CHAR, 1, 0 } },
    { { TY_SHORT, 0, 0 }, { TY_SHORT, 1, 0 } },
    { { TY_INT, 0, 0 }, { TY_INT, 1, 0 } },
    { { TY_LONG, 0, 0 }, { TY_LONG, 1, 0 } },
};

struct type *ty_base(enum ty_kind kind, int is_unsigned)
{
    return &bases[kind][is_unsigned ? 1 : 0];
}

struct type *ty_ptr(struct type *pointee)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_PTR;
    t->pointee = pointee;
    return t;
}

int ty_size(const struct type *t)
{
    switch (t->kind) {
    case TY_CHAR: return 1;
    case TY_SHORT: return 2;
    case TY_INT: return 4;
    case TY_LONG: return 8;
    case TY_PTR: return 8;
    case TY_VOID: break;
    }
    return 0;
}

int ty_equal(const struct type *a, const struct type *b)
{
    if (a->kind != b->kind || a->is_unsigned != b->is_unsigned)
        return 0;
    if (a->kind == TY_PTR)
        return ty_equal(a->pointee, b->pointee);
    return 1;
}

int ty_is_integer(const struct type *t)
{
    return t->kind == TY_CHAR || t->kind == TY_SHORT ||
           t->kind == TY_INT || t->kind == TY_LONG;
}

int ty_is_scalar(const struct type *t)
{
    return ty_is_integer(t) || t->kind == TY_PTR;
}

int ty_wide(const struct type *t)
{
    return t->kind == TY_LONG || t->kind == TY_PTR;
}

int ty_signed_int(const struct type *t)
{
    return ty_is_integer(t) && !t->is_unsigned;
}

const char *ty_name(const struct type *t)
{
    static char buf[64];
    const char *base;
    int stars = 0;

    while (t->kind == TY_PTR) {
        stars++;
        t = t->pointee;
    }
    switch (t->kind) {
    case TY_VOID: base = "void"; break;
    case TY_CHAR: base = t->is_unsigned ? "unsigned char" : "char"; break;
    case TY_SHORT: base = t->is_unsigned ? "unsigned short" : "short"; break;
    case TY_INT: base = t->is_unsigned ? "unsigned int" : "int"; break;
    case TY_LONG: base = t->is_unsigned ? "unsigned long" : "long"; break;
    default: base = "?"; break;
    }
    int n = snprintf(buf, sizeof buf, "%s ", base);
    for (int i = 0; i < stars && n < (int)sizeof buf - 1; i++)
        buf[n++] = '*';
    if (!stars)
        buf[n - 1] = 0; /* drop the trailing space */
    else
        buf[n] = 0;
    return buf;
}
