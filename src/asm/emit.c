#include "emit.h"

#include <stdio.h>
#include <stdlib.h>

#include "../driver/util.h"

void code_byte(struct code *c, int b)
{
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->p = xrealloc(c->p, (size_t)c->cap);
    }
    c->p[c->len++] = (unsigned char)b;
}

void code_u32(struct code *c, unsigned long v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
    code_byte(c, (int)((v >> 16) & 0xff));
    code_byte(c, (int)((v >> 24) & 0xff));
}

void code_patch32(struct code *c, int off, unsigned long v)
{
    c->p[off] = (unsigned char)(v & 0xff);
    c->p[off + 1] = (unsigned char)((v >> 8) & 0xff);
    c->p[off + 2] = (unsigned char)((v >> 16) & 0xff);
    c->p[off + 3] = (unsigned char)((v >> 24) & 0xff);
}

void code_align(struct code *c, int align, int fill)
{
    while (c->len % align)
        code_byte(c, fill);
}

/* REX.W when the 64-bit form is wanted */
static void rexw(struct code *c, int w)
{
    if (w == 8)
        code_byte(c, 0x48);
}

/* ModRM for [rbp+disp]: rm=101 with mod=01 (disp8) or mod=10 (disp32). */
static void modrm_rbp(struct code *c, int reg, int disp)
{
    if (disp >= -128 && disp <= 127) {
        code_byte(c, 0x45 | (reg << 3));
        code_byte(c, disp & 0xff);
    } else {
        code_byte(c, 0x85 | (reg << 3));
        code_u32(c, (unsigned long)(unsigned int)disp);
    }
}

/* ModRM for [base+disp] with an arbitrary base register. rsp needs a
 * SIB byte; rbp cannot use the disp-less form (that encoding means
 * RIP-relative), so both take an explicit displacement. */
static void modrm_base(struct code *c, int reg, int base, int disp)
{
    int rm = base & 7;
    int mod;

    if (disp >= -128 && disp <= 127)
        mod = 1;
    else
        mod = 2;
    code_byte(c, (mod << 6) | ((reg & 7) << 3) | rm);
    if (rm == 4)
        code_byte(c, 0x24); /* SIB: base=rsp, no index */
    if (mod == 1)
        code_byte(c, disp & 0xff);
    else
        code_u32(c, (unsigned long)(unsigned int)disp);
}

/* REX for a (reg, base) pair; emitted when 64-bit or when either
 * register is r8..r15 (none are used here, but the bits are correct). */
static void rex_rb(struct code *c, int w64, int reg, int base)
{
    int rex = 0x40 | (w64 ? 8 : 0) | ((reg & 8) ? 4 : 0) |
              ((base & 8) ? 1 : 0);
    if (rex != 0x40)
        code_byte(c, rex);
}

void x86_load_reg_mem(struct code *c, int dst, int base, int disp,
                      int size)
{
    switch (size) {
    case 1:
        rex_rb(c, 0, dst, base);
        code_byte(c, 0x0f);
        code_byte(c, 0xb6); /* movzx r32, r/m8 */
        break;
    case 2:
        rex_rb(c, 0, dst, base);
        code_byte(c, 0x0f);
        code_byte(c, 0xb7); /* movzx r32, r/m16 */
        break;
    case 4:
        rex_rb(c, 0, dst, base);
        code_byte(c, 0x8b);
        break;
    case 8:
        rex_rb(c, 1, dst, base);
        code_byte(c, 0x8b);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad load size %d\n", size);
        exit(1);
    }
    modrm_base(c, dst, base, disp);
}

void x86_store_mem_reg(struct code *c, int base, int disp, int src,
                       int size)
{
    switch (size) {
    case 1:
        rex_rb(c, 0, src, base);
        code_byte(c, 0x88);
        break;
    case 2:
        code_byte(c, 0x66);
        rex_rb(c, 0, src, base);
        code_byte(c, 0x89);
        break;
    case 4:
        rex_rb(c, 0, src, base);
        code_byte(c, 0x89);
        break;
    case 8:
        rex_rb(c, 1, src, base);
        code_byte(c, 0x89);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad store size %d\n", size);
        exit(1);
    }
    modrm_base(c, src, base, disp);
}

