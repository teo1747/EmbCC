#include "type.h"

#include <stdio.h>
#include <string.h>

#include "../driver/util.h"

/* [kind][is_unsigned] — TY_PTR/TY_ARRAY/TY_STRUCT handled separately.
 * Designated initializers so this table survives struct type growing. */
static struct type bases[5][2] = {
    { { .kind = TY_VOID }, { .kind = TY_VOID } },
    { { .kind = TY_CHAR }, { .kind = TY_CHAR, .is_unsigned = 1 } },
    { { .kind = TY_SHORT }, { .kind = TY_SHORT, .is_unsigned = 1 } },
    { { .kind = TY_INT }, { .kind = TY_INT, .is_unsigned = 1 } },
    { { .kind = TY_LONG }, { .kind = TY_LONG, .is_unsigned = 1 } },
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

struct type *ty_array(struct type *elem, int count)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_ARRAY;
    t->pointee = elem;
    t->count = count;
    return t;
}

struct type *ty_struct(const char *tag, int is_union)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_STRUCT;
    t->tag = tag;
    t->is_union = is_union;
    return t;
}

void ty_struct_layout(struct type *t, struct member *members, int n)
{
    int off = 0, align = 1;

    for (int i = 0; i < n; i++) {
        int ma = ty_align(members[i].ty);
        int ms = ty_size(members[i].ty);
        if (t->is_union) {
            members[i].off = 0;
            if (ms > off)
                off = ms;
        } else {
            off = (off + ma - 1) & ~(ma - 1);
            members[i].off = off;
            off += ms;
        }
        if (ma > align)
            align = ma;
    }
    t->members = members;
    t->nmembers = n;
    t->align = align;
    t->size = (off + align - 1) & ~(align - 1);
    t->complete = 1;
}

struct member *ty_find_member(struct type *t, const char *name)
{
    for (int i = 0; i < t->nmembers; i++)
        if (strcmp(t->members[i].name, name) == 0)
            return &t->members[i];
    return NULL;
}

int ty_size(const struct type *t)
{
    switch (t->kind) {
    case TY_CHAR: return 1;
    case TY_SHORT: return 2;
    case TY_INT: return 4;
    case TY_LONG: return 8;
    case TY_PTR: return 8;
    case TY_ARRAY: return t->count * ty_size(t->pointee);
    case TY_STRUCT: return t->size; /* 0 while incomplete */
    case TY_VOID: break;
    }
    return 0;
}

int ty_align(const struct type *t)
{
    switch (t->kind) {
    case TY_ARRAY: return ty_align(t->pointee);
    case TY_STRUCT: return t->complete ? t->align : 1;
    default: return ty_size(t);
    }
}

int ty_equal(const struct type *a, const struct type *b)
{
    if (a->kind != b->kind || a->is_unsigned != b->is_unsigned)
        return 0;
    if (a->kind == TY_PTR)
        return ty_equal(a->pointee, b->pointee);
    if (a->kind == TY_ARRAY)
        return a->count == b->count && ty_equal(a->pointee, b->pointee);
    if (a->kind == TY_STRUCT)
        return a == b; /* one node per tag: identity is equality */
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
    /* Rotating buffers so one diagnostic can name two types — with a
     * single buffer "cannot convert char* to int" printed the SAME
     * spelling twice (found by a refusal test, of course). */
    static char bufs[4][64];
    static int which;
    char *buf = bufs[which];
    size_t bufsz = sizeof bufs[0];
    which = (which + 1) & 3;
    const char *base;
    int stars = 0;
    int dims[4];
    int ndims = 0;

    while (t->kind == TY_PTR || t->kind == TY_ARRAY) {
        if (t->kind == TY_PTR) {
            stars++;
        } else {
            if (ndims < 4)
                dims[ndims] = t->count;
            ndims++;
        }
        t = t->pointee;
    }
    char structbuf[48];
    switch (t->kind) {
    case TY_VOID: base = "void"; break;
    case TY_CHAR: base = t->is_unsigned ? "unsigned char" : "char"; break;
    case TY_SHORT: base = t->is_unsigned ? "unsigned short" : "short"; break;
    case TY_INT: base = t->is_unsigned ? "unsigned int" : "int"; break;
    case TY_LONG: base = t->is_unsigned ? "unsigned long" : "long"; break;
    case TY_STRUCT:
        snprintf(structbuf, sizeof structbuf, "%s %s",
                 t->is_union ? "union" : "struct",
                 t->tag ? t->tag : "<anonymous>");
        base = structbuf;
        break;
    default: base = "?"; break;
    }
    int n = snprintf(buf, bufsz, "%s", base);
    if (stars) {
        buf[n++] = ' ';
        for (int i = 0; i < stars && n < (int)bufsz - 8; i++)
            buf[n++] = '*';
    }
    for (int i = 0; i < ndims && i < 4 && n < (int)bufsz - 16; i++)
        n += snprintf(buf + n, bufsz - (size_t)n, "[%d]", dims[i]);
    buf[n] = 0;
    return buf;
}
