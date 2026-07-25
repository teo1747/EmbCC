#include "ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../sema/sema.h"
#include "../sema/type.h"

/* The source line currently being lowered. gen_stmt updates it as it walks
 * the statement list, and gen_func resets it per function; emit() stamps it
 * onto every instruction so the -g line table in codegen can map .text
 * offsets back to source lines. irgen runs one function at a time, single
 * threaded, so a file-scope cursor is sound. Off (-g absent) it is simply
 * ignored — nothing reads ir_ins.line. */
static int g_cur_line;

static struct ir_ins *emit(struct ir_func *fn)
{
    if (fn->nins == fn->cap) {
        fn->cap = fn->cap ? fn->cap * 2 : 16;
        fn->ins = xrealloc(fn->ins, (size_t)fn->cap * sizeof *fn->ins);
    }
    struct ir_ins *i = &fn->ins[fn->nins++];
    /* Zero first: the array is grown with xrealloc, so a reused slot
     * carries a previous instruction's bytes. Fields an instruction does
     * not set (retsize/retnclass on a non-struct call, the whole va/arg
     * machinery on a plain op) must read as 0, not stale garbage — a
     * garbage retnclass once walked retcls[] off the end and crashed. */
    memset(i, 0, sizeof *i);
    i->op = IR_CONST;
    i->line = g_cur_line;
    i->dst = i->a = i->b = -1;
    i->w = 4;
    i->size = 4;
    i->sign = 1;
    i->pred = B_ADD;
    i->label = -1;
    return i;
}

static int new_temp(struct ir_func *fn) { return fn->nvregs++; }
static int new_label(struct ir_func *fn) { return fn->nlabels++; }

static int ty_w(const struct type *t) { return ty_wide(t) ? 8 : 4; }

static void emit_label(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LABEL;
    i->label = label;
}

static void emit_jmp(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_JMP;
    i->label = label;
}

/* goto/label resolution. Labels are function-scoped and forward-referable, so a
 * name gets its IR label the first time EITHER a `goto` or the label itself is
 * seen. gen_func resets this table; after the body it errors on any label that
 * was referenced by a goto but never defined. */
#define IRGEN_MAX_LABELS 256
static struct { const char *name; int label; int defined; int line; }
    g_labels[IRGEN_MAX_LABELS];
static int g_nlabels_used;

static int label_idx(struct ir_func *fn, const char *name, int line)
{
    for (int i = 0; i < g_nlabels_used; i++)
        if (strcmp(g_labels[i].name, name) == 0)
            return i;
    if (g_nlabels_used >= IRGEN_MAX_LABELS)
        diag_fatal(fn->src->file, line, "too many labels in one function");
    g_labels[g_nlabels_used].name = name;
    g_labels[g_nlabels_used].label = new_label(fn);
    g_labels[g_nlabels_used].defined = 0;
    g_labels[g_nlabels_used].line = line;
    return g_nlabels_used++;
}

static void emit_brz(struct ir_func *fn, int v, int w, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRZ;
    i->a = v;
    i->w = w;
    i->label = label;
}

static void emit_brnz(struct ir_func *fn, int v, int w, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRNZ;
    i->a = v;
    i->w = w;
    i->label = label;
}

/* A floating constant is just its BIT PATTERN moved into the slot: a
 * float temp's slot holds raw bits, so no xmm and no constant pool are
 * involved. Same reason loads and stores need no float path. */
static int emit_fconst(struct ir_func *fn, double d, int w)
{
    long bits = 0;
    if (w == 8) {
        double v = d;
        memcpy(&bits, &v, 8);
    } else {
        float v = (float)d;
        unsigned int u = 0;
        memcpy(&u, &v, 4);
        bits = (long)u;
    }
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = bits;
    i->w = w;
    i->dst = new_temp(fn);
    return i->dst;
}

static int emit_const(struct ir_func *fn, long imm, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = imm;
    i->w = w;
    i->dst = new_temp(fn);
    return i->dst;
}

/* dst = a op b at width w; returns dst */
static int emit_bin(struct ir_func *fn, enum ir_op op, int a, int b,
                    int w, int sign)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->sign = sign;
    i->dst = new_temp(fn);
    return i->dst;
}

static int emit_fbin(struct ir_func *fn, enum ir_op op, int a, int b, int w)
{
    struct ir_ins *i = emit(fn);
    i->op = op;
    i->a = a;
    i->b = b;
    i->w = w;
    i->flt = 1;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Load variable v (type t) into a fresh promoted temp. */
static int emit_ldvar(struct ir_func *fn, int v, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LDVAR;
    i->a = v;
    i->size = ty_size(t);
    i->sign = ty_signed_int(t);
    i->w = ty_w(t);
    i->dst = new_temp(fn);
    return i->dst;
}

static void emit_stvar(struct ir_func *fn, int v, int val,
                       const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_STVAR;
    i->dst = v;
    i->a = val;
    i->size = ty_size(t);
}

static int emit_gaddr(struct ir_func *fn, struct global *g)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_GADDR;
    i->glob = g;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Typed load/store through an address temp. */
static int emit_load(struct ir_func *fn, int addr, const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LOAD;
    i->a = addr;
    i->size = ty_size(t);
    i->sign = ty_signed_int(t);
    i->w = ty_w(t);
    i->dst = new_temp(fn);
    return i->dst;
}

static void emit_store(struct ir_func *fn, int addr, int val,
                       const struct type *t)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_STORE;
    i->a = addr;
    i->b = val;
    i->size = ty_size(t);
}

static void emit_mov(struct ir_func *fn, int dst, int src)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_MOV;
    i->dst = dst;
    i->a = src;
}

/* ---- bitfield access (little-endian, gcc-compatible) ----
 * A bitfield occupies bits [bit_off, bit_off+width) of the storage unit at
 * `addr` (a load/store of the field's declared type). Reading shifts the
 * field to the top of the value class then back down — arithmetic for a
 * signed field so its sign bit fills — the classic two-shift extraction,
 * immune to neighbouring fields packed into the same unit. */
static int bf_load(struct ir_func *fn, int addr, const struct member *m)
{
    const struct type *bt = m->ty;
    int w = ty_wide(bt) ? 8 : 4;          /* value-class width, bytes */
    int vb = w * 8;
    /* load the raw storage unit UNSIGNED, so no stray sign extension */
    int v = emit_load(fn, addr, ty_base(bt->kind, 1));
    int lsh = vb - m->bit_off - m->bit_width;
    if (lsh)
        v = emit_bin(fn, IR_SHL, v, emit_const(fn, lsh, 4), w, 0);
    int rsh = vb - m->bit_width;
    if (rsh)
        v = emit_bin(fn, IR_SHR, v, emit_const(fn, rsh, 4), w,
                     ty_signed_int(bt));
    return v;
}

/* Store `val` into a bitfield: read the storage unit, clear the field's
 * bits, OR in the low `width` bits of the value, write it back. Returns the
 * field re-read, which is the assignment expression's (truncated) value. */
static int bf_store(struct ir_func *fn, int addr, const struct member *m,
                    int val)
{
    const struct type *bt = m->ty;
    int w = ty_wide(bt) ? 8 : 4;
    struct type *ut = ty_base(bt->kind, 1);
    unsigned long fmask = m->bit_width >= 64
                        ? ~0UL : (((unsigned long)1 << m->bit_width) - 1);
    unsigned long placed = fmask << m->bit_off;
    int old = emit_load(fn, addr, ut);
    int cleared = emit_bin(fn, IR_AND, old,
                           emit_const(fn, (long)~placed, w), w, 0);
    int low = emit_bin(fn, IR_AND, val,
                       emit_const(fn, (long)fmask, w), w, 0);
    if (m->bit_off)
        low = emit_bin(fn, IR_SHL, low, emit_const(fn, m->bit_off, 4), w, 0);
    int merged = emit_bin(fn, IR_OR, cleared, low, w, 0);
    emit_store(fn, addr, merged, ut);
    return bf_load(fn, addr, m);
}