void x86_movs_load_base(struct code *c, int xmm, int base, int disp,
                        int w)
{
    code_byte(c, w == 4 ? 0xf3 : 0xf2);
    code_byte(c, 0x0f);
    code_byte(c, 0x10);
    modrm_base(c, xmm, base, disp);
}

void x86_movs_store_base(struct code *c, int base, int disp, int xmm,
                         int w)
{
    code_byte(c, w == 4 ? 0xf3 : 0xf2);
    code_byte(c, 0x0f);
    code_byte(c, 0x11);
    modrm_base(c, xmm, base, disp);
}

void x86_lea_reg_slot(struct code *c, int dst, int disp)
{
    rex_rb(c, 1, dst, REG_RBP);
    code_byte(c, 0x8d);
    modrm_base(c, dst, REG_RBP, disp);
}

void x86_mov_reg_reg(struct code *c, int dst, int src)
{
    rex_rb(c, 1, src, dst);
    code_byte(c, 0x89); /* mov r/m64, r64 */
    code_byte(c, 0xc0 | ((src & 7) << 3) | (dst & 7));
}

/* dst = src, w-bit. w==4 leaves a 32-bit mov (zeroing the upper half — the
 * register allocator's "narrow value is zero-extended" invariant); w==8 is a
 * full 64-bit copy. */
void x86_mov_rr_w(struct code *c, int dst, int src, int w)
{
    rex_rb(c, w == 8, src, dst);
    code_byte(c, 0x89); /* mov r/m, r : reg=src, rm=dst */
    code_byte(c, 0xc0 | ((src & 7) << 3) | (dst & 7));
}

