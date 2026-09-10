/* AArch64 instruction encoding (ARCHITECTURE §2: integrated — no external
 * assembler exists on-OS). Exactly the encodings the aarch64 codegen emits;
 * an instruction this file cannot encode is a missing function, which fails
 * at build time — never a silently wrong word.
 *
 * Width model mirrors the IR's and the x86 emitter's: `w` is 4 or 8 and
 * chooses the W (32-bit) or X (64-bit) form. Memory accesses take a `size`
 * of 1/2/4/8 with a `sign` for the extending loads.
 *
 * Frame slots are addressed [sp, #off] with a NON-NEGATIVE off, unlike the
 * x86 backend's [rbp-N]: the unsigned-offset load/store form scales its
 * 12-bit field by the access size, which reaches 32 KiB from sp, where the
 * signed form reaches only ±256 from x29. Anything past that goes through
 * a scratch register (see a64_ldr/a64_str).
 */
#ifndef EMBCC_ASM_EMIT_ARM64_H
#define EMBCC_ASM_EMIT_ARM64_H

#include "emit.h"      /* struct code, code_byte, code_patch32 */

/* Register roles. The codegen is deliberately naive (every vreg in a stack
 * slot), so it needs only an accumulator, a second operand and two scratch
 * registers — all in the caller-saved x9..x15 range, so nothing has to be
 * preserved across a call. */
enum {
    A64_ACC   = 9,    /* the accumulator: the x86 backend's rax */
    A64_TMP   = 10,   /* the second operand: the x86 backend's rcx */
    A64_ADDR  = 11,   /* address scratch */
    A64_SCR   = 12,   /* a second scratch (big offsets, indirect targets) */
    A64_SRET  = 8,    /* AAPCS64 indirect result location register */
    A64_FP    = 29,
    A64_LR    = 30,
    A64_SP    = 31,   /* 31 means SP or XZR depending on the instruction */
    A64_ZR    = 31,
    /* The FP scratch pair. v16..v31 are caller-saved AND are never
     * argument registers, so unlike the x86 backend's xmm0/xmm1 these
     * cannot collide with an argument being set up for a call. */
    A64_FACC  = 16,
    A64_FTMP  = 17
};

/* Condition codes, in their architectural encoding. */
enum {
    A64_EQ = 0,  A64_NE = 1,  A64_CS = 2,  A64_CC = 3,
    A64_MI = 4,  A64_PL = 5,  A64_VS = 6,  A64_VC = 7,
    A64_HI = 8,  A64_LS = 9,  A64_GE = 10, A64_LT = 11,
    A64_GT = 12, A64_LE = 13, A64_AL = 14
};

/* One 32-bit instruction word, little-endian. */
void a64_word(struct code *c, unsigned long word);

/* ---- moves ---------------------------------------------------------- */
void a64_mov_reg(struct code *c, int rd, int rm, int w);
/* Any 64-bit constant, via movz + movk (or movn when that is shorter). */
void a64_mov_imm(struct code *c, int rd, long imm, int w);

/* ---- arithmetic and logic ------------------------------------------- */
/* add/sub with a 12-bit unsigned immediate, optionally shifted left 12.
 * Refuses (returns 0) an immediate neither form can hold; callers that may
 * exceed it materialise the value instead. */
int  a64_add_imm(struct code *c, int rd, int rn, long imm, int w);
int  a64_sub_imm(struct code *c, int rd, int rn, long imm, int w);
/* op: '+' '-' '&' '|' '^' */
void a64_alu_reg(struct code *c, int op, int rd, int rn, int rm, int w);
void a64_mul(struct code *c, int rd, int rn, int rm, int w);
void a64_div(struct code *c, int rd, int rn, int rm, int sign, int w);
/* rd = ra - rn*rm — the second half of a remainder. */
void a64_msub(struct code *c, int rd, int rn, int rm, int ra, int w);
/* kind: '<' lsl, '>' asr, 'u' lsr */
void a64_shift_reg(struct code *c, int kind, int rd, int rn, int rm, int w);
void a64_neg(struct code *c, int rd, int rm, int w);
void a64_mvn(struct code *c, int rd, int rm, int w);
/* Byte reverse: size 2/4/8 (__builtin_bswapN). */
void a64_rev(struct code *c, int rd, int rn, int size);

