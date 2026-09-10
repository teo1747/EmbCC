/* AArch64 instruction encoding — see emit_arm64.h.
 *
 * Every encoding below is written against the Arm ARM's field layouts and
 * checked byte-for-byte against aarch64-elf-objdump by
 * tests/golden/arm64-encoding.sh. A silently wrong word is the one failure
 * mode this file must not have, so nothing here is "probably right".
 */
#include "emit_arm64.h"

#include <stdio.h>
#include <stdlib.h>

void a64_word(struct code *c, unsigned long word)
{
    code_byte(c, (int)(word & 0xff));
    code_byte(c, (int)((word >> 8) & 0xff));
    code_byte(c, (int)((word >> 16) & 0xff));
    code_byte(c, (int)((word >> 24) & 0xff));
}

static void bad(const char *what, long v)
{
    fprintf(stderr, "embcc: internal error: aarch64 %s cannot encode %ld\n",
            what, v);
    exit(1);
}

/* sf: the bit that selects the 64-bit form of most instructions. */
static unsigned long sf(int w) { return w == 8 ? 0x80000000UL : 0UL; }

/* ---- moves ---------------------------------------------------------- */

void a64_mov_reg(struct code *c, int rd, int rm, int w)
{
    if (rd == rm)
        return;
    /* MOV (register) is ORR Rd, ZR, Rm. SP is not encodable there, so a
     * move to or from SP goes through ADD Rd, Rn, #0 instead. */
    if (rd == A64_SP || rm == A64_SP) {
        a64_word(c, 0x91000000UL | ((unsigned long)rm << 5) |
                    (unsigned long)rd);
        return;
    }
    a64_word(c, 0x2A0003E0UL | sf(w) | ((unsigned long)rm << 16) |
                (unsigned long)rd);
}

void a64_mov_imm(struct code *c, int rd, long imm, int w)
{
    unsigned long v = (unsigned long)imm;
    if (w == 4)
        v &= 0xffffffffUL;

    /* MOVN builds a value whose 16-bit chunks are mostly 0xffff in one
     * instruction where MOVZ would need four, which is what makes small
     * negative constants cheap. */
    unsigned long inv = (w == 4) ? (~v & 0xffffffffUL) : ~v;
    int nz = 0, ni = 0;
    for (int k = 0; k < (w == 8 ? 4 : 2); k++) {
        if (((v >> (16 * k)) & 0xffff) != 0) nz++;
        if (((inv >> (16 * k)) & 0xffff) != 0) ni++;
    }

    if (ni < nz) {
        /* MOVN with the first non-0xffff chunk, then MOVK the rest. */
        int first = -1;
        for (int k = 0; k < (w == 8 ? 4 : 2); k++)
            if (((inv >> (16 * k)) & 0xffff) != 0) { first = k; break; }
        if (first < 0) first = 0;         /* v is all ones */
        unsigned long chunk = (inv >> (16 * first)) & 0xffff;
        a64_word(c, 0x12800000UL | sf(w) | ((unsigned long)first << 21) |
                    (chunk << 5) | (unsigned long)rd);
        for (int k = 0; k < (w == 8 ? 4 : 2); k++) {
            if (k == first)
                continue;
            unsigned long ch = (v >> (16 * k)) & 0xffff;
            if (ch == 0xffff)             /* already set by the MOVN */
                continue;
            a64_word(c, 0x72800000UL | sf(w) | ((unsigned long)k << 21) |
                        (ch << 5) | (unsigned long)rd);
        }
        return;
    }

    /* MOVZ the first non-zero chunk, MOVK each remaining one. A zero
     * constant still needs the MOVZ, so `first` defaults to 0. */
    int first = -1;
    for (int k = 0; k < (w == 8 ? 4 : 2); k++)
        if (((v >> (16 * k)) & 0xffff) != 0) { first = k; break; }
    if (first < 0) first = 0;
    unsigned long chunk = (v >> (16 * first)) & 0xffff;
    a64_word(c, 0x52800000UL | sf(w) | ((unsigned long)first << 21) |
                (chunk << 5) | (unsigned long)rd);
    for (int k = first + 1; k < (w == 8 ? 4 : 2); k++) {
        unsigned long ch = (v >> (16 * k)) & 0xffff;
        if (ch == 0)
            continue;
        a64_word(c, 0x72800000UL | sf(w) | ((unsigned long)k << 21) |
                    (ch << 5) | (unsigned long)rd);
    }
}