/* dst64 = sign-extend(src's low 32 bits) — movsxd. */
void x86_movsxd_rr(struct code *c, int dst, int src)
{
    rex_rb(c, 1, dst, src);
    code_byte(c, 0x63); /* movsxd reg, r/m32 : reg=dst, rm=src */
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* dst = extend(the low `size` (1 or 2) bytes of src) to width w — movsx/movzx,
 * the reg-reg twin of x86_load_slot's narrow cases. */
void x86_movx_rr(struct code *c, int dst, int src, int size, int sign, int w)
{
    rex_rb(c, w == 8, dst, src);
    code_byte(c, 0x0f);
    if (size == 1)
        code_byte(c, sign ? 0xbe : 0xb6); /* movsx/movzx r, r/m8 */
    else
        code_byte(c, sign ? 0xbf : 0xb7); /* movsx/movzx r, r/m16 */
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* dst op= src (+ - * & | ^), w-bit — the reg-reg twin of x86_alu_eax_mem, same
 * "r, r/m" opcodes with reg=dst, rm=src. */
void x86_alu_rr(struct code *c, int op, int dst, int src, int w)
{
    rex_rb(c, w == 8, dst, src);
    switch (op) {
    case '+': code_byte(c, 0x03); break;
    case '-': code_byte(c, 0x2b); break;
    case '*': code_byte(c, 0x0f); code_byte(c, 0xaf); break;
    case '&': code_byte(c, 0x23); break;
    case '|': code_byte(c, 0x0b); break;
    case '^': code_byte(c, 0x33); break;
    default:
        fprintf(stderr, "embcc: internal: no reg-reg encoding for '%c'\n", op);
        exit(1);
    }
    code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
}

/* group-1 ALU `reg OP= imm` (add/sub/and/or/xor, and cmp via op 'c'): the imm8
 * form (83 /ext ib, sign-extended) when the value fits, else imm32 (81 /ext id).
 * Works for any register including rax — shorter than materialising the constant
 * in a scratch register first. */
void x86_alu_reg_imm(struct code *c, int op, int reg, long imm, int w)
{
    int ext;
    switch (op) {
    case '+': ext = 0; break;
    case '|': ext = 1; break;
    case '&': ext = 4; break;
    case '-': ext = 5; break;
    case '^': ext = 6; break;
    case 'c': ext = 7; break;   /* cmp */
    default:
        fprintf(stderr, "embcc: internal: no reg-imm encoding for '%c'\n", op);
        exit(1);
    }
    rex_rb(c, w == 8, 0, reg);   /* reg is the r/m operand -> REX.B */
    if (imm >= -128 && imm <= 127) {
        code_byte(c, 0x83);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
        code_byte(c, (int)(imm & 0xff));
    } else {
        code_byte(c, 0x81);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
        code_u32(c, (unsigned long)imm);
    }
}

/* imul dst, src, imm — the three-operand form: dst = src * imm, so dst need not
 * equal src and nothing routes through rax. imm8 (6b) when it fits, else imm32
 * (69). */
void x86_imul_reg_imm(struct code *c, int dst, int src, long imm, int w)
{
    rex_rb(c, w == 8, dst, src);   /* dst -> REX.R, src -> REX.B */
    if (imm >= -128 && imm <= 127) {
        code_byte(c, 0x6b);
        code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
        code_byte(c, (int)(imm & 0xff));
    } else {
        code_byte(c, 0x69);
        code_byte(c, 0xc0 | ((dst & 7) << 3) | (src & 7));
        code_u32(c, (unsigned long)imm);
    }
}

/* test reg, reg — ZF/SF from the value itself, the compact `cmp reg, 0`. */
void x86_test_reg(struct code *c, int reg, int w)
{
    rex_rb(c, w == 8, reg, reg);
    code_byte(c, 0x85);
    code_byte(c, 0xc0 | ((reg & 7) << 3) | (reg & 7));
}

/* cmp a, b (computes a - b, sets flags) — reg-reg twin of x86_cmp_eax_mem. */
void x86_cmp_rr(struct code *c, int a, int b, int w)
{
    rex_rb(c, w == 8, a, b);
    code_byte(c, 0x3b); /* cmp r, r/m : reg=a, rm=b */
    code_byte(c, 0xc0 | ((a & 7) << 3) | (b & 7));
}

/* [rdx:rax] / src -> quotient rax, remainder rdx (idiv /7, div /6). */
void x86_div_rr(struct code *c, int src, int sign, int w)
{
    rex_rb(c, w == 8, 0, src);
    code_byte(c, 0xf7);
    code_byte(c, 0xc0 | ((sign ? 7 : 6) << 3) | (src & 7));
}

int x86_argreg(int index)
{
    static const int regs[6] = { REG_RDI, REG_RSI, REG_RDX, REG_RCX,
                                 8 /* r8 */, 9 /* r9 */ };
    return regs[index];
}

void x86_prologue(struct code *c, int framesize)
{
    if (framesize % 16 != 0) {
        fprintf(stderr, "embcc: internal: frame size %d not 16-aligned\n",
                framesize);
        exit(1);
    }
    code_byte(c, 0x55);                     /* push rbp */
    code_byte(c, 0x48); code_byte(c, 0x89); /* mov rbp, rsp */
    code_byte(c, 0xe5);
    if (framesize > 0) {
        code_byte(c, 0x48); code_byte(c, 0x81); /* sub rsp, imm32 */
        code_byte(c, 0xec);
        code_u32(c, (unsigned long)framesize);
    }
}

void x86_epilogue(struct code *c)
{
    code_byte(c, 0xc9); /* leave */
    code_byte(c, 0xc3); /* ret */
}

/* SysV order rdi,rsi,rdx,rcx,r8,r9: {reg-field, needs REX.R/B} */
static const struct { int reg, rex; } argregs[6] = {
    { 7, 0 }, { 6, 0 }, { 2, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }
};

void x86_store_arg(struct code *c, int argno, int disp)
{
    code_byte(c, argregs[argno].rex ? 0x4c : 0x48); /* REX.W (+R) */
    code_byte(c, 0x89); /* mov r/m64, r64 */
    modrm_rbp(c, argregs[argno].reg, disp);
}

void x86_load_arg(struct code *c, int argno, int disp)
{
    code_byte(c, argregs[argno].rex ? 0x4c : 0x48);
    code_byte(c, 0x8b); /* mov r64, r/m64 */
    modrm_rbp(c, argregs[argno].reg, disp);
}

void x86_mov_eax_imm(struct code *c, long imm, int w)
{
    if (w == 4) {
        code_byte(c, 0xb8); /* mov eax, imm32 */
        code_u32(c, (unsigned long)imm);
        return;
    }
    if (imm >= -2147483647L - 1 && imm <= 2147483647L) {
        code_byte(c, 0x48); /* mov rax, imm32 (sign-extended) */
        code_byte(c, 0xc7);
        code_byte(c, 0xc0);
        code_u32(c, (unsigned long)imm);
        return;
    }
    code_byte(c, 0x48); /* movabs rax, imm64 */
    code_byte(c, 0xb8);
    code_u32(c, (unsigned long)imm & 0xffffffffUL);
    code_u32(c, ((unsigned long)imm >> 32) & 0xffffffffUL);
}

void x86_load_slot(struct code *c, int disp, int size, int sign, int w)
{
    switch (size) {
    case 1:
        rexw(c, w);
        code_byte(c, 0x0f); /* movsx/movzx r, r/m8 */
        code_byte(c, sign ? 0xbe : 0xb6);
        modrm_rbp(c, 0, disp);
        break;
    case 2:
        rexw(c, w);
        code_byte(c, 0x0f); /* movsx/movzx r, r/m16 */
        code_byte(c, sign ? 0xbf : 0xb7);
        modrm_rbp(c, 0, disp);
        break;
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48); /* movsxd rax, r/m32 */
            code_byte(c, 0x63);
        } else {
            /* 32-bit mov zeroes the upper half — the unsigned extend */
            code_byte(c, 0x8b);
        }
        modrm_rbp(c, 0, disp);
        break;
    case 8:
        code_byte(c, 0x48);
        code_byte(c, 0x8b); /* mov rax, r/m64 */
        modrm_rbp(c, 0, disp);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad load size %d\n", size);
        exit(1);
    }
}

