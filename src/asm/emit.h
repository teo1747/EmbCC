/* x86-64 instruction encoding (ARCHITECTURE §2: integrated — no external
 * assembler exists on-OS). Exactly the encodings codegen emits; an
 * instruction this file cannot encode is a missing function, which
 * fails at build time — never a silently wrong byte sequence.
 *
 * Width model mirrors the IR's: `w` is 4 or 8 and chooses the 32- or
 * 64-bit form (REX.W). Memory accesses take a `size` of 1/2/4/8 with a
 * `sign` for the extending loads. Frame slots are [rbp+disp]; loads
 * land in eax/rax, the shift count goes through cl, store addresses
 * through rcx.
 */
#ifndef EMBCC_ASM_EMIT_H
#define EMBCC_ASM_EMIT_H

struct code {
    unsigned char *p;
    int len, cap;
};

void code_byte(struct code *c, int b);
void code_u32(struct code *c, unsigned long v);          /* little-endian */
void code_patch32(struct code *c, int off, unsigned long v);
void code_align(struct code *c, int align, int fill);

/* prologue: push rbp; mov rbp,rsp; sub rsp,framesize (multiple of 16
 * so rsp stays 16-aligned at calls). epilogue: leave; ret. */
void x86_prologue(struct code *c, int framesize);
void x86_epilogue(struct code *c);

/* SysV integer argument registers, index 0..5 = rdi,rsi,rdx,rcx,r8,r9.
 * Always full 64-bit moves: narrower argument types occupy the low
 * bytes and the ABI leaves the upper bits unspecified. */
void x86_store_arg(struct code *c, int argno, int disp);
void x86_load_arg(struct code *c, int argno, int disp);

/* mov eax/rax, imm — picks the shortest correct encoding. */
void x86_mov_eax_imm(struct code *c, long imm, int w);

/* Extending load from a frame slot into eax/rax: size 1/2/4/8, sign
 * for movsx vs movzx, w = destination class. */
void x86_load_slot(struct code *c, int disp, int size, int sign, int w);
/* Truncating store of eax/rax's low `size` bytes to a frame slot. */
void x86_store_slot(struct code *c, int disp, int size);

/* Loads/stores through a pointer: address in rax (load) or rcx (store,
 * value in eax/rax). */
void x86_load_mem_rax(struct code *c, int size, int sign, int w);
void x86_store_mem_rcx(struct code *c, int size);
void x86_mov_rcx_slot(struct code *c, int disp);  /* mov rcx,[rbp+disp] */
void x86_lea_rax_slot(struct code *c, int disp);  /* lea rax,[rbp+disp] */
/* lea rax,[rip+0]; returns the rel32 patch offset (for a relocation). */
int x86_lea_rax_rip(struct code *c);
void x86_zero_eax(struct code *c); /* xor eax,eax — al=0 for varargs calls */

void x86_alu_eax_mem(struct code *c, int op, int disp, int w); /* + - * & | ^ */
void x86_cdq(struct code *c, int w);              /* cdq / cqo */
void x86_zero_edx(struct code *c);                /* xor edx,edx */
void x86_div_mem(struct code *c, int disp, int sign, int w); /* idiv / div */
void x86_mov_eax_edx(struct code *c, int w);      /* remainder to eax */
void x86_mov_ecx_mem(struct code *c, int disp, int w);
void x86_shift_eax_cl(struct code *c, int kind, int w); /* '<' shl, '>' sar, 'u' shr */
void x86_neg_eax(struct code *c, int w);
void x86_not_eax(struct code *c, int w);

/* cmp eax/rax with a slot, then set al by condition and zero-extend.
 * cc is the setcc opcode byte (0x92..0x9f), chosen by codegen. */
void x86_cmp_eax_mem(struct code *c, int disp, int w);
void x86_setcc_eax(struct code *c, int cc);

/* Branches: test eax/rax; jz/jmp with a zero rel32 placeholder — both
 * return the patch offset, resolved per-function by codegen. */
void x86_test_eax(struct code *c, int w);
int x86_jz_rel32(struct code *c);
int x86_jmp_rel32(struct code *c);

/* call rel32 with a zero placeholder; returns the rel32 field offset. */
int x86_call_rel32(struct code *c);

#endif