/* ---- arithmetic and logic ------------------------------------------- */

static int addsub_imm(struct code *c, unsigned long base, int rd, int rn,
                      long imm, int w)
{
    if (imm < 0)
        return 0;
    unsigned long sh = 0, v = (unsigned long)imm;
    if (v > 0xfff) {
        if ((v & 0xfff) != 0 || v > 0xfffUL << 12)
            return 0;
        v >>= 12;
        sh = 1;
    }
    a64_word(c, base | sf(w) | (sh << 22) | (v << 10) |
                ((unsigned long)rn << 5) | (unsigned long)rd);
    return 1;
}

int a64_add_imm(struct code *c, int rd, int rn, long imm, int w)
{
    if (imm < 0)
        return a64_sub_imm(c, rd, rn, -imm, w);
    return addsub_imm(c, 0x11000000UL, rd, rn, imm, w);
}

int a64_sub_imm(struct code *c, int rd, int rn, long imm, int w)
{
    if (imm < 0)
        return a64_add_imm(c, rd, rn, -imm, w);
    return addsub_imm(c, 0x51000000UL, rd, rn, imm, w);
}

void a64_alu_reg(struct code *c, int op, int rd, int rn, int rm, int w)
{
    unsigned long base;
    switch (op) {
    case '+': base = 0x0B000000UL; break;   /* ADD  (shifted register) */
    case '-': base = 0x4B000000UL; break;   /* SUB  */
    case '&': base = 0x0A000000UL; break;   /* AND  (shifted register) */
    case '|': base = 0x2A000000UL; break;   /* ORR  */
    case '^': base = 0x4A000000UL; break;   /* EOR  */
    default: bad("alu op", op); return;
    }
    a64_word(c, base | sf(w) | ((unsigned long)rm << 16) |
                ((unsigned long)rn << 5) | (unsigned long)rd);
}

void a64_mul(struct code *c, int rd, int rn, int rm, int w)
{
    /* MUL is MADD Rd, Rn, Rm, ZR. */
    a64_word(c, 0x1B007C00UL | sf(w) | ((unsigned long)rm << 16) |
                ((unsigned long)rn << 5) | (unsigned long)rd);
}

void a64_msub(struct code *c, int rd, int rn, int rm, int ra, int w)
{
    a64_word(c, 0x1B008000UL | sf(w) | ((unsigned long)rm << 16) |
                ((unsigned long)ra << 10) | ((unsigned long)rn << 5) |
                (unsigned long)rd);
}

void a64_div(struct code *c, int rd, int rn, int rm, int sign, int w)
{
    unsigned long base = sign ? 0x1AC00C00UL   /* SDIV */
                              : 0x1AC00800UL;  /* UDIV */
    a64_word(c, base | sf(w) | ((unsigned long)rm << 16) |
                ((unsigned long)rn << 5) | (unsigned long)rd);
}

void a64_shift_reg(struct code *c, int kind, int rd, int rn, int rm, int w)
{
    unsigned long base;
    switch (kind) {
    case '<': base = 0x1AC02000UL; break;   /* LSLV */
    case '>': base = 0x1AC02800UL; break;   /* ASRV */
    case 'u': base = 0x1AC02400UL; break;   /* LSRV */
    default: bad("shift kind", kind); return;
    }
    a64_word(c, base | sf(w) | ((unsigned long)rm << 16) |
                ((unsigned long)rn << 5) | (unsigned long)rd);
}