void x86_store_slot(struct code *c, int disp, int size)
{
    switch (size) {
    case 1:
        code_byte(c, 0x88); /* mov r/m8, al */
        break;
    case 2:
        code_byte(c, 0x66); /* mov r/m16, ax */
        code_byte(c, 0x89);
        break;
    case 4:
        code_byte(c, 0x89); /* mov r/m32, eax */
        break;
    case 8:
        code_byte(c, 0x48); /* mov r/m64, rax */
        code_byte(c, 0x89);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad store size %d\n", size);
        exit(1);
    }
    modrm_rbp(c, 0, disp);
}

void x86_load_mem_rax(struct code *c, int size, int sign, int w)
{
    /* same matrix as x86_load_slot with ModRM 00 = [rax] */
    switch (size) {
    case 1:
        rexw(c, w);
        code_byte(c, 0x0f);
        code_byte(c, sign ? 0xbe : 0xb6);
        code_byte(c, 0x00);
        break;
    case 2:
        rexw(c, w);
        code_byte(c, 0x0f);
        code_byte(c, sign ? 0xbf : 0xb7);
        code_byte(c, 0x00);
        break;
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48);
            code_byte(c, 0x63); /* movsxd rax, [rax] */
        } else {
            code_byte(c, 0x8b);
        }
        code_byte(c, 0x00);
        break;
    case 8:
        code_byte(c, 0x48);
        code_byte(c, 0x8b);
        code_byte(c, 0x00);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad load size %d\n", size);
        exit(1);
    }
}

/* ModRM for [base] (disp 0) with register field `reg`, in the shortest correct
 * form. The r/m encoding has two traps: base whose low 3 bits are 100 (rsp/r12)
 * needs a SIB byte, and 101 (rbp/r13) collides with RIP-relative at mod=00 so it
 * takes a mod=01 disp8 of zero. */
static void modrm_base0(struct code *c, int reg, int base)
{
    int lo = base & 7;
    if (lo == 5) {          /* rbp / r13 */
        code_byte(c, (1 << 6) | ((reg & 7) << 3) | 5);
        code_byte(c, 0x00);
    } else if (lo == 4) {   /* rsp / r12 */
        code_byte(c, ((reg & 7) << 3) | 4);
        code_byte(c, 0x24); /* SIB: base=r/sp/r12, no index */
    } else {
        code_byte(c, ((reg & 7) << 3) | lo);
    }
}

/* ModRM+SIB for [base + index*scale], disp 0, register field `reg`. Always uses
 * a SIB byte (rm=100). A base whose low 3 bits are 101 (rbp/r13) still needs a
 * mod=01 disp8=0. The index is an allocated register, never rsp, so rm-index 100
 * (= "no index") never collides. */
