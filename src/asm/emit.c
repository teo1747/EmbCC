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

void x86_neg_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* neg: /3 */
    code_byte(c, 0xd8);
}

void x86_not_eax(struct code *c, int w)
{
    rexw(c, w);
    code_byte(c, 0xf7); /* not: /2 */
    code_byte(c, 0xd0);
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

int x86_jmp_rel32(struct code *c)
{
    code_byte(c, 0xe9);
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