static int expr_is_bitfield(const struct expr *e)
{
    return e->kind == EXPR_MEMBER && e->memb && e->memb->is_bitfield;
}

static int emit_cmp(struct ir_func *fn, enum binop pred, int a, int b,
                    int w, int sign)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = pred;
    i->a = a;
    i->b = b;
    i->w = w;
    i->sign = sign;
    i->dst = new_temp(fn);
    return i->dst;
}

static int gen_expr(struct ir_func *fn, struct expr *e);
static int gen_complit(struct ir_func *fn, struct expr *e);

/* va_arg(ap, T) for an INTEGER-class T (SysV). ap's value is a pointer to
 * a __va_list_tag { gp_offset u32, fp_offset u32, overflow_arg_area ptr,
 * reg_save_area ptr }. If gp_offset < 48 the argument sits in the register
 * save area at reg_save_area + gp_offset and gp_offset advances by 8;
 * otherwise it is next in the overflow area, which advances by 8. */
static int gen_va_arg(struct ir_func *fn, struct expr *e)
{
    struct type *rt = e->ty;
    struct type *u32 = ty_base(TY_INT, 1);
    struct type *ptr = ty_base(TY_LONG, 1); /* an 8-byte slot */
    int ap = gen_expr(fn, e->lhs);          /* pointer to the tag */

    int a_ova = emit_bin(fn, IR_ADD, ap, emit_const(fn, 8, 8), 8, 1);
    int a_rsa = emit_bin(fn, IR_ADD, ap, emit_const(fn, 16, 8), 8, 1);

    /* SSE class (float/double): the SysV register save area lays the eight xmm
     * regs AFTER the six GP regs, so fp_offset (at ap+4) runs 48..176 in strides
     * of 16 (each xmm slot is 16 bytes, of which we read the low 8 = a double).
     * A variadic float arg is promoted to double, so the overflow slot is 8. */
    if (ty_is_float(rt)) {
        struct type *dbl = ty_base(TY_DOUBLE, 0);
        int a_fp = emit_bin(fn, IR_ADD, ap, emit_const(fn, 4, 8), 8, 1);
        int fp = emit_load(fn, a_fp, u32);      /* fp_offset (at ap+4) */
        int in_reg = emit_cmp(fn, B_LT, fp, emit_const(fn, 176, 4), 4, 0);
        int addr = new_temp(fn);
        int l_over = new_label(fn), l_done = new_label(fn);
        emit_brz(fn, in_reg, 4, l_over);        /* fp_offset >= 176 -> overflow */
        /* register save area: addr = reg_save_area + fp_offset; fp_offset += 16 */
        int rsa = emit_load(fn, a_rsa, ptr);
        emit_mov(fn, addr, emit_bin(fn, IR_ADD, rsa, fp, 8, 1));
        emit_store(fn, a_fp,
                   emit_bin(fn, IR_ADD, fp, emit_const(fn, 16, 4), 4, 0), u32);
        emit_jmp(fn, l_done);
        /* overflow area: addr = overflow_arg_area; advance it by 8 */
        emit_label(fn, l_over);
        int ova = emit_load(fn, a_ova, ptr);
        emit_mov(fn, addr, ova);
        emit_store(fn, a_ova,
                   emit_bin(fn, IR_ADD, ova, emit_const(fn, 8, 8), 8, 1), ptr);
        emit_label(fn, l_done);
        int v = emit_load(fn, addr, dbl);       /* the value is a promoted double */
        if (rt->kind == TY_FLOAT) {             /* va_arg(ap,float): narrow it */
            struct ir_ins *cv = emit(fn);
            cv->op = IR_F2F; cv->a = v; cv->size = 8; cv->w = 4;
            cv->dst = new_temp(fn);
            return cv->dst;
        }
        return v;
    }

    int gp = emit_load(fn, ap, u32);        /* gp_offset (at ap+0) */
    int in_reg = emit_cmp(fn, B_LT, gp, emit_const(fn, 48, 4), 4, 0);

    int addr = new_temp(fn);
    int l_over = new_label(fn), l_done = new_label(fn);
    emit_brz(fn, in_reg, 4, l_over);        /* gp_offset >= 48 -> overflow */

    /* register save area: addr = reg_save_area + gp_offset; gp_offset += 8 */
    int rsa = emit_load(fn, a_rsa, ptr);
    emit_mov(fn, addr, emit_bin(fn, IR_ADD, rsa, gp, 8, 1));
    emit_store(fn, ap, emit_bin(fn, IR_ADD, gp, emit_const(fn, 8, 4), 4, 0),
               u32);
    emit_jmp(fn, l_done);

    /* overflow area: addr = overflow_arg_area; advance it by 8 */
    emit_label(fn, l_over);
    int ova = emit_load(fn, a_ova, ptr);
    emit_mov(fn, addr, ova);
    emit_store(fn, a_ova, emit_bin(fn, IR_ADD, ova, emit_const(fn, 8, 8),
                                   8, 1), ptr);

    emit_label(fn, l_done);
    return emit_load(fn, addr, rt);
}

/* The address of an lvalue (or of a struct-typed expression — struct
 * "values" are represented by their address, since sema bars them from
 * every value context). */
static int gen_addr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_VAR:
        if (e->gref)
            return emit_gaddr(fn, e->gref);
        {
            struct ir_ins *i = emit(fn);
            i->op = IR_ADDR;
            i->a = e->var_index;
            i->dst = new_temp(fn);
            return i->dst;
        }
    case EXPR_DEREF:
        return gen_expr(fn, e->rhs);
    case EXPR_MEMBER: {
        int base = e->is_arrow ? gen_expr(fn, e->lhs)
                               : gen_addr(fn, e->lhs);
        if (e->memb->off == 0)
            return base;
        int off = emit_const(fn, e->memb->off, 8);
        return emit_bin(fn, IR_ADD, base, off, 8, 1);
    }
    case EXPR_COMPLIT:
        return gen_complit(fn, e);
    default:
        fprintf(stderr, "embcc: internal: address of a non-lvalue\n");
        exit(1);
    }
}

static int gen_expr(struct ir_func *fn, struct expr *e);

/* A compound literal `(type){ init }`: clear its synthesized slot, place the
 * flattened initializer leaves (zero-fill + last-write-wins, like a declared
 * aggregate), and return the object's address. */
static int gen_complit(struct ir_func *fn, struct expr *e)
{
    struct ir_ins *ad = emit(fn);
    ad->op = IR_ADDR;
    ad->a = e->var_index;
    ad->dst = new_temp(fn);
    int base = ad->dst;
    struct ir_ins *z = emit(fn);
    z->op = IR_MEMZERO;
    z->a = base;
    /* e->ty is the decayed pointer for an array literal; the OBJECT's size
     * is the undecayed array (or the type itself for struct/scalar). */
    z->size = ty_size(e->undecayed ? e->undecayed : e->ty);
    for (int k = 0; k < e->ninits; k++) {
        int v = gen_expr(fn, e->inits[k].e);
        int at = base;
        if (e->inits[k].off) {
            int o = emit_const(fn, e->inits[k].off, 8);
            at = emit_bin(fn, IR_ADD, base, o, 8, 1);
        }
        if (e->inits[k].ty->kind == TY_STRUCT) {
            struct ir_ins *m = emit(fn);
            m->op = IR_MEMCPY;
            m->a = at;
            m->b = v;
            m->size = ty_size(e->inits[k].ty);
        } else {
            emit_store(fn, at, v, e->inits[k].ty);
        }
    }
    return base;
}

/* The unit being generated — for the string table. One compilation per
 * process, so a file-scope current-unit pointer is honest. */
static struct ir_unit *cur_unit;