static void modrm_baseindex0(struct code *c, int reg, int base, int index,
                             int scale)
{
    int ss = scale == 8 ? 3 : scale == 4 ? 2 : scale == 2 ? 1 : 0;
    int mod = (base & 7) == 5 ? 1 : 0;
    code_byte(c, (mod << 6) | ((reg & 7) << 3) | 4);          /* rm=100 -> SIB */
    code_byte(c, (ss << 6) | ((index & 7) << 3) | (base & 7));
    if (mod == 1)
        code_byte(c, 0x00);
}

/* Load into rax from [base + index*scale] (scale 1/2/4/8), same extension matrix
 * as x86_load_mem_rax — folds an address computation into the load. REX.X/REX.B
 * carry high index/base registers. */
void x86_load_baseindex_rax(struct code *c, int base, int index, int scale,
                            int size, int sign, int w)
{
    int rexXB = ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexXB;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_baseindex0(c, 0, base, index, scale);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexXB);
            code_byte(c, 0x63);
        } else {
            int rex = 0x40 | rexXB;
            if (rex != 0x40) code_byte(c, rex);
            code_byte(c, 0x8b);
        }
        modrm_baseindex0(c, 0, base, index, scale);
        break;
    case 8:
        code_byte(c, 0x48 | rexXB);
        code_byte(c, 0x8b);
        modrm_baseindex0(c, 0, base, index, scale);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad load size %d\n", size);
        exit(1);
    }
}

/* Load into rax from [base + disp], same extension matrix as x86_load_mem_rax —
 * folds a constant-offset address (a struct field, `p->m`) into the load.
 * modrm_base carries the disp and the rsp/r12 SIB case; REX.B a high base. */
void x86_load_basedisp_rax(struct code *c, int base, int disp,
                           int size, int sign, int w)
{
    int rexb = (base & 8) ? 1 : 0;
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexb;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_base(c, 0, base, disp);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexb);
            code_byte(c, 0x63);
        } else {
            int rex = 0x40 | rexb;
            if (rex != 0x40) code_byte(c, rex);
            code_byte(c, 0x8b);
        }
        modrm_base(c, 0, base, disp);
        break;
    case 8:
        code_byte(c, 0x48 | rexb);
        code_byte(c, 0x8b);
        modrm_base(c, 0, base, disp);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad load size %d\n", size);
        exit(1);
    }
}

/* Load into rax straight from [base], with the same extension matrix as
 * x86_load_mem_rax — no `mov base,rax` first. REX.B carries a high base
 * (r8..r15); modrm_base0 handles the rsp/rbp/r12/r13 addressing traps. */
void x86_load_base_rax(struct code *c, int base, int size, int sign, int w)
{
    int rexb = (base & 8) ? 1 : 0;
    switch (size) {
    case 1:
    case 2: {
        int rex = 0x40 | (w == 8 ? 8 : 0) | rexb;
        if (rex != 0x40) code_byte(c, rex);
        code_byte(c, 0x0f);
        code_byte(c, size == 1 ? (sign ? 0xbe : 0xb6) : (sign ? 0xbf : 0xb7));
        modrm_base0(c, 0, base);
        break;
    }
    case 4:
        if (w == 8 && sign) {
            code_byte(c, 0x48 | rexb);
            code_byte(c, 0x63);      /* movsxd rax, [base] */
        } else {
            if (rexb) code_byte(c, 0x41);
            code_byte(c, 0x8b);
        }
        modrm_base0(c, 0, base);
        break;
    case 8:
        code_byte(c, 0x48 | rexb);
        code_byte(c, 0x8b);
        modrm_base0(c, 0, base);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad load size %d\n", size);
        exit(1);
    }
}

void x86_store_mem_rcx(struct code *c, int size)
{
    switch (size) {
    case 1:
        code_byte(c, 0x88); /* mov [rcx], al */
        break;
    case 2:
        code_byte(c, 0x66);
        code_byte(c, 0x89);
        break;
    case 4:
        code_byte(c, 0x89);
        break;
    case 8:
        code_byte(c, 0x48);
        code_byte(c, 0x89);
        break;
    default:
        fprintf(stderr, "embcc: internal: bad store size %d\n", size);
        exit(1);
    }
    code_byte(c, 0x01); /* ModRM: [rcx], eax/rax */
}

