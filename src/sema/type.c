#include "type.h"

#include <stdio.h>
#include <string.h>

#include "../driver/util.h"

/* [kind][is_unsigned] — TY_PTR/TY_ARRAY/TY_STRUCT handled separately.
 * Designated initializers so this table survives struct type growing.
 * _Bool is unsigned in both slots (it holds only 0 or 1). */
static struct type bases[8][2] = {
    { { .kind = TY_VOID }, { .kind = TY_VOID } },
    { { .kind = TY_BOOL, .is_unsigned = 1 },
      { .kind = TY_BOOL, .is_unsigned = 1 } },
    { { .kind = TY_CHAR }, { .kind = TY_CHAR, .is_unsigned = 1 } },
    { { .kind = TY_SHORT }, { .kind = TY_SHORT, .is_unsigned = 1 } },
    { { .kind = TY_INT }, { .kind = TY_INT, .is_unsigned = 1 } },
    { { .kind = TY_LONG }, { .kind = TY_LONG, .is_unsigned = 1 } },
    { { .kind = TY_FLOAT }, { .kind = TY_FLOAT } },   /* never unsigned */
    { { .kind = TY_DOUBLE }, { .kind = TY_DOUBLE } },
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

struct type *ty_func(struct type *ret, struct type **ptypes, int n,
                     int is_varargs)
{
    struct type *t = xcalloc(1, sizeof *t);
    t->kind = TY_FUNC;
    t->ret = ret;
    for (int i = 0; i < n; i++)
        t->ptypes[i] = ptypes[i];
    t->nptypes = n;
    t->is_varargs = is_varargs;
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

/* `packed` drops every member's alignment to 1 (no inter-member padding,
 * struct align 1); `user_align`, from __attribute__((aligned(N))), raises
 * the struct's alignment to at least N. */
void ty_struct_layout(struct type *t, struct member *members, int n,
                      int packed, int user_align)
{
    int align = 1;
    /* Non-bitfields track a byte offset; bitfields a bit position. The two
     * share one running cursor kept in bits (bitpos), rounded up to a byte
     * when a plain member intervenes — this is the little-endian gcc layout
     * (a field never crosses a boundary of its declared type). */
    int bitpos = 0;   /* bits from the struct start; unions ignore it */
    int umax = 0;     /* union: largest member extent, in bytes */

    for (int i = 0; i < n; i++) {
        struct member *m = &members[i];
        int ma = packed ? 1 : ty_align(m->ty);
        /* An explicit __attribute__((aligned(N))) on the member raises its
         * alignment (and, through `align` below, the struct's) — it overrides
         * even `packed`, which only lowers the *default* alignment. */
        if (m->user_align > ma)
            ma = m->user_align;

        if (m->is_bitfield) {
            int unit = 8 * ty_size(m->ty);   /* storage-unit width, bits */
            if (t->is_union) {
                m->off = 0;
                m->bit_off = 0;
                int ext = (m->bit_width + 7) / 8;
                if (ext > umax) umax = ext;
            } else if (m->bit_width == 0) {
                /* a zero-width field rounds up to the next unit boundary and
                 * names nothing — a separator, never stored or accessed. */
                if (!packed)
                    bitpos = (bitpos + unit - 1) / unit * unit;
                m->off = bitpos / 8;
                m->bit_off = 0;
            } else {
                /* keep the field within one storage unit of its type */
                if (!packed &&
                    bitpos / unit != (bitpos + m->bit_width - 1) / unit)
                    bitpos = (bitpos + unit - 1) / unit * unit;
                m->off = (bitpos / unit) * ty_size(m->ty);
                m->bit_off = bitpos - m->off * 8;
                bitpos += m->bit_width;
            }
        } else {
            int ms = ty_size(m->ty);
            if (t->is_union) {
                m->off = 0;
                if (ms > umax) umax = ms;
            } else {
                int bytepos = (bitpos + 7) / 8;      /* leave any open unit */
                bytepos = (bytepos + ma - 1) & ~(ma - 1);
                m->off = bytepos;
                bitpos = (bytepos + ms) * 8;
            }
        }
        if (ma > align)
            align = ma;
    }
    if (user_align > align)
        align = user_align;
    int bytes = t->is_union ? umax : (bitpos + 7) / 8;
    t->members = members;
    t->nmembers = n;
    t->align = align;
    t->size = (bytes + align - 1) & ~(align - 1);
    t->complete = 1;
}

struct member *ty_find_member(struct type *t, const char *name)
{
    for (int i = 0; i < t->nmembers; i++)
        if (t->members[i].name && strcmp(t->members[i].name, name) == 0)
            return &t->members[i];
    return NULL;
}

int ty_size(const struct type *t)
{
    switch (t->kind) {
    case TY_BOOL: return 1;
    case TY_CHAR: return 1;
    case TY_SHORT: return 2;
    case TY_INT: return 4;
    case TY_LONG: return 8;
    case TY_FLOAT: return 4;
    case TY_DOUBLE: return 8;
    case TY_PTR: return 8;
    case TY_ARRAY: return t->count * ty_size(t->pointee);
    case TY_STRUCT: return t->size; /* 0 while incomplete */
    case TY_FUNC: break;            /* no size; only pointers to it */
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
    if (a->kind == TY_FUNC) {
        if (a->nptypes != b->nptypes || a->is_varargs != b->is_varargs ||
            !ty_equal(a->ret, b->ret))
            return 0;
        for (int i = 0; i < a->nptypes; i++)
            if (!ty_equal(a->ptypes[i], b->ptypes[i]))
                return 0;
        return 1;
    }
    return 1;
}

int ty_is_integer(const struct type *t)
{
    return t->kind == TY_BOOL || t->kind == TY_CHAR ||
           t->kind == TY_SHORT || t->kind == TY_INT || t->kind == TY_LONG;
}

int ty_is_float(const struct type *t)
{
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE;
}

int ty_is_arith(const struct type *t)
{
    return ty_is_integer(t) || ty_is_float(t);
}

int ty_is_scalar(const struct type *t)
{
    return ty_is_arith(t) || t->kind == TY_PTR;
}

/* 64-bit value class. Floats have their own register file, so this
 * answers width only — never "which register bank". */
int ty_wide(const struct type *t)
{
    return t->kind == TY_LONG || t->kind == TY_PTR ||
           t->kind == TY_DOUBLE;
}

int ty_signed_int(const struct type *t)
{
    return ty_is_integer(t) && !t->is_unsigned;
}

/* Merge rule for two scalars landing in the same eightbyte: anything
 * non-floating makes the whole eightbyte INTEGER. */
static void class_merge(enum arg_class *slot, int *seen, enum arg_class c)
{
    if (!*seen) {
        *slot = c;
        *seen = 1;
        return;
    }
    if (c == CLASS_INTEGER)
        *slot = CLASS_INTEGER;
}

/* Walks every scalar leaf of t at byte offset `off`, classifying the
 * eightbyte each one falls in. Arrays and nested structs recurse, which
 * is what makes "all floating" mean all the way down. */
static void classify_fields(const struct type *t, int off,
                            enum arg_class *cls, int *seen)
{
    if (t->kind == TY_STRUCT) {
        for (int i = 0; i < t->nmembers; i++)
            classify_fields(t->members[i].ty, off + t->members[i].off,
                            cls, seen);
        return;
    }
    if (t->kind == TY_ARRAY) {
        int esz = ty_size(t->pointee);
        for (int i = 0; i < t->count; i++)
            classify_fields(t->pointee, off + i * esz, cls, seen);
        return;
    }
    int idx = off / 8;
    if (idx < 0 || idx > 1)
        return; /* caller already decided MEMORY */
    class_merge(&cls[idx], &seen[idx],
                ty_is_float(t) ? CLASS_SSE : CLASS_INTEGER);
}

int ty_classify(const struct type *t, enum arg_class *classes)
{
    if (t->kind != TY_STRUCT) {
        classes[0] = ty_is_float(t) ? CLASS_SSE : CLASS_INTEGER;
        return 1;
    }
    int size = ty_size(t);
    if (size > 16)
        return 0; /* MEMORY */

    int seen[2] = { 0, 0 };
    classes[0] = classes[1] = CLASS_INTEGER;
    classify_fields(t, 0, classes, seen);
    int n = (size + 7) / 8;
    for (int i = 0; i < n; i++)
        if (!seen[i])
            classes[i] = CLASS_INTEGER; /* padding-only: harmless */
    return n;
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
    case TY_BOOL: base = "_Bool"; break;
    case TY_CHAR: base = t->is_unsigned ? "unsigned char" : "char"; break;
    case TY_SHORT: base = t->is_unsigned ? "unsigned short" : "short"; break;
    case TY_INT: base = t->is_unsigned ? "unsigned int" : "int"; break;
    case TY_LONG: base = t->is_unsigned ? "unsigned long" : "long"; break;
    case TY_FLOAT: base = "float"; break;
    case TY_DOUBLE: base = "double"; break;
    case TY_STRUCT:
        snprintf(structbuf, sizeof structbuf, "%s %s",
                 t->is_union ? "union" : "struct",
                 t->tag ? t->tag : "<anonymous>");
        base = structbuf;
        break;
    case TY_FUNC:
        base = "function";
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
