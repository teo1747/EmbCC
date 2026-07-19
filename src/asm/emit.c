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

/* SysV order edi,esi,edx,ecx,r8d,r9d: {reg-field, needs REX.R/B} */
static const struct { int reg, rex; } argregs[6] = {
    { 7, 0 }, { 6, 0 }, { 2, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 }
};

void x86_store_arg(struct code *c, int argno, int disp)
{
    if (argregs[argno].rex)
        code_byte(c, 0x44); /* REX.R */
    code_byte(c, 0x89);     /* mov r/m32, r32 */
    modrm_rbp(c, argregs[argno].reg, disp);
}

void x86_load_arg(struct code *c, int argno, int disp)
{
    if (argregs[argno].rex)
        code_byte(c, 0x44); /* REX.R */
    code_byte(c, 0x8b);     /* mov r32, r/m32 */
    modrm_rbp(c, argregs[argno].reg, disp);
}

void x86_mov_eax_imm32(struct code *c, long imm)
{
    code_byte(c, 0xb8); /* mov eax, imm32 */
    code_u32(c, (unsigned long)(unsigned int)imm);
}

void x86_mov_eax_mem(struct code *c, int disp)
{
    code_byte(c, 0x8b); /* mov r32, r/m32 */
    modrm_rbp(c, 0, disp);
}

void x86_mov_mem_eax(struct code *c, int disp)
{
    code_byte(c, 0x89); /* mov r/m32, r32 */
    modrm_rbp(c, 0, disp);
}

void x86_alu_eax_mem(struct code *c, int op, int disp)
{
    switch (op) {
    case '+':
        code_byte(c, 0x03); /* add r32, r/m32 */
        break;
    case '-':
        code_byte(c, 0x2b); /* sub r32, r/m32 */
        break;
    case '*':
        code_byte(c, 0x0f); /* imul r32, r/m32 */
        code_byte(c, 0xaf);
        break;
    default:
        fprintf(stderr, "embcc: internal: no encoding for op '%c'\n", op);
        exit(1);
    }
    modrm_rbp(c, 0, disp);
}

int x86_call_rel32(struct code *c)
{
    code_byte(c, 0xe8);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

void x86_cmp_eax_mem(struct code *c, int disp)
{
    code_byte(c, 0x3b); /* cmp r32, r/m32 */
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

void x86_test_eax(struct code *c)
{
    code_byte(c, 0x85); /* test r/m32, r32 */
    code_byte(c, 0xc0);
}

int x86_jz_rel32(struct code *c)
{
    code_byte(c, 0x0f); /* jz rel32 */
    code_byte(c, 0x84);
    int off = c->len;
    code_u32(c, 0);
    return off;
}

int x86_jmp_rel32(struct code *c)
{
    code_byte(c, 0xe9); /* jmp rel32 */
    int off = c->len;
    code_u32(c, 0);
    return off;
}