void x86_mov_rcx_slot(struct code *c, int disp)
{
    code_byte(c, 0x48);
    code_byte(c, 0x8b); /* mov rcx, [rbp+disp] */
    modrm_rbp(c, 1, disp);
}

void x86_lea_rax_slot(struct code *c, int disp)
{
    code_byte(c, 0x48);
    code_byte(c, 0x8d); /* lea rax, [rbp+disp] */
    modrm_rbp(c, 0, disp);
}

int x86_lea_rax_rip(struct code *c)
{
    code_byte(c, 0x48);
    code_byte(c, 0x8d); /* lea rax, [rip+rel32] */
    code_byte(c, 0x05); /* ModRM: mod=00 rm=101 = RIP-relative */
    int off = c->len;
    code_u32(c, 0);
    return off;
}

void x86_zero_eax(struct code *c)
{
    code_byte(c, 0x31); /* xor eax, eax */
    code_byte(c, 0xc0);
}

void x86_alu_eax_mem(struct code *c, int op, int disp, int w)
{
    rexw(c, w);
    switch (op) {
    case '+':
        code_byte(c, 0x03); /* add r, r/m */
        break;
    case '-':
        code_byte(c, 0x2b); /* sub r, r/m */
        break;
    case '*':
        code_byte(c, 0x0f); /* imul r, r/m */
        code_byte(c, 0xaf);
        break;
    case '&':
        code_byte(c, 0x23); /* and r, r/m */
        break;
    case '|':
        code_byte(c, 0x0b); /* or r, r/m */
        break;
    case '^':
        code_byte(c, 0x33); /* xor r, r/m */
        break;
    default:
        fprintf(stderr, "embcc: internal: no encoding for op '%c'\n", op);
        exit(1);
    }
    modrm_rbp(c, 0, disp);
}

void x86_cdq(struct code *c, int w)
{
    rexw(c, w); /* cqo when 64-bit */
    code_byte(c, 0x99);
}

void x86_zero_edx(struct code *c)
{
    code_byte(c, 0x31); /* xor edx, edx (also clears upper rdx) */
    code_byte(c, 0xd2);
}

void x86_div_mem(struct code *c, int disp, int sign, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* idiv is /7, div is /6 */
    modrm_rbp(c, sign ? 7 : 6, disp);
}

void x86_mov_eax_edx(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0x89); /* mov eax/rax, edx/rdx */
    code_byte(c, 0xd0);
}

void x86_mov_ecx_mem(struct code *c, int disp, int w)
{
    rexw(c, w);
    code_byte(c, 0x8b); /* mov ecx/rcx, r/m */
    modrm_rbp(c, 1, disp);
}

void x86_shift_eax_cl(struct code *c, int kind, int w)
{
    rexw(c, w);
    code_byte(c, 0xd3); /* group 2, count in cl */
    switch (kind) {
    case '<':
        code_byte(c, 0xe0); /* shl: /4 */
        break;
    case '>':
        code_byte(c, 0xf8); /* sar: /7 */
        break;
    case 'u':
        code_byte(c, 0xe8); /* shr: /5 */
        break;
    default:
        fprintf(stderr, "embcc: internal: bad shift kind\n");
        exit(1);
    }
}

/* shift `reg` by a constant: the 1-count short form (D1 /ext) or the imm8 form
 * (C1 /ext ib). kind: '<' shl, '>' sar, 'u' shr — the twin of x86_shift_eax_cl. */
void x86_shift_reg_imm(struct code *c, int reg, int kind, int count, int w)
{
    int ext = kind == '<' ? 4 : kind == 'u' ? 5 : 7;   /* shl:/4 shr:/5 sar:/7 */
    rex_rb(c, w == 8, 0, reg);
    if (count == 1) {
        code_byte(c, 0xd1);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
    } else {
        code_byte(c, 0xc1);
        code_byte(c, 0xc0 | (ext << 3) | (reg & 7));
        code_byte(c, count & 0xff);
    }
}

void x86_neg_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* neg: /3 */
    code_byte(c, 0xd8);
}

/* Byte-swap the low `size` bytes of rax/eax/ax in place. bswap has no
 * 16-bit form, so a 2-byte swap is `rol $8, %ax`. */