/* Intern a string into the unit's .rodata pool, returning its index.
 * Deduping means a literal shared by code and a global initializer lands
 * once. */
int ir_intern_string(struct ir_unit *iu, const char *bytes, int len)
{
    for (int i = 0; i < iu->nstrs; i++)
        if (iu->strs[i].len == len &&
            memcmp(iu->strs[i].bytes, bytes, (size_t)len) == 0)
            return i;
    if (iu->nstrs == iu->capstrs) {
        iu->capstrs = iu->capstrs ? iu->capstrs * 2 : 8;
        iu->strs = xrealloc(iu->strs,
                            (size_t)iu->capstrs * sizeof *iu->strs);
    }
    struct ir_str *s = &iu->strs[iu->nstrs];
    s->bytes = bytes;
    s->len = len;
    s->off = iu->rodata_len;
    iu->rodata_len += len;
    return iu->nstrs++;
}

static int intern_str(const char *bytes, int len)
{
    return ir_intern_string(cur_unit, bytes, len);
}

/* !x and conditions want "is zero" — comparison against a zero of the
 * operand's width. */
static int emit_isz(struct ir_func *fn, int v, int w)
{
    int zero = emit_const(fn, 0, w);
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = B_EQ;
    i->a = v;
    i->b = zero;
    i->w = w;
    i->sign = 1;
    i->dst = new_temp(fn);
    return i->dst;
}

/* Convert any scalar to _Bool: the result is (v != 0), a 0/1 int. C says a
 * store to _Bool normalizes this way, and a float 0.5 must become 1 (so it
 * compares the float directly, not a truncation). */
static int emit_tobool(struct ir_func *fn, int v, const struct type *from)
{
    int flt = ty_is_float(from);
    /* the zero operand MUST be emitted before the compare that reads it —
     * the IR is lowered in order, so an operand emitted after would be
     * materialized after the cmp already ran (a real bug, once). */
    int zero = flt ? emit_fconst(fn, 0.0, ty_size(from))
                   : emit_const(fn, 0, ty_w(from));
    struct ir_ins *i = emit(fn);
    i->op = IR_CMP;
    i->pred = B_NE;
    i->a = v;
    i->b = zero;
    i->sign = 0;
    i->flt = flt;
    i->w = flt ? ty_size(from) : ty_w(from);
    i->dst = new_temp(fn);
    return i->dst;
}

/* An I2F/F2I instruction, spelled out because unsigned-64 conversions build
 * several by hand. `a` is the source vreg. */
static int emit_i2f(struct ir_func *fn, int a, int srcw, int dstw)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_I2F; i->a = a; i->size = srcw; i->sign = 1; i->w = dstw;
    i->dst = new_temp(fn);
    return i->dst;
}
static int emit_f2i(struct ir_func *fn, int a, int srcw, int dstw)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_F2I; i->a = a; i->size = srcw; i->sign = 1; i->w = dstw;
    i->dst = new_temp(fn);
    return i->dst;
}

/* unsigned-64 -> floating. SSE2's cvtsi2sd is SIGNED, so a u64 with its top bit
 * set would convert as a huge negative. Split into two 32-bit halves -- each is
 * positive and < 2^32, so cvtsi2sd is exact -- then hi*2^32 + lo. Both partials
 * are exact doubles, so the single add rounds the true u64 once (correctly
 * rounded). For a float target do it in double first (exact) then narrow, which
 * avoids a double rounding. */
static int gen_u64_to_float(struct ir_func *fn, int v, int tsize)
{
    int hi = emit_bin(fn, IR_SHR, v, emit_const(fn, 32, 4), 8, 0);      /* v >> 32 */
    int lo = emit_bin(fn, IR_AND, v, emit_const(fn, 0xffffffffL, 8), 8, 1);
    int hd = emit_i2f(fn, hi, 8, 8);
    int ld = emit_i2f(fn, lo, 8, 8);
    int hs = emit_fbin(fn, IR_MUL, hd, emit_fconst(fn, 4294967296.0, 8), 8);
    int res = emit_fbin(fn, IR_ADD, hs, ld, 8);
    if (tsize == 4) {   /* narrow the exact double to float: one rounding */
        struct ir_ins *nf = emit(fn);
        nf->op = IR_F2F; nf->a = res; nf->size = 8; nf->w = 4;
        nf->dst = new_temp(fn);
        return nf->dst;
    }
    return res;
}

/* floating -> unsigned-64. cvttsd2si is SIGNED: exact for v < 2^63, but v in
 * [2^63, 2^64) overflows it. For those, convert (v - 2^63) and set the top bit
 * back. Branch on v >= 2^63. */
static int gen_float_to_u64(struct ir_func *fn, int v, int fsize)
{
    int two63 = emit_fconst(fn, 9223372036854775808.0, fsize);   /* 2^63 */
    struct ir_ins *cmp = emit(fn);                               /* c = v >= 2^63 */
    cmp->op = IR_CMP; cmp->pred = B_GE; cmp->a = v; cmp->b = two63;
    cmp->w = fsize; cmp->flt = 1; cmp->dst = new_temp(fn);
    int res = new_temp(fn);
    int l_small = new_label(fn), l_done = new_label(fn);
    emit_brz(fn, cmp->dst, 4, l_small);                          /* v < 2^63 -> direct */
    /* v >= 2^63: (u64)(v - 2^63) with the sign bit flipped back on */
    int vm = emit_fbin(fn, IR_SUB, v, two63, fsize);
    int big = emit_bin(fn, IR_XOR, emit_f2i(fn, vm, fsize, 8),
                       emit_const(fn, (long)1 << 63, 8), 8, 0);
    emit_mov(fn, res, big);
    emit_jmp(fn, l_done);
    emit_label(fn, l_small);
    emit_mov(fn, res, emit_f2i(fn, v, fsize, 8));
    emit_label(fn, l_done);
    return res;
}

/* Change a temp's representation between type classes: truncating to a
 * narrow type re-extends from its low bytes; widening extends per the
 * SOURCE's signedness. Free conversions return the same temp. */
