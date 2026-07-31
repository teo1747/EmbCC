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

/* ---- SSE2 scalar floating point ----------------------------------
 * w is 4 (single, 'ss') or 8 (double, 'sd'). Values live in the same
 * stack slots as everything else — a float slot just holds the raw bit
 * pattern, which is why loads, stores and constants need no float path
 * at all: only the ARITHMETIC has to reach xmm. */
void x86_movs_load(struct code *c, int xmm, int disp, int w);
void x86_movs_store(struct code *c, int xmm, int disp, int w);
void x86_sse_alu_mem(struct code *c, int op, int disp, int w); /* + - * / */
void x86_ucomis_mem(struct code *c, int disp, int w);
/* setcc pair for float == and != : ordered equality is "equal AND not
 * unordered", because a NaN compares equal to nothing, itself included. */
void x86_set_float_eq(struct code *c, int ne);
void x86_cvtsi2s(struct code *c, int disp, int srcw, int dstw);
void x86_cvtts2si(struct code *c, int disp, int srcw, int dstw);
void x86_cvts2s(struct code *c, int disp, int srcw);
void x86_mov_al_imm(struct code *c, int v); /* varargs: xmm count in al */

/* ---- general [base+disp] addressing, for struct traffic ----------
 * Register numbers are the encoding's: rax0 rcx1 rdx2 rbx3 rsp4 rbp5
 * rsi6 rdi7. Sizes are 1/2/4/8. These exist because a struct copy and
 * an aggregate argument address memory through a POINTER, not through
 * the frame pointer everything else uses. */
#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RSP 4
#define REG_RBP 5
#define REG_RSI 6
#define REG_RDI 7
void x86_load_reg_mem(struct code *c, int dst, int base, int disp, int size);
void x86_store_mem_reg(struct code *c, int base, int disp, int src, int size);
void x86_movs_load_base(struct code *c, int xmm, int base, int disp, int w);
void x86_movs_store_base(struct code *c, int base, int disp, int xmm, int w);
void x86_lea_reg_slot(struct code *c, int dst, int disp); /* lea r,[rbp+d] */
void x86_mov_reg_reg(struct code *c, int dst, int src);   /* 64-bit */
/* register-register forms for the -O2 register allocator (values live in
 * callee-saved regs, not memory). All operate on register NUMBERS 0..15. */
void x86_mov_rr_w(struct code *c, int dst, int src, int w);   /* dst=src, 32/64 */
void x86_movsxd_rr(struct code *c, int dst, int src);         /* dst64=sext(src32) */
void x86_movx_rr(struct code *c, int dst, int src, int size, int sign, int w);
                                                    /* dst = extend(src low 1/2 bytes) */
void x86_alu_rr(struct code *c, int op, int dst, int src, int w); /* dst op= src */
void x86_cmp_rr(struct code *c, int a, int b, int w);         /* cmp a, b */
void x86_div_rr(struct code *c, int src, int sign, int w);    /* [rdx:rax]/src */
/* argument registers by index, for aggregates arriving in pieces */
int  x86_argreg(int index);
void x86_not_eax(struct code *c, int w);
void x86_bswap(struct code *c, int size);
void x86_mfence(struct code *c);
void x86_ud2(struct code *c);
void x86_xchg_rax_mem_rcx(struct code *c, int size);
void x86_lock_xadd_rcx(struct code *c, int size);
void x86_lock_cmpxchg_rcx(struct code *c, int size);

/* cmp eax/rax with a slot, then set al by condition and zero-extend.
 * cc is the setcc opcode byte (0x92..0x9f), chosen by codegen. */
void x86_cmp_eax_mem(struct code *c, int disp, int w);
void x86_setcc_eax(struct code *c, int cc);

/* Branches: test eax/rax; jz/jmp with a zero rel32 placeholder — both
 * return the patch offset, resolved per-function by codegen. */
void x86_test_eax(struct code *c, int w);
int x86_jz_rel32(struct code *c);
int x86_jnz_rel32(struct code *c);
int x86_jmp_rel32(struct code *c);
int x86_jcc_rel32(struct code *c, int setcc); /* setcc cond byte (0x9x) -> Jcc rel32 */

/* call rel32 with a zero placeholder; returns the rel32 field offset. */
int x86_call_rel32(struct code *c);

/* Indirect calls go through r11 — caller-saved, never an argument
 * register, and it leaves al free for the varargs convention. */
void x86_mov_r11_slot(struct code *c, int disp);
void x86_call_r11(struct code *c);

#endif