void x86_bswap(struct code *c, int size)
{
    if (size == 2) {
        code_byte(c, 0x66);   /* operand-size prefix: 16-bit */
        code_byte(c, 0xc1);   /* rol r/m16, imm8 */
        code_byte(c, 0xc0);   /* mod=11 /0 reg=ax */
        code_byte(c, 0x08);
        return;
    }
    if (size == 8)
        code_byte(c, 0x48);   /* REX.W: bswap %rax */
    code_byte(c, 0x0f);
    code_byte(c, 0xc8);       /* bswap eax/rax */
}

/* mfence — a full memory barrier (__sync_synchronize). */
void x86_mfence(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0xae);
    code_byte(c, 0xf0);
}

/* ud2 — the guaranteed-undefined instruction (__builtin_unreachable). */
void x86_ud2(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x0b);
}

/* xchg rax/eax/ax/al with [rcx] — the memory operand makes it implicitly
 * LOCKed, i.e. atomic. RAX ends holding the old value at [rcx]. */
void x86_xchg_rax_mem_rcx(struct code *c, int size)
{
    switch (size) {
    case 1: code_byte(c, 0x86); break;
    case 2: code_byte(c, 0x66); code_byte(c, 0x87); break;
    case 4: code_byte(c, 0x87); break;
    case 8: code_byte(c, 0x48); code_byte(c, 0x87); break;
    default:
        fprintf(stderr, "embcc: internal: bad xchg size %d\n", size);
        exit(1);
    }
    code_byte(c, 0x01); /* ModRM: [rcx] <-> eax/rax */
}

/* lock xadd %rax/eax/ax/al, (%rcx): atomically *rcx += rax, rax = old *rcx. */
void x86_lock_xadd_rcx(struct code *c, int size)
{
    code_byte(c, 0xf0);                       /* LOCK */
    if (size == 2) code_byte(c, 0x66);
    if (size == 8) code_byte(c, 0x48);        /* REX.W */
    code_byte(c, 0x0f);
    code_byte(c, size == 1 ? 0xc0 : 0xc1);
    code_byte(c, 0x01);                       /* ModRM: reg=rax, [rcx] */
}

/* lock cmpxchg %rdx/edx/dx/dl, (%rcx): compare RAX with *rcx; if equal set
 * *rcx = RDX and ZF, else load *rcx into RAX and clear ZF. Atomic. */
void x86_lock_cmpxchg_rcx(struct code *c, int size)
{
    code_byte(c, 0xf0);                       /* LOCK */
    if (size == 2) code_byte(c, 0x66);
    if (size == 8) code_byte(c, 0x48);        /* REX.W */
    code_byte(c, 0x0f);
    code_byte(c, size == 1 ? 0xb0 : 0xb1);
    code_byte(c, 0x11);                       /* ModRM: reg=rdx, [rcx] */
}

void x86_not_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* not: /2 */
    code_byte(c, 0xd0);
}

/* The SSE2 prefix that selects scalar single vs scalar double. */
static void sse_prefix(struct code *c, int w)
{
    code_byte(c, w == 4 ? 0xf3 : 0xf2);
}

void x86_movs_load(struct code *c, int xmm, int disp, int w)
{
    sse_prefix(c, w);
    code_byte(c, 0x0f);
    code_byte(c, 0x10); /* movss/movsd xmm, m */
    modrm_rbp(c, xmm, disp);
}

void x86_movs_store(struct code *c, int xmm, int disp, int w)
{
    sse_prefix(c, w);
    code_byte(c, 0x0f);
    code_byte(c, 0x11); /* movss/movsd m, xmm */
    modrm_rbp(c, xmm, disp);
}

void x86_sse_alu_mem(struct code *c, int op, int disp, int w)
{
    sse_prefix(c, w);
    code_byte(c, 0x0f);
    switch (op) {
    case '+': code_byte(c, 0x58); break; /* addss/addsd */
    case '-': code_byte(c, 0x5c); break; /* subss/subsd */
    case '*': code_byte(c, 0x59); break; /* mulss/mulsd */
    case '/': code_byte(c, 0x5e); break; /* divss/divsd */
    default:
        fprintf(stderr, "embcc: internal: no SSE encoding for '%c'\n", op);
        exit(1);
    }
    modrm_rbp(c, 0, disp); /* always xmm0 */
}