static int gen_convert(struct ir_func *fn, int v, const struct type *from,
                       const struct type *to)
{
    int fsize = ty_size(from), tsize = ty_size(to);
    int fw = ty_w(from), tw = ty_w(to);

    /* To _Bool is a normalize-to-0/1, not a truncation. */
    if (to->kind == TY_BOOL && from->kind != TY_BOOL)
        return emit_tobool(fn, v, from);

    /* Floating conversions are real instructions, not reinterpretations
     * — the bit patterns have nothing in common. */
    if (ty_is_float(from) || ty_is_float(to)) {
        struct ir_ins *i;
        if (ty_is_float(from) && ty_is_float(to)) {
            if (fsize == tsize)
                return v;
            i = emit(fn);
            i->op = IR_F2F;
            i->a = v;
            i->size = fsize;
            i->w = tsize;
            i->dst = new_temp(fn);
            return i->dst;
        }
        if (ty_is_float(to)) {
            /* unsigned 64-bit -> float needs the split-and-add fixup; every
             * other integer source goes straight through signed cvtsi2sd. */
            if (from->is_unsigned && ty_size(from) == 8)
                return gen_u64_to_float(fn, v, tsize);
            /* int -> float, and the source WIDTH matters: a 32-bit
             * operation zero-extends its result into the 8-byte slot
             * regardless of signedness, so a negative int read back as
             * 64 bits is 2^32 too large. Read a signed 32-bit source as
             * 32 bits and let cvtsi2sd interpret the sign; read an
             * unsigned int as 64, where the zero extension IS the value
             * (which is what makes it exact). */
            int srcw = 4;
            if (ty_wide(from) ||
                (from->is_unsigned && ty_size(from) == 4))
                srcw = 8;
            i = emit(fn);
            i->op = IR_I2F;
            i->a = v;
            i->size = srcw;
            i->sign = 1;
            i->w = tsize;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* float -> unsigned 64-bit needs the 2^63 bias fixup (cvttsd2si is
         * signed); other targets use the signed convert-then-narrow below. */
        if (to->is_unsigned && ty_size(to) == 8)
            return gen_float_to_u64(fn, v, fsize);
        /* float -> int: truncates toward zero, as C requires. Convert
         * to the 64-bit form then narrow, so unsigned int lands right. */
        i = emit(fn);
        i->op = IR_F2I;
        i->a = v;
        i->size = fsize;
        i->w = 8;
        i->sign = 1;
        i->dst = new_temp(fn);
        int iv = i->dst;
        if (tsize <= 2) {
            struct ir_ins *x = emit(fn);
            x->op = IR_EXT;
            x->a = iv;
            x->size = tsize;
            x->sign = ty_signed_int(to);
            x->w = 4;
            x->dst = new_temp(fn);
            return x->dst;
        }
        return iv;
    }

    if (tsize <= 2) {
        /* to char/short: truncate + extend per TARGET's signedness */
        struct ir_ins *i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        i->size = tsize;
        i->sign = ty_signed_int(to);
        i->w = 4;
        i->dst = new_temp(fn);
        return i->dst;
    }
    if (tw == 8 && fw == 4) {
        /* int class -> long/pointer: extend per SOURCE signedness;
         * narrow sources were already promoted, so extend from 32. */
        struct ir_ins *i = emit(fn);
        i->op = IR_EXT;
        i->a = v;
        i->size = 4;
        i->sign = fsize <= 2 ? 1 : ty_signed_int(from);
        i->w = 8;
        i->dst = new_temp(fn);
        return i->dst;
    }
    /* long->int (read low 32), ptr<->long, same class: free */
    return v;
}

static int gen_expr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM:
        return emit_const(fn, e->num, ty_w(e->ty));
    case EXPR_FNUM:
        return emit_fconst(fn, e->fnum, ty_size(e->ty));
    case EXPR_STR: {
        e->str_index = intern_str(e->name, (int)e->num);
        struct ir_ins *i = emit(fn);
        i->op = IR_STRADDR;
        i->label = e->str_index;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_VAR:
        if (e->fref) { /* function designator: its address */
            struct ir_ins *i = emit(fn);
            i->op = IR_FADDR;
            i->callee = e->fref;
            i->dst = new_temp(fn);
            return i->dst;
        }
        /* arrays and structs are represented by their address */
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return gen_addr(fn, e);
        if (e->gref)
            return emit_load(fn, emit_gaddr(fn, e->gref), e->ty);
        return emit_ldvar(fn, e->var_index, e->ty);
    case EXPR_MEMBER: {
        int addr = gen_addr(fn, e);
        if (e->memb->is_bitfield)
            return bf_load(fn, addr, e->memb);
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr; /* array member decays; nested struct is addr */
        return emit_load(fn, addr, e->ty);
    }
    case EXPR_COMPLIT: {
        int addr = gen_complit(fn, e);
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr; /* an array decays; a struct is carried by address */
        return emit_load(fn, addr, e->ty);
    }
    case EXPR_ASSIGN: {
        if (e->ty->kind == TY_STRUCT) {
            /* a struct assignment is a copy of its bytes */
            int dst = gen_addr(fn, e->lhs);
            int src = gen_expr(fn, e->rhs); /* structs ARE addresses */
            struct ir_ins *i = emit(fn);
            i->op = IR_MEMCPY;
            i->a = dst;
            i->b = src;
            i->size = ty_size(e->ty);
            return dst;
        }
        if (e->lhs->kind == EXPR_VAR && !e->lhs->gref) {
            int v = gen_expr(fn, e->rhs);
            emit_stvar(fn, e->lhs->var_index, v, e->ty);
            return v;
        }
        /* global, *p, or member: a store through an address */
        int addr = gen_addr(fn, e->lhs);
        int v = gen_expr(fn, e->rhs);
        if (expr_is_bitfield(e->lhs))
            return bf_store(fn, addr, e->lhs->memb, v);
        emit_store(fn, addr, v, e->ty);
        return v;
    }
    case EXPR_INCDEC: {
        struct type *t = e->ty;
        int scale = t->kind == TY_PTR ? ty_size(t->pointee) : 1;
        int w = ty_w(t);
        int local = e->lhs->kind == EXPR_VAR && !e->lhs->gref;
        int is_bf = expr_is_bitfield(e->lhs);
        int addr = local ? -1 : gen_addr(fn, e->lhs);
        int cur = local ? emit_ldvar(fn, e->lhs->var_index, t)
                : is_bf ? bf_load(fn, addr, e->lhs->memb)
                        : emit_load(fn, addr, t);
        int old = -1;
        if (e->is_post) {
            struct ir_ins *save = emit(fn);
            save->op = IR_MOV;
            save->a = cur;
            save->dst = old = new_temp(fn);
        }
        int d = emit_const(fn, (long)e->delta * scale, w);
        int sum = emit_bin(fn, IR_ADD, cur, d, w, 1);
        if (ty_size(t) <= 2 && !is_bf) {
            /* ++c on a char must wrap like a char, in the value too */
            struct ir_ins *i = emit(fn);
            i->op = IR_EXT;
            i->a = sum;
            i->size = ty_size(t);
            i->sign = ty_signed_int(t);
            i->w = 4;
            i->dst = new_temp(fn);
            sum = i->dst;
        }
        if (local)
            emit_stvar(fn, e->lhs->var_index, sum, t);
        else if (is_bf)
            sum = bf_store(fn, addr, e->lhs->memb, sum);
        else
            emit_store(fn, addr, sum, t);
        return e->is_post ? old : sum;
    }
    case EXPR_NOT: {
        int v = gen_expr(fn, e->rhs);
        return emit_isz(fn, v, ty_w(e->rhs->ty));
    }
    case EXPR_NEG:
    case EXPR_BNOT: {
        int v = gen_expr(fn, e->rhs);
        if (e->kind == EXPR_NEG && ty_is_float(e->ty)) {
            /* -x on a float flips the sign BIT: exact for -0.0 and for
             * NaN, which 0.0-x is not, and it needs no new instruction
             * because the slot already holds the pattern. */
            int sz = ty_size(e->ty);
            int mask = emit_const(fn,
                                  sz == 8 ? (long)0x8000000000000000LL
                                          : (long)0x80000000L, sz);
            return emit_bin(fn, IR_XOR, v, mask, sz, 0);
        }
        struct ir_ins *i = emit(fn);
        i->op = e->kind == EXPR_NEG ? IR_NEG : IR_BNOT;
        i->a = v;
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_DEREF: {
        int addr = gen_expr(fn, e->rhs);
        /* *fp is fp (the OPERAND points at a function — nothing to
         * load); rows of 2-D arrays and structs are addresses too */
        if (e->rhs->ty->pointee->kind == TY_FUNC)
            return addr;
        if (e->undecayed || e->ty->kind == TY_STRUCT)
            return addr;
        struct ir_ins *i = emit(fn);
        i->op = IR_LOAD;
        i->a = addr;
        i->size = ty_size(e->ty);
        i->sign = ty_signed_int(e->ty);
        i->w = ty_w(e->ty);
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_ADDR:
        return gen_addr(fn, e->rhs);
    case EXPR_CAST: {
        int v = gen_expr(fn, e->rhs);
        if (e->ty->kind == TY_VOID)
            return v; /* evaluated for its effect; the value is dropped */
        return gen_convert(fn, v, e->rhs->ty, e->ty);
    }
    case EXPR_SIZEOF:
        break; /* folded to EXPR_NUM by sema; unreachable */
    case EXPR_VA_ARG:
        return gen_va_arg(fn, e);
    case EXPR_BINOP: {
        struct type *lt = e->lhs->ty, *rt = e->rhs->ty;

        switch (e->op) {
        case B_LAND:
        case B_LOR: {
            /* Short-circuit: the right side must not run when the left
             * decides — observable through calls. */
            int dst = new_temp(fn);
            int l_short = new_label(fn);
            int l_end = new_label(fn);
            int a = gen_expr(fn, e->lhs);
            int aw = ty_w(lt);
            if (e->op == B_LAND) {
                emit_brz(fn, a, aw, l_short);
                int b = gen_expr(fn, e->rhs);
                int nz = emit_isz(fn, b, ty_w(rt));
                int one = emit_isz(fn, nz, 4); /* !!b */
                struct ir_ins *m = emit(fn);
                m->op = IR_MOV;
                m->a = one;
                m->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_short);
                struct ir_ins *z = emit(fn);
                z->op = IR_CONST;
                z->imm = 0;
                z->w = 4;
                z->dst = dst;
            } else {
                int l_rhs = new_label(fn);
                emit_brz(fn, a, aw, l_rhs);
                struct ir_ins *o = emit(fn);
                o->op = IR_CONST;
                o->imm = 1;
                o->w = 4;
                o->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_rhs);
                int b = gen_expr(fn, e->rhs);
                int nz = emit_isz(fn, b, ty_w(rt));
                int one = emit_isz(fn, nz, 4); /* !!b */
                struct ir_ins *m = emit(fn);
                m->op = IR_MOV;
                m->a = one;
                m->dst = dst;
            }
            emit_label(fn, l_end);
            return dst;
        }
        case B_ADD:
        case B_SUB: {
            int lp = lt->kind == TY_PTR, rp = rt->kind == TY_PTR;
            if (lp && rp) {
                /* ptr - ptr: signed byte difference divided by the element
                 * size. Division (not a shift) so any element size works,
                 * including non-powers-of-two like a 24-byte struct — the
                 * mirror of the IR_MUL scaling on the ptr+int path. */
                int a = gen_expr(fn, e->lhs);
                int b = gen_expr(fn, e->rhs);
                int diff = emit_bin(fn, IR_SUB, a, b, 8, 1);
                int size = ty_size(lt->pointee);
                if (size <= 1)
                    return diff;
                int c = emit_const(fn, size, 8);
                return emit_bin(fn, IR_DIV, diff, c, 8, 1);
            }
            if (lp || rp) {
                /* ptr +/- int: scale the (already long) index */
                struct expr *pe = lp ? e->lhs : e->rhs;
                struct expr *ie = lp ? e->rhs : e->lhs;
                int p = gen_expr(fn, lp ? pe : ie);
                int idx = gen_expr(fn, lp ? ie : pe);
                if (!lp) {
                    int t = p;
                    p = idx;
                    idx = t;
                }
                int size = ty_size((lp ? lt : rt)->pointee);
                if (size > 1) {
                    int c = emit_const(fn, size, 8);
                    idx = emit_bin(fn, IR_MUL, idx, c, 8, 1);
                }
                return emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB,
                                p, idx, 8, 1);
            }
            /* plain arithmetic */
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            enum ir_op o = e->op == B_ADD ? IR_ADD : IR_SUB;
            if (ty_is_float(e->ty))
                return emit_fbin(fn, o, a, b, ty_size(e->ty));
            return emit_bin(fn, o, a, b, ty_w(e->ty),
                            ty_signed_int(e->ty));
        }
        case B_EQ:
        case B_NE:
        case B_LT:
        case B_LE:
        case B_GT:
        case B_GE: {
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            struct ir_ins *i = emit(fn);
            i->op = IR_CMP;
            i->pred = e->op;
            i->a = a;
            i->b = b;
            if (ty_is_float(lt)) {
                i->flt = 1;
                i->w = ty_size(lt);
            } else {
                i->w = ty_w(lt);
            }
            /* pointers compare unsigned, as C requires */
            i->sign = ty_signed_int(lt);
            i->dst = new_temp(fn);
            return i->dst;
        }
        default: {
            static const enum ir_op map[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
            };
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            if (ty_is_float(e->ty))
                return emit_fbin(fn, map[e->op - B_ADD], a, b,
                                 ty_size(e->ty));
            return emit_bin(fn, map[e->op - B_ADD], a, b,
                            ty_w(e->ty), ty_signed_int(e->ty));
        }
        }
    }
    case EXPR_COMMA:
        gen_expr(fn, e->lhs); /* for side effects */
        return gen_expr(fn, e->rhs);
    case EXPR_INITLIST:
        break; /* consumed by sema's flattening; never evaluated */
    case EXPR_COMPOUND: {
        /* the address is computed ONCE — the whole reason this is not
         * desugared to `x = x op y` */
        struct type *lt = e->lhs->ty;
        int local = e->lhs->kind == EXPR_VAR && !e->lhs->gref;
        int is_bf = expr_is_bitfield(e->lhs);
        int addr = local ? -1 : gen_addr(fn, e->lhs);
        int cur = local ? emit_ldvar(fn, e->lhs->var_index, lt)
                : is_bf ? bf_load(fn, addr, e->lhs->memb)
                        : emit_load(fn, addr, lt);
        int rv = gen_expr(fn, e->rhs);
        int res;
        if (lt->kind == TY_PTR) {
            int esz = ty_size(lt->pointee);
            if (esz > 1) {
                int k = emit_const(fn, esz, 8);
                rv = emit_bin(fn, IR_MUL, rv, k, 8, 1);
            }
            res = emit_bin(fn, e->op == B_ADD ? IR_ADD : IR_SUB, cur, rv,
                           8, 1);
        } else {
            struct type *ct = e->cast_ty;
            int cv = gen_convert(fn, cur, lt, ct);
            static const enum ir_op map[] = {
                IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
                IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR,
            };
            enum ir_op o = map[e->op - B_ADD];
            if (ty_is_float(ct))
                res = emit_fbin(fn, o, cv, rv, ty_size(ct));
            else
                res = emit_bin(fn, o, cv, rv, ty_w(ct),
                               ty_signed_int(ct));
            res = gen_convert(fn, res, ct, lt);
        }
        if (local)
            emit_stvar(fn, e->lhs->var_index, res, lt);
        else if (is_bf)
            return bf_store(fn, addr, e->lhs->memb, res);
        else
            emit_store(fn, addr, res, lt);
        return res;
    }
    case EXPR_COND: {
        int dst = new_temp(fn);
        int l_else = new_label(fn);
        int l_end = new_label(fn);
        int c = gen_expr(fn, e->args[0]);
        emit_brz(fn, c, ty_w(e->args[0]->ty), l_else);
        int a = gen_expr(fn, e->lhs);
        struct ir_ins *m1 = emit(fn);
        m1->op = IR_MOV;
        m1->a = a;
        m1->dst = dst;
        emit_jmp(fn, l_end);
        emit_label(fn, l_else);
        int b = gen_expr(fn, e->rhs);
        struct ir_ins *m2 = emit(fn);
        m2->op = IR_MOV;
        m2->a = b;
        m2->dst = dst;
        emit_label(fn, l_end);
        return dst;
    }
    case EXPR_CALL: {
        /* stdarg builtins: va_start records where its va_list lives so
         * codegen can point it at a freshly built __va_list_tag; va_end
         * is a no-op. Neither is a real call. */
        if (e->name && strcmp(e->name, "__builtin_va_start") == 0) {
            int ap = gen_addr(fn, e->args[0]);
            struct ir_ins *i = emit(fn);
            i->op = IR_VA_START;
            i->a = ap;
            return -1;
        }
        if (e->name && strcmp(e->name, "__builtin_va_end") == 0)
            return -1;
        int args[MAX_PARAMS];
        int fptemp = -1;
        if (!e->callee) /* through a pointer: evaluate the callee */
            fptemp = gen_expr(fn, e->lhs);
        for (int k = 0; k < e->nargs; k++)
            args[k] = gen_expr(fn, e->args[k]);
        struct ir_ins *i = emit(fn);
        i->op = IR_CALL;
        i->callee = e->callee;
        i->indirect = !e->callee;
        i->a = fptemp;
        i->call_varargs = e->callee ? e->callee->is_varargs
                                    : e->lhs->ty->pointee->is_varargs;
        i->nargs = e->nargs;

        /* Classify every argument here, where the types still exist;
         * codegen only places what it is told. MEMORY-class arguments
         * are assigned offsets in this call's outgoing area, and the
         * function's frame reserves the widest such area. */
        int stk = 0;
        int ireg = 0, freg = 0;
        /* a hidden return pointer consumes rdi before anything else */
        if (e->ty->kind == TY_STRUCT) {
            enum arg_class rc[2];
            if (ty_classify(e->ty, rc) == 0)
                ireg = 1;
        }
        for (int k = 0; k < e->nargs; k++) {
            struct type *at = e->args[k]->ty;
            struct ir_arg *ar = &i->argv[k];
            ar->vreg = args[k];
            ar->is_struct = at->kind == TY_STRUCT;
            ar->size = ty_size(at);
            ar->nclass = ty_classify(at, ar->cls);
            ar->stk_off = 0;
            ar->on_stack = 0;

            /* SysV: an argument goes on the stack when its class has no
             * registers left for ALL of its eightbytes — the decision is
             * made here so codegen only follows it, and the two cannot
             * drift apart. */
            int ni = 0, nf = 0;
            for (int q = 0; q < ar->nclass; q++) {
                if (ar->cls[q] == CLASS_SSE)
                    nf++;
                else
                    ni++;
            }
            if (ar->nclass == 0 || ireg + ni > 6 || freg + nf > 8) {
                ar->on_stack = 1;
                stk = (stk + 7) & ~7;
                ar->stk_off = stk;
                stk += (ar->size + 7) & ~7;
            } else {
                ireg += ni;
                freg += nf;
            }
        }
        if (stk > fn->outgoing_bytes)
            fn->outgoing_bytes = stk;

        if (e->ty->kind == TY_STRUCT) {
            i->retsize = ty_size(e->ty);
            i->retnclass = ty_classify(e->ty, i->retcls);
            fn->scratch_bytes = (fn->scratch_bytes + 7) & ~7;
            i->scratch = fn->scratch_bytes;
            fn->scratch_bytes += (i->retsize + 7) & ~7;
        }
        i->flt = ty_is_float(e->ty);
        i->w = i->flt ? ty_size(e->ty) : 8;
        i->dst = new_temp(fn);
        return i->dst;
    }
    }
    return -1; /* unreachable; every kind returns above */
}