void a64_neg(struct code *c, int rd, int rm, int w)
{
    /* NEG is SUB Rd, ZR, Rm. */
    a64_word(c, 0x4B0003E0UL | sf(w) | ((unsigned long)rm << 16) |
                (unsigned long)rd);
}

void a64_mvn(struct code *c, int rd, int rm, int w)
{
    /* MVN is ORN Rd, ZR, Rm (ORR with N=1). */
    a64_word(c, 0x2A2003E0UL | sf(w) | ((unsigned long)rm << 16) |
                (unsigned long)rd);
}

void a64_rev(struct code *c, int rd, int rn, int size)
{
    unsigned long word;
    switch (size) {
    case 8: word = 0xDAC00C00UL; break;   /* REV   Xd, Xn */
    case 4: word = 0x5AC00800UL; break;   /* REV   Wd, Wn */
    case 2: word = 0x5AC00400UL; break;   /* REV16 Wd, Wn */
    default: bad("rev size", size); return;
    }
    a64_word(c, word | ((unsigned long)rn << 5) | (unsigned long)rd);
}

/* ---- compare -------------------------------------------------------- */

void a64_cmp_reg(struct code *c, int rn, int rm, int w)
{
    /* CMP is SUBS ZR, Rn, Rm. */
    a64_word(c, 0x6B00001FUL | sf(w) | ((unsigned long)rm << 16) |
                ((unsigned long)rn << 5));
}

void a64_cset(struct code *c, int rd, int cond)
{
    /* CSET Rd, cond is CSINC Rd, ZR, ZR, invert(cond). The inversion is
     * the low bit of the condition field. */
    unsigned long inv = (unsigned long)(cond ^ 1);
    a64_word(c, 0x9A9F07E0UL | (inv << 12) | (unsigned long)rd);
}

/* ---- extension ------------------------------------------------------ */

void a64_extend(struct code *c, int rd, int rn, int size, int sign, int w)
{
    if (sign) {
        /* SBFM Rd, Rn, #0, #(8*size - 1). The 64-bit form sets N=1. */
        unsigned long imms = (unsigned long)(8 * size - 1);
        if (w == 8)
            a64_word(c, 0x93400000UL | (imms << 10) |
                        ((unsigned long)rn << 5) | (unsigned long)rd);
        else
            a64_word(c, 0x13000000UL | (imms << 10) |
                        ((unsigned long)rn << 5) | (unsigned long)rd);
        return;
    }
    if (size == 8) {
        a64_mov_reg(c, rd, rn, 8);
        return;
    }
    if (size == 4) {
        /* A 32-bit ORR zero-extends to 64 bits for free -- and it is
         * emitted even when rd == rn, because clearing the upper half IS
         * the work here. a64_mov_reg would elide it as a no-op copy. */
        a64_word(c, 0x2A0003E0UL | ((unsigned long)rn << 16) |
                    (unsigned long)rd);
        return;
    }
    /* UBFM Wd, Wn, #0, #(8*size - 1) — UXTB / UXTH. */
    unsigned long imms = (unsigned long)(8 * size - 1);
    a64_word(c, 0x53000000UL | (imms << 10) | ((unsigned long)rn << 5) |
                (unsigned long)rd);
}

/* ---- memory --------------------------------------------------------- */