void x86_ucomis_mem(struct code *c, int disp, int w)
{
    if (w == 8)
        code_byte(c, 0x66); /* ucomisd */
    code_byte(c, 0x0f);
    code_byte(c, 0x2e);
    modrm_rbp(c, 0, disp);
}

void x86_set_float_eq(struct code *c, int ne)
{
    /* ucomis sets ZF=PF=CF=1 for unordered. == must be false for NaN,
     * != must be true, so neither is a single setcc. */
    code_byte(c, 0x0f);
    code_byte(c, ne ? 0x95 : 0x94); /* setne/sete al */
    code_byte(c, 0xc0);
    code_byte(c, 0x0f);
    code_byte(c, ne ? 0x9a : 0x9b); /* setp/setnp cl */
    code_byte(c, 0xc1);
    code_byte(c, ne ? 0x08 : 0x20); /* or/and al, cl */
    code_byte(c, 0xc8);
    code_byte(c, 0x0f); /* movzx eax, al */
    code_byte(c, 0xb6);
    code_byte(c, 0xc0);
}

void x86_cvtsi2s(struct code *c, int disp, int srcw, int dstw)
{
    sse_prefix(c, dstw);
    if (srcw == 8)
        code_byte(c, 0x48); /* REX.W: 64-bit integer source */
    code_byte(c, 0x0f);
    code_byte(c, 0x2a); /* cvtsi2ss/cvtsi2sd xmm0, r/m */
    modrm_rbp(c, 0, disp);
}

void x86_cvtts2si(struct code *c, int disp, int srcw, int dstw)
{
    sse_prefix(c, srcw);
    if (dstw == 8)
        code_byte(c, 0x48); /* REX.W: 64-bit integer destination */
    code_byte(c, 0x0f);
    code_byte(c, 0x2c); /* cvttss2si/cvttsd2si rax, xmm/m (truncating) */
    modrm_rbp(c, 0, disp);
}

void x86_cvts2s(struct code *c, int disp, int srcw)
{
    sse_prefix(c, srcw);
    code_byte(c, 0x0f);
    code_byte(c, 0x5a); /* cvtss2sd / cvtsd2ss */
    modrm_rbp(c, 0, disp);
}

void x86_mov_al_imm(struct code *c, int v)
{
    code_byte(c, 0xb0); /* mov al, imm8 */
    code_byte(c, v & 0xff);
}

void x86_cmp_eax_mem(struct code *c, int disp, int w)
{
    rexw(c, w);
    code_byte(c, 0x3b); /* cmp r, r/m */
    modrm_rbp(c, 0, disp);
}

void x86_setcc_eax(struct code *c, int cc)
{
    code_byte(c, 0x0f); /* setcc al */
    code_byte(c, cc);
    code_byte(c, 0xc0);
    code_byte(c, 0x0f); /* movzx eax, al */
    code_byte(c, 0xb6);
    code_byte(c, 0xc0);
}

void x86_test_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0x85); /* test r/m, r */
    code_byte(c, 0xc0);
}

int x86_jz_rel32(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x84);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

int x86_jnz_rel32(struct code *c)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x85); /* jnz rel32 */
    int off = c->len;
    code_u32(c, 0);
    return off;
}

int x86_jmp_rel32(struct code *c)
{
    code_byte(c, 0xe9);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

/* Conditional jump rel32 for a setcc condition byte (0x9x, as cc_for returns):
 * the Jcc opcode shares the condition's low nibble (0f 8x). Patch offset back. */
int x86_jcc_rel32(struct code *c, int setcc)
{
    code_byte(c, 0x0f);
    code_byte(c, 0x80 | (setcc & 0x0f));
    int off = c->len;
    code_u32(c, 0);
    return off;
}

int x86_call_rel32(struct code *c)
{
    code_byte(c, 0xe8);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

void x86_mov_r11_slot(struct code *c, int disp)
{
    code_byte(c, 0x4c); /* REX.WR: mov r11, [rbp+disp] */
    code_byte(c, 0x8b);
    modrm_rbp(c, 3, disp);
}

void x86_call_r11(struct code *c)
{
    code_byte(c, 0x41); /* REX.B */
    code_byte(c, 0xff); /* call r/m64: /2 */
    code_byte(c, 0xd3);
}