/* Assemble an extended-asm template into machine bytes, now that every
 * operand has a register (opregs[N] is the register of %N — outputs first,
 * then inputs, as gcc numbers them). EmbCC has no general text assembler,
 * only the small vocabulary real low-level userland C needs: `int $imm`
 * (the syscall trap), `cpuid`, `rdrand %N`, and `setc %N`. Anything else is
 * refused loudly (THE RULE). */
static void asm_assemble(struct ir_func *fn, struct stmt *s,
                         const int *opregs, int nops, struct ir_asm *ia)
{
    const char *file = fn->src->file;
    int line = s->line;
    const char *tmpl = s->asm_s->tmpl;
    unsigned char *code = NULL;
    int n = 0, cap = 0;
    const char *p = tmpl;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == ';')
            p++;
        if (!*p)
            break;
        const char *m = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' &&
               *p != ';')
            p++;
        int mlen = (int)(p - m);
        while (*p == ' ' || *p == '\t')
            p++;

        /* one operand reg for the instructions that take a %N */
        int reg = -1;
        if (*p == '%' && p[1] >= '0' && p[1] <= '9') {
            p++;
            int idx = 0;
            while (*p >= '0' && *p <= '9')
                idx = idx * 10 + (*p++ - '0');
            if (idx >= nops)
                diag_fatal(file, line,
                           "asm operand %%%d out of range in \"%s\"",
                           idx, tmpl);
            reg = opregs[idx];
        }

        if (n + 8 > cap) {
            cap = cap ? cap * 2 : 16;
            code = xrealloc(code, (size_t)cap);
        }
        if (mlen == 3 && strncmp(m, "int", 3) == 0) {
            if (*p != '$')
                diag_fatal(file, line, "asm 'int' wants $vector: \"%s\"",
                           tmpl);
            p++;
            long imm = 0;
            int base = 10;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                base = 16;
                p += 2;
            }
            for (; ; p++) {
                int d;
                if (*p >= '0' && *p <= '9') d = *p - '0';
                else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
                else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
                else break;
                imm = imm * base + d;
            }
            if (imm < 0 || imm > 255)
                diag_fatal(file, line, "asm 'int' vector %ld out of range",
                           imm);
            code[n++] = 0xcd;
            code[n++] = (unsigned char)imm;
        } else if (mlen == 5 && strncmp(m, "cpuid", 5) == 0) {
            code[n++] = 0x0f;
            code[n++] = 0xa2;
        } else if (mlen == 6 && strncmp(m, "rdrand", 6) == 0) {
            if (reg < 0)
                diag_fatal(file, line, "asm 'rdrand' wants %%N: \"%s\"", tmpl);
            code[n++] = (unsigned char)(0x48 | (reg >= 8 ? 1 : 0)); /* REX.W(.B) */
            code[n++] = 0x0f;
            code[n++] = 0xc7;
            code[n++] = (unsigned char)(0xf0 | (reg & 7));          /* /6 */
        } else if (mlen == 4 && strncmp(m, "setc", 4) == 0) {
            if (reg < 0)
                diag_fatal(file, line, "asm 'setc' wants %%N: \"%s\"", tmpl);
            if (reg >= 4)  /* REX to name spl/bpl/sil/dil or r8b.. as a byte */
                code[n++] = (unsigned char)(0x40 | (reg >= 8 ? 1 : 0));
            code[n++] = 0x0f;
            code[n++] = 0x92;
            code[n++] = (unsigned char)(0xc0 | (reg & 7));          /* /0 */
        } else if (mlen == 6 && strncmp(m, "sqrts", 5) == 0 &&
                   (m[5] == 'd' || m[5] == 's')) {
            /* sqrtsd/sqrtss %src,%dst (AT&T order): F2/F3 0F 51 /r, both xmm.
             * `reg` already holds the FIRST operand (%src); parse `,%dst`. */
            int is_sd = m[5] == 'd';
            if (reg < 16)
                diag_fatal(file, line,
                           "asm '%.*s' operands must be 'x' (xmm): \"%s\"",
                           mlen, m, tmpl);
            int src = reg;
            if (*p != ',')
                diag_fatal(file, line,
                           "asm '%.*s' wants %%src,%%dst: \"%s\"", mlen, m, tmpl);
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '%' || !(p[1] >= '0' && p[1] <= '9'))
                diag_fatal(file, line,
                           "asm '%.*s' destination must be %%N: \"%s\"",
                           mlen, m, tmpl);
            p++;
            int idx2 = 0;
            while (*p >= '0' && *p <= '9')
                idx2 = idx2 * 10 + (*p++ - '0');
            if (idx2 >= nops)
                diag_fatal(file, line,
                           "asm operand %%%d out of range in \"%s\"", idx2, tmpl);
            int dst = opregs[idx2];
            if (dst < 16)
                diag_fatal(file, line,
                           "asm '%.*s' destination must be 'x' (xmm): \"%s\"",
                           mlen, m, tmpl);
            int s = src - 16, d = dst - 16;
            code[n++] = (unsigned char)(is_sd ? 0xf2 : 0xf3);
            if (d >= 8 || s >= 8)     /* REX.R names dst>=8, REX.B names src>=8 */
                code[n++] = (unsigned char)(0x40 | (d >= 8 ? 4 : 0) |
                                            (s >= 8 ? 1 : 0));
            code[n++] = 0x0f;
            code[n++] = 0x51;
            code[n++] = (unsigned char)(0xc0 | ((d & 7) << 3) | (s & 7));
        } else {
            diag_fatal(file, line,
                       "asm instruction \"%.*s\" not supported "
                       "(EmbCC assembles int/cpuid/rdrand/setc)", mlen, m);
        }
    }
    ia->code = code;
    ia->codelen = n;
}

