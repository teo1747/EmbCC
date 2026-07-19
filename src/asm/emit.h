/* x86-64 instruction encoding (ARCHITECTURE §2: integrated — no external
 * assembler exists on-OS). Exactly the encodings M1's codegen emits;
 * an instruction this file cannot encode is a missing function, which
 * fails at build time — never a silently wrong byte sequence.
 *
 * All memory operands are [rbp+disp] (the M1 frame model: every vreg
 * has a stack slot). All arithmetic is 32-bit, matching int.
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

/* prologue: push rbp; mov rbp,rsp; sub rsp,framesize.
 * framesize must keep rsp 16-byte aligned at calls: entry rsp%16 == 8,
 * push rbp realigns to 0, so framesize must be a multiple of 16. */
void x86_prologue(struct code *c, int framesize);
void x86_epilogue(struct code *c);                        /* leave; ret */

/* SysV integer argument registers, index 0..5 = edi,esi,edx,ecx,r8d,r9d */
void x86_store_arg(struct code *c, int argno, int disp);  /* mov [rbp+disp], argreg */
void x86_load_arg(struct code *c, int argno, int disp);   /* mov argreg, [rbp+disp] */

void x86_mov_eax_imm32(struct code *c, long imm);
void x86_mov_eax_mem(struct code *c, int disp);           /* mov eax, [rbp+disp] */
void x86_mov_mem_eax(struct code *c, int disp);           /* mov [rbp+disp], eax */
void x86_alu_eax_mem(struct code *c, int op, int disp);   /* '+','-','*','&','|','^' */

/* Signed division: cdq sign-extends eax into edx:eax, idiv leaves the
 * quotient in eax and the remainder in edx. */
void x86_cdq(struct code *c);
void x86_idiv_mem(struct code *c, int disp);              /* idiv dword [rbp+disp] */
void x86_mov_eax_edx(struct code *c);

/* Shifts take their count in cl; int is signed so >> is sar. */
void x86_mov_ecx_mem(struct code *c, int disp);
void x86_shl_eax_cl(struct code *c);
void x86_sar_eax_cl(struct code *c);

void x86_neg_eax(struct code *c);
void x86_not_eax(struct code *c);

/* call rel32 with a zero placeholder; returns the offset of the rel32
 * field so the caller can patch it once the target's address is known. */
int x86_call_rel32(struct code *c);

/* Comparisons: cmp eax with a slot, then set al by condition and
 * zero-extend, leaving 0/1 in eax. cc is the x86 condition nibble
 * carrier (0x94 sete .. 0x9f setg), chosen by codegen. */
void x86_cmp_eax_mem(struct code *c, int disp);
void x86_setcc_eax(struct code *c, int cc);

/* Branches: test eax,eax; jz/jmp with a zero rel32 placeholder —
 * both return the patch offset, resolved per-function by codegen. */
void x86_test_eax(struct code *c);
int x86_jz_rel32(struct code *c);
int x86_jmp_rel32(struct code *c);

#endif