/* The scaled 12-bit unsigned-offset forms, indexed by log2(size). */
static unsigned long ldst_base(int size, int load, int sign, int w)
{
    unsigned long sz;
    switch (size) {
    case 1: sz = 0; break;
    case 2: sz = 1; break;
    case 4: sz = 2; break;
    case 8: sz = 3; break;
    default: bad("access size", size); return 0;
    }
    unsigned long opc;
    if (!load) {
        opc = 0;                          /* STR */
    } else if (size == 8) {
        opc = 1;                          /* LDR — the only 64-bit form */
    } else if (size == 4) {
        /* There is no "load signed word into W": a 32-bit load already
         * delivers every bit of a 4-byte value, and the register's upper
         * half is zeroed either way. Only widening to an X register needs
         * the signed form (LDRSW). Encoding opc=3 at this size is
         * unallocated, which assembles to a word objdump prints as
         * `.inst ... undefined` — silently, until the CPU traps on it. */
        opc = (sign && w == 8) ? 2 : 1;
    } else if (!sign) {
        opc = 1;                          /* LDRB / LDRH, zero-extending */
    } else {
        opc = (w == 8) ? 2 : 3;           /* LDRSB / LDRSH into X or W */
    }
    return 0x39000000UL | (sz << 30) | (opc << 22);
}

static void ldst(struct code *c, int rt, int rn, long off, int size,
                 int load, int sign, int w)
{
    unsigned long base = ldst_base(size, load, sign, w);
    if (off >= 0 && off % size == 0 && off / size <= 0xfff) {
        a64_word(c, base | ((unsigned long)(off / size) << 10) |
                    ((unsigned long)rn << 5) | (unsigned long)rt);
        return;
    }
    /* Out of the scaled field: form the address in the scratch register.
     * Callers are documented not to hold anything in A64_SCR here. */
    a64_mov_imm(c, A64_SCR, off, 8);
    if (rn == A64_SP)
        /* SP is not encodable as Rn in the shifted-register ADD; the
         * extended-register form (option = UXTX) is the one that takes it. */
        a64_word(c, 0x8B2063E0UL | ((unsigned long)A64_SCR << 16) |
                    (unsigned long)A64_SCR);   /* add scr, sp, scr */
    else
        a64_alu_reg(c, '+', A64_SCR, rn, A64_SCR, 8);
    a64_word(c, base | ((unsigned long)A64_SCR << 5) | (unsigned long)rt);
}

void a64_ldr(struct code *c, int rt, int rn, long off,
             int size, int sign, int w)
{
    ldst(c, rt, rn, off, size, 1, sign, w);
}

void a64_str(struct code *c, int rt, int rn, long off, int size)
{
    ldst(c, rt, rn, off, size, 0, 0, 8);
}

/* ---- frame ---------------------------------------------------------- */

void a64_prologue(struct code *c, int framesize)
{
    a64_word(c, 0xA9BF7BFDUL);            /* stp x29, x30, [sp, #-16]! */
    a64_word(c, 0x910003FDUL);            /* mov x29, sp */
    if (framesize > 0 &&
        !a64_sub_imm(c, A64_SP, A64_SP, framesize, 8)) {
        a64_mov_imm(c, A64_SCR, framesize, 8);
        /* sub sp, sp, scr  (extended-register form, LSL #0) */
        a64_word(c, 0xCB2063FFUL | ((unsigned long)A64_SCR << 16));
    }
}

void a64_epilogue(struct code *c, int framesize)
{
    if (framesize > 0 &&
        !a64_add_imm(c, A64_SP, A64_SP, framesize, 8)) {
        /* mov sp, x29 restores it in one instruction regardless of size. */
        a64_word(c, 0x910003BFUL);        /* mov sp, x29 */
    }
    a64_word(c, 0xA8C17BFDUL);            /* ldp x29, x30, [sp], #16 */
    a64_word(c, 0xD65F03C0UL);            /* ret */
}

/* ---- control flow --------------------------------------------------- */

void a64_ret(struct code *c) { a64_word(c, 0xD65F03C0UL); }
void a64_blr(struct code *c, int rn)
{
    a64_word(c, 0xD63F0000UL | ((unsigned long)rn << 5));
}
void a64_br(struct code *c, int rn)
{
    a64_word(c, 0xD61F0000UL | ((unsigned long)rn << 5));
}

int a64_b(struct code *c)    { int o = c->len; a64_word(c, 0x14000000UL); return o; }
int a64_bl(struct code *c)   { int o = c->len; a64_word(c, 0x94000000UL); return o; }