/* Assign a free register to an operand whose constraint is allocatable
 * (reg == -2). The pool prefers the low, byte-addressable registers so a
 * setc destination needs no REX. */
static int asm_alloc_reg(int *used, const char *file, int line)
{
    static const int pool[] = { 0, 1, 2, 3, 6, 7, 8, 9, 10, 11 };
    for (unsigned i = 0; i < sizeof pool / sizeof pool[0]; i++)
        if (!used[pool[i]]) {
            used[pool[i]] = 1;
            return pool[i];
        }
    diag_fatal(file, line, "asm: out of registers for the operands");
    return -1;
}

/* Assign a free XMM register to an 'x' (SSE) asm operand. XMM registers are
 * encoded as 16 + n (0..7 -> 16..23) so they share one operand-register space
 * with the GPRs (0..15); codegen and asm_assemble decode reg >= 16 as xmm. */
static int asm_alloc_xmm(int *xused, const char *file, int line)
{
    for (int i = 0; i < 8; i++)
        if (!xused[i]) { xused[i] = 1; return 16 + i; }
    diag_fatal(file, line, "asm: out of xmm registers for the operands");
    return -1;
}

/* Innermost enclosing loop's exit and continue targets; sema already
 * rejected break/continue outside any loop. */
struct loopctx {
    int brk, cont;
};