/* ---- compare -------------------------------------------------------- */
void a64_cmp_reg(struct code *c, int rn, int rm, int w);
void a64_cset(struct code *c, int rd, int cond);

/* ---- extension ------------------------------------------------------ */
/* rd = rn re-extended from `size` bytes, signed or not, into a `w`-wide
 * value. A 32-bit result is zero-extended to 64 bits by the hardware, so
 * the unsigned 4-byte case is a plain 32-bit move. */
void a64_extend(struct code *c, int rd, int rn, int size, int sign, int w);

/* ---- memory --------------------------------------------------------- */
/* rt = *(rn + off), extending per size/sign into a `w`-wide value.
 * `off` is a byte offset and may exceed the scaled 12-bit field, in which
 * case the address is formed in A64_SCR first — so A64_SCR must not be
 * live across these calls. */
void a64_ldr(struct code *c, int rt, int rn, long off,
             int size, int sign, int w);
/* *(rn + off) = rt's low `size` bytes. Same A64_SCR caveat. */
void a64_str(struct code *c, int rt, int rn, long off, int size);

/* ---- frame ---------------------------------------------------------- */
/* stp x29,x30,[sp,#-16]! ; mov x29,sp ; sub sp,sp,#framesize */
void a64_prologue(struct code *c, int framesize);
/* add sp,sp,#framesize ; ldp x29,x30,[sp],#16 ; ret */
void a64_epilogue(struct code *c, int framesize);

/* ---- control flow --------------------------------------------------- */
/* Each returns the offset of the instruction word, for later patching. */
int  a64_b(struct code *c);
int  a64_bcond(struct code *c, int cond);
int  a64_cbz(struct code *c, int rt, int nonzero, int w);
int  a64_bl(struct code *c);
void a64_blr(struct code *c, int rn);
void a64_br(struct code *c, int rn);
void a64_ret(struct code *c);
/* Patch a branch at `at` to land on .text offset `target`. */
void a64_patch_b26(struct code *c, int at, int target);
void a64_patch_b19(struct code *c, int at, int target);

/* ---- symbol addresses ----------------------------------------------- */
/* The adrp/add pair that materialises a symbol address. Both fields are
 * left zero for the linker; each returns the instruction offset so the
 * driver can attach ADR_PREL_PG_HI21 and ADD_ABS_LO12_NC relocations. */
int  a64_adrp(struct code *c, int rd);
int  a64_add_lo12(struct code *c, int rd, int rn);

/* adr rd, . — a PC-relative address within ±1 MiB, patched to a .text
 * offset later. Used for the address of a label (GNU &&label), which is
 * always inside the same function. */
int  a64_adr(struct code *c, int rd);
void a64_patch_adr(struct code *c, int at, int target);

/* ---- scalar floating point ------------------------------------------ */
/* `w` is 4 (single, S registers) or 8 (double, D registers) throughout,
 * matching the IR's float width. */
void a64_fldr(struct code *c, int vt, int rn, long off, int w);
void a64_fstr(struct code *c, int vt, int rn, long off, int w);
/* op: '+' '-' '*' '/' */
void a64_falu(struct code *c, int op, int vd, int vn, int vm, int w);
void a64_fneg(struct code *c, int vd, int vn, int w);
void a64_fmov_reg(struct code *c, int vd, int vn, int w);
void a64_fcmp(struct code *c, int vn, int vm, int w);
/* int -> float: rn is a general register of width `iw`, vd an FP register
 * of width `fw`; `sign` picks scvtf over ucvtf. */
void a64_cvt_i2f(struct code *c, int vd, int rn, int sign, int iw, int fw);
/* float -> int, rounding toward zero (C's conversion). */
void a64_cvt_f2i(struct code *c, int rd, int vn, int sign, int iw, int fw);
/* float <-> double. */
void a64_fcvt(struct code *c, int vd, int vn, int from_w, int to_w);

/* ---- misc ----------------------------------------------------------- */
void a64_dmb_ish(struct code *c);   /* __sync_synchronize */
void a64_udf(struct code *c);       /* __builtin_unreachable */

#endif