int a64_bcond(struct code *c, int cond)
{
    int o = c->len;
    a64_word(c, 0x54000000UL | (unsigned long)cond);
    return o;
}

int a64_cbz(struct code *c, int rt, int nonzero, int w)
{
    int o = c->len;
    a64_word(c, (nonzero ? 0x35000000UL : 0x34000000UL) | sf(w) |
                (unsigned long)rt);
    return o;
}

static unsigned long word_at(struct code *c, int at)
{
    return (unsigned long)(unsigned char)c->p[at] |
           ((unsigned long)(unsigned char)c->p[at + 1] << 8) |
           ((unsigned long)(unsigned char)c->p[at + 2] << 16) |
           ((unsigned long)(unsigned char)c->p[at + 3] << 24);
}

void a64_patch_b26(struct code *c, int at, int target)
{
    long delta = ((long)target - (long)at) / 4;
    if (delta < -(1L << 25) || delta >= (1L << 25))
        bad("branch displacement", delta);
    unsigned long w = word_at(c, at);
    w = (w & 0xFC000000UL) | ((unsigned long)delta & 0x03FFFFFFUL);
    code_patch32(c, at, w);
}

void a64_patch_b19(struct code *c, int at, int target)
{
    long delta = ((long)target - (long)at) / 4;
    if (delta < -(1L << 18) || delta >= (1L << 18))
        bad("conditional branch displacement", delta);
    unsigned long w = word_at(c, at);
    w = (w & 0xFF00001FUL) | (((unsigned long)delta & 0x7FFFFUL) << 5);
    code_patch32(c, at, w);
}

/* ---- symbol addresses ----------------------------------------------- */

int a64_adrp(struct code *c, int rd)
{
    int o = c->len;
    a64_word(c, 0x90000000UL | (unsigned long)rd);
    return o;
}

int a64_adr(struct code *c, int rd)
{
    int o = c->len;
    a64_word(c, 0x10000000UL | (unsigned long)rd);
    return o;
}

void a64_patch_adr(struct code *c, int at, int target)
{
    long delta = (long)target - (long)at;      /* bytes, not words */
    if (delta < -(1L << 20) || delta >= (1L << 20))
        bad("adr displacement", delta);
    unsigned long d = (unsigned long)delta & 0x1FFFFFUL;
    unsigned long w = word_at(c, at);
    w = (w & 0x9F00001FUL) | ((d & 3UL) << 29) | (((d >> 2) & 0x7FFFFUL) << 5);
    code_patch32(c, at, w);
}

int a64_add_lo12(struct code *c, int rd, int rn)
{
    int o = c->len;
    a64_word(c, 0x91000000UL | ((unsigned long)rn << 5) | (unsigned long)rd);
    return o;
}

/* ---- misc ----------------------------------------------------------- */

void a64_dmb_ish(struct code *c) { a64_word(c, 0xD5033BBFUL); }
void a64_udf(struct code *c)     { a64_word(c, 0x00000000UL); }

/* ---- scalar floating point ------------------------------------------ */

/* The SIMD&FP load/store share the integer unsigned-offset layout with the
 * V bit (26) set: size 10 selects S registers, 11 selects D. */
static void fldst(struct code *c, int vt, int rn, long off, int w, int load)
{
    unsigned long sz = (w == 8) ? 3UL : 2UL;
    unsigned long base = 0x3D000000UL | (sz << 30) | (load ? 0x400000UL : 0);
    if (off >= 0 && off % w == 0 && off / w <= 0xfff) {
        a64_word(c, base | ((unsigned long)(off / w) << 10) |
                    ((unsigned long)rn << 5) | (unsigned long)vt);
        return;
    }
    a64_mov_imm(c, A64_SCR, off, 8);
    if (rn == A64_SP)
        a64_word(c, 0x8B2063E0UL | ((unsigned long)A64_SCR << 16) |
                    (unsigned long)A64_SCR);
    else
        a64_alu_reg(c, '+', A64_SCR, rn, A64_SCR, 8);
    a64_word(c, base | ((unsigned long)A64_SCR << 5) | (unsigned long)vt);
}