static void gen_stmt(struct ir_func *fn, struct stmt *s,
                     const struct loopctx *loop)
{
    for (; s; s = s->next) {
        if (s->line)
            g_cur_line = s->line;   /* -g: rows key off statement lines */
        switch (s->kind) {
        case STMT_BREAK:
            emit_jmp(fn, loop->brk);
            break;
        case STMT_CONTINUE:
            emit_jmp(fn, loop->cont);
            break;
        case STMT_LABEL: {
            int ix = label_idx(fn, s->name, s->line);
            if (g_labels[ix].defined)
                diag_fatal(fn->src->file, s->line, "duplicate label '%s'",
                           s->name);
            g_labels[ix].defined = 1;
            emit_label(fn, g_labels[ix].label);
            gen_stmt(fn, s->body, loop);   /* the labeled statement */
            break;
        }
        case STMT_GOTO:
            emit_jmp(fn, g_labels[label_idx(fn, s->name, s->line)].label);
            break;
        case STMT_DECL:
            if (s->is_extern)
                break; /* block-scope extern: a declaration, emits no code */
            if (s->sglob)
                break; /* a static local IS its global; no code here */
            if (s->ninits) {
                /* C zero-fills whatever the initializer does not
                 * mention, so clear the object first and then place
                 * the listed values. */
                struct ir_ins *ad = emit(fn);
                ad->op = IR_ADDR;
                ad->a = s->var_index;
                ad->dst = new_temp(fn);
                int base = ad->dst;
                struct ir_ins *z = emit(fn);
                z->op = IR_MEMZERO;
                z->a = base;
                z->size = ty_size(s->dty);
                for (int k = 0; k < s->ninits; k++) {
                    int v = gen_expr(fn, s->inits[k].e);
                    int at = base;
                    if (s->inits[k].off) {
                        int o = emit_const(fn, s->inits[k].off, 8);
                        at = emit_bin(fn, IR_ADD, base, o, 8, 1);
                    }
                    if (s->inits[k].ty->kind == TY_STRUCT) {
                        struct ir_ins *m = emit(fn);
                        m->op = IR_MEMCPY;
                        m->a = at;
                        m->b = v;
                        m->size = ty_size(s->inits[k].ty);
                    } else {
                        emit_store(fn, at, v, s->inits[k].ty);
                    }
                }
                break;
            }
            if (s->expr) {
                int v = gen_expr(fn, s->expr);
                if (s->dty->kind == TY_STRUCT) {
                    /* initializing a struct is the same byte copy an
                     * assignment is */
                    struct ir_ins *a = emit(fn);
                    a->op = IR_ADDR;
                    a->a = s->var_index;
                    a->dst = new_temp(fn);
                    struct ir_ins *i = emit(fn);
                    i->op = IR_MEMCPY;
                    i->a = a->dst;
                    i->b = v;
                    i->size = ty_size(s->dty);
                } else {
                    emit_stvar(fn, s->var_index, v, s->dty);
                }
            }
            break;
        case STMT_EXPR:
            gen_expr(fn, s->expr); /* value discarded */
            break;
        case STMT_ASM: {
            struct asm_stmt *a = s->asm_s;
            struct ir_asm *ia = xcalloc(1, sizeof *ia);
            ia->nin = a->nin;
            ia->nout = a->nout;
            ia->in = xcalloc((size_t)(a->nin ? a->nin : 1), sizeof *ia->in);
            ia->out = xcalloc((size_t)(a->nout ? a->nout : 1),
                              sizeof *ia->out);
            /* Assign registers: fixed ones (from sema) reserve their slot;
             * allocatable ones (-2) get a free register. Then %N substitution
             * numbers outputs first, then inputs, exactly as gcc does. */
            int used[16] = { 0 };
            int xused[8] = { 0 };   /* xmm operands (reg 16..23) */
            for (int i = 0; i < a->nout; i++) {
                if (a->out[i].reg >= 16) xused[a->out[i].reg - 16] = 1;
                else if (a->out[i].reg >= 0) used[a->out[i].reg] = 1;
            }
            for (int i = 0; i < a->nin; i++) {
                if (a->in[i].reg >= 16) xused[a->in[i].reg - 16] = 1;
                else if (a->in[i].reg >= 0) used[a->in[i].reg] = 1;
            }
            int opregs[2 * MAX_PARAMS], nops = 0;
            for (int i = 0; i < a->nout; i++) {
                int r = a->out[i].reg;
                if (r == -2)      r = asm_alloc_reg(used, fn->src->file, s->line);
                else if (r == -3) r = asm_alloc_xmm(xused, fn->src->file, s->line);
                ia->out[i].reg = r;
                opregs[nops++] = r;
            }
            for (int i = 0; i < a->nin; i++) {
                int r = a->in[i].reg;
                if (r == -2)      r = asm_alloc_reg(used, fn->src->file, s->line);
                else if (r == -3) r = asm_alloc_xmm(xused, fn->src->file, s->line);
                ia->in[i].reg = r;
                opregs[nops++] = r;
            }
            asm_assemble(fn, s, opregs, nops, ia);
            /* An input carries its VALUE; an output the ADDRESS of its
             * lvalue. An xmm ('x') input is moved with movss/movsd, so its
             * size is the operand's own float width. */
            for (int i = 0; i < a->nin; i++) {
                ia->in[i].temp = gen_expr(fn, a->in[i].expr);
                ia->in[i].size = ia->in[i].reg >= 16
                               ? ty_size(a->in[i].expr->ty) : 8;
            }
            for (int i = 0; i < a->nout; i++) {
                ia->out[i].temp = gen_addr(fn, a->out[i].expr);
                ia->out[i].size = ty_size(a->out[i].expr->ty);
            }
            struct ir_ins *ins = emit(fn);
            ins->op = IR_ASM;
            ins->asm_ir = ia;
            break;
        }
        case STMT_RETURN: {
            struct ir_ins *i;
            int v = s->expr ? gen_expr(fn, s->expr) : -1;
            i = emit(fn);
            i->op = IR_RET;
            i->a = v;
            if (s->expr && ty_is_float(s->expr->ty)) {
                i->flt = 1; /* the value goes home in xmm0, not rax */
                i->w = ty_size(s->expr->ty);
            } else if (s->expr && s->expr->ty->kind == TY_STRUCT) {
                /* `a` is the ADDRESS of the value; how it travels home
                 * is the callee's classification, computed in codegen
                 * from the function's own return type. */
                i->size = ty_size(s->expr->ty);
            }
            break;
        }
        case STMT_IF: {
            int l_else = new_label(fn);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, ty_w(s->cond->ty), l_else);
            gen_stmt(fn, s->thn, loop);
            if (s->els) {
                int l_end = new_label(fn);
                emit_jmp(fn, l_end);
                emit_label(fn, l_else);
                gen_stmt(fn, s->els, loop);
                emit_label(fn, l_end);
            } else {
                emit_label(fn, l_else);
            }
            break;
        }
        case STMT_DO: {
            /* body first, THEN the test — the whole point of do-while;
             * continue re-tests, so it targets the condition. */
            struct loopctx lc;
            int l_top = new_label(fn);
            lc.cont = new_label(fn);
            lc.brk = new_label(fn);
            emit_label(fn, l_top);
            gen_stmt(fn, s->body, &lc);
            emit_label(fn, lc.cont);
            int c = gen_expr(fn, s->cond);
            emit_brnz(fn, c, ty_w(s->cond->ty), l_top);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_CASE:
        case STMT_DEFAULT:
            emit_label(fn, s->label);
            break;
        case STMT_SWITCH: {
            /* A compare-and-branch chain: correct and slow, the house
             * rule (ARCHITECTURE §3). A jump table is an OPTIMIZATION
             * and belongs to the optimizer era, not here. */
            struct loopctx lc;
            lc.brk = new_label(fn);
            /* continue inside a switch belongs to the enclosing LOOP;
             * sema has already refused it when there is none. */
            lc.cont = loop ? loop->cont : -1;

            int v = gen_expr(fn, s->cond);
            int w = ty_w(s->cond->ty);
            int sign = ty_signed_int(s->cond->ty);
            struct stmt *list = switch_stmts(s->body);
            int dflt = -1;

            for (struct stmt *c = list; c; c = c->next) {
                if (c->kind == STMT_DEFAULT) {
                    c->label = new_label(fn);
                    dflt = c->label;
                    continue;
                }
                if (c->kind != STMT_CASE)
                    continue;
                c->label = new_label(fn);
                int k = emit_const(fn, c->cval, w);
                struct ir_ins *i = emit(fn);
                i->op = IR_CMP;
                i->pred = B_EQ;
                i->a = v;
                i->b = k;
                i->w = w;
                i->sign = sign;
                i->dst = new_temp(fn);
                emit_brnz(fn, i->dst, 4, c->label);
            }
            emit_jmp(fn, dflt >= 0 ? dflt : lc.brk);
            gen_stmt(fn, list, &lc); /* fallthrough is just: no jumps */
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_WHILE: {
            struct loopctx lc;
            lc.cont = new_label(fn); /* while: continue re-tests */
            lc.brk = new_label(fn);
            emit_label(fn, lc.cont);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, ty_w(s->cond->ty), lc.brk);
            gen_stmt(fn, s->body, &lc);
            emit_jmp(fn, lc.cont);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_FOR: {
            /* for: continue jumps to the STEP, not the condition. */
            int l_cond = new_label(fn);
            struct loopctx lc;
            lc.cont = new_label(fn);
            lc.brk = new_label(fn);
            if (s->initdecl)
                gen_stmt(fn, s->initdecl, &lc);
            if (s->init)
                gen_expr(fn, s->init);
            emit_label(fn, l_cond);
            if (s->cond) { /* NULL = forever, left by break */
                int c = gen_expr(fn, s->cond);
                emit_brz(fn, c, ty_w(s->cond->ty), lc.brk);
            }
            gen_stmt(fn, s->body, &lc);
            emit_label(fn, lc.cont);
            if (s->step)
                gen_expr(fn, s->step);
            emit_jmp(fn, l_cond);
            emit_label(fn, lc.brk);
            break;
        }
        case STMT_BLOCK:
            gen_stmt(fn, s->body, loop);
            break;
        }
    }
}

/* -g: record one source variable. Skips the unnamed (prototype params never
 * reach a definition, but be defensive) so the DWARF DIE always has a name. */
static void add_dbgvar(struct ir_func *fn, const char *name, int vreg,
                       int is_param, struct type *ty)
{
    if (!name) return;
    if (fn->ndbgvars == fn->dbgvarcap) {
        fn->dbgvarcap = fn->dbgvarcap ? fn->dbgvarcap * 2 : 8;
        fn->dbgvars = xrealloc(fn->dbgvars,
                               (size_t)fn->dbgvarcap * sizeof *fn->dbgvars);
    }
    struct ir_dbgvar *v = &fn->dbgvars[fn->ndbgvars++];
    v->name = name;
    v->vreg = vreg;
    v->is_param = is_param;
    v->ty = ty;
}

/* -g: walk the body for block-scope locals. Each STMT_DECL owns a var slot
 * (var_index); a static local became a global (sglob) and has no frame slot,
 * so it is skipped. Flattened into the subprogram — lexical-block scoping is a
 * later refinement, not needed to print a local by name. */
static void collect_locals(struct ir_func *fn, struct stmt *s)
{
    for (; s; s = s->next) {
        switch (s->kind) {
        case STMT_DECL:
            if (s->is_extern)     /* block-scope extern: no local slot at all */
                break;
            if (!s->sglob)
                add_dbgvar(fn, s->name, s->var_index, 0,
                           fn->src->var_tys[s->var_index]);
            break;
        case STMT_IF:
            collect_locals(fn, s->thn);
            collect_locals(fn, s->els);
            break;
        case STMT_WHILE:
        case STMT_DO:
            collect_locals(fn, s->body);
            break;
        case STMT_FOR:
            collect_locals(fn, s->initdecl);
            collect_locals(fn, s->body);
            break;
        case STMT_BLOCK:
        case STMT_SWITCH:
            collect_locals(fn, s->body);
            break;
        default:
            break;
        }
    }
}

static void gen_func(struct ir_func *fn, struct func *f)
{
    fn->src = f;
    fn->nvregs = f->nvars; /* params + locals occupy [0, nvars) */
    g_cur_line = f->line;  /* prologue rows attribute to the definition */
    /* -g bookkeeping (harmless when -g is off — only the DWARF pass reads it):
     * parameters are vregs [0, nparams); locals come from the body. */
    for (int i = 0; i < f->nparams; i++)
        add_dbgvar(fn, f->params[i], i, 1, f->param_tys[i]);
    collect_locals(fn, f->body);
    g_nlabels_used = 0;                 /* labels are per-function */
    gen_stmt(fn, f->body, NULL);
    for (int i = 0; i < g_nlabels_used; i++)
        if (!g_labels[i].defined)
            diag_fatal(fn->src->file, g_labels[i].line,
                       "label '%s' used but not defined", g_labels[i].name);
}

struct ir_unit *irgen(struct unit *u)
{
    struct ir_unit *iu = xcalloc(1, sizeof *iu);
    iu->src = u;
    cur_unit = iu;

    /* Only canonical, defined functions produce code; prototypes of
     * externals produce symbols and relocations instead (driver).
     * UNUSED static functions are skipped entirely — headers define
     * static inline helpers wholesale (newlib stdio does), and
     * emitting the unused ones would drag their callees into every
     * link. Internal linkage makes this invisible to other objects. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn &&
            (!f->is_static || f->used || strcmp(f->name, "main") == 0))
            iu->nfuncs++;
    iu->funcs = xcalloc((size_t)iu->nfuncs, sizeof *iu->funcs);

    int n = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn &&
            (!f->is_static || f->used || strcmp(f->name, "main") == 0))
            gen_func(&iu->funcs[n++], f);
    return iu;
}