void a64_fldr(struct code *c, int vt, int rn, long off, int w)
{
    fldst(c, vt, rn, off, w, 1);
}

void a64_fstr(struct code *c, int vt, int rn, long off, int w)
{
    fldst(c, vt, rn, off, w, 0);
}

/* `type` for the FP data-processing groups: 00 single, 01 double. */
static unsigned long ftype(int w) { return (w == 8) ? (1UL << 22) : 0UL; }

void a64_falu(struct code *c, int op, int vd, int vn, int vm, int w)
{
    unsigned long opcode;
    switch (op) {
    case '*': opcode = 0; break;
    case '/': opcode = 1; break;
    case '+': opcode = 2; break;
    case '-': opcode = 3; break;
    default: bad("float alu op", op); return;
    }
    a64_word(c, 0x1E200800UL | ftype(w) | (opcode << 12) |
                ((unsigned long)vm << 16) | ((unsigned long)vn << 5) |
                (unsigned long)vd);
}

/* Floating-point data-processing, one source: opcode in bits 20..15. */
static void fdp1(struct code *c, unsigned long opcode, int vd, int vn,
                 unsigned long type)
{
    a64_word(c, 0x1E200000UL | type | (opcode << 15) | (0x10UL << 10) |
                ((unsigned long)vn << 5) | (unsigned long)vd);
}

void a64_fmov_reg(struct code *c, int vd, int vn, int w)
{
    if (vd == vn)
        return;
    fdp1(c, 0, vd, vn, ftype(w));
}

void a64_fneg(struct code *c, int vd, int vn, int w)
{
    fdp1(c, 2, vd, vn, ftype(w));
}

void a64_fcvt(struct code *c, int vd, int vn, int from_w, int to_w)
{
    if (from_w == to_w) {
        a64_fmov_reg(c, vd, vn, to_w);
        return;
    }
    /* The opcode names the DESTINATION precision (01 single, 00 double at
     * bits 16..15), while `type` names the source's. */
    if (from_w == 4)
        fdp1(c, 5, vd, vn, ftype(4));    /* fcvt Dd, Sn */
    else
        fdp1(c, 4, vd, vn, ftype(8));    /* fcvt Sd, Dn */
}

void a64_fcmp(struct code *c, int vn, int vm, int w)
{
    a64_word(c, 0x1E202000UL | ftype(w) | ((unsigned long)vm << 16) |
                ((unsigned long)vn << 5));
}

/* Conversions between the two register files: sf | type | rmode | opcode. */
static void fcvt_int(struct code *c, unsigned long rmode, unsigned long opcode,
                     int rd_or_vd, int rn_or_vn, int iw, int fw)
{
    unsigned long word = 0x1E200000UL | ftype(fw) | (rmode << 19) |
                         (opcode << 16) |
                         ((unsigned long)rn_or_vn << 5) |
                         (unsigned long)rd_or_vd;
    if (iw == 8)
        word |= 0x80000000UL;            /* sf: the X-register form */
    a64_word(c, word);
}

void a64_cvt_i2f(struct code *c, int vd, int rn, int sign, int iw, int fw)
{
    /* scvtf = opcode 010, ucvtf = 011, both with rmode 00. */
    fcvt_int(c, 0, sign ? 2UL : 3UL, vd, rn, iw, fw);
}

void a64_cvt_f2i(struct code *c, int rd, int vn, int sign, int iw, int fw)
{
    /* fcvtzs = opcode 000, fcvtzu = 001, both with rmode 11 (toward zero,
     * which is the rounding C's float-to-integer conversion requires). */
    fcvt_int(c, 3, sign ? 0UL : 1UL, rd, vn, iw, fw);
}
