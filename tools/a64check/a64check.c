/* Emits one instruction per line through src/asm/emit_arm64.c and writes the
 * raw words to stdout, alongside the mnemonic EACH ONE IS SUPPOSED TO BE on
 * stderr. tests/golden/arm64-encoding.sh disassembles the bytes with
 * aarch64-elf-objdump and diffs the two.
 *
 * This is the only defence against a wrong bit in an encoding: a backend
 * that assembles its own instructions has no assembler to catch it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/asm/emit_arm64.h"

static struct code C;
static FILE *want;

/* Emit `expect` as the expected disassembly of whatever the last call added. */
static int mark;
static void expect(const char *text)
{
    /* One expected disassembly line per word emitted, '|'-separated when a
     * single call produces several. */
    const char *p = text;
    for (int o = mark; o < C.len; o += 4) {
        const char *bar = strchr(p, '|');
        int n = bar ? (int)(bar - p) : (int)strlen(p);
        fprintf(want, "%.*s\n", n, p);
        if (bar) p = bar + 1; else p += n;
    }
    mark = C.len;
}

int main(void)
{
    want = stderr;

    /* Branches come FIRST so their patched targets are small fixed
     * addresses that do not move when instructions are appended below.
     * The displacement, not just the opcode, is what is being checked. */
    {
        int b  = a64_b(&C);   a64_patch_b26(&C, b, b + 8);
        expect("b\t8");
        int bc = a64_bcond(&C, A64_NE); a64_patch_b19(&C, bc, bc + 8);
        expect("b.ne\tc");
        int cb = a64_cbz(&C, 9, 0, 8);  a64_patch_b19(&C, cb, cb + 8);
        expect("cbz\tx9, 10");
        int cn = a64_cbz(&C, 9, 1, 4);  a64_patch_b19(&C, cn, cn + 8);
        expect("cbnz\tw9, 14");
        int bl = a64_bl(&C);  a64_patch_b26(&C, bl, bl - 8);
        expect("bl\t8");
        /* adr's displacement is in BYTES, not words like a branch's — the
         * one place the two differ, so it is checked in the same block. */
        int ad = a64_adr(&C, 9);  a64_patch_adr(&C, ad, ad + 12);
        expect("adr\tx9, 20");
    }

    a64_mov_reg(&C, 9, 10, 8);          expect("mov\tx9, x10");
    a64_mov_reg(&C, 9, 10, 4);          expect("mov\tw9, w10");
    a64_mov_imm(&C, 9, 0, 8);           expect("mov\tx9, #0x0");
    a64_mov_imm(&C, 9, 42, 8);          expect("mov\tx9, #0x2a");
    a64_mov_imm(&C, 0, 0xffff, 8);      expect("mov\tx0, #0xffff");
    a64_mov_imm(&C, 9, -1, 8);          expect("mov\tx9, #0xffffffffffffffff");
    a64_mov_imm(&C, 9, -42, 8);         expect("mov\tx9, #0xffffffffffffffd6");

    a64_add_imm(&C, 9, 10, 1, 8);       expect("add\tx9, x10, #0x1");
    a64_add_imm(&C, 9, 10, 4095, 8);    expect("add\tx9, x10, #0xfff");
    a64_sub_imm(&C, 31, 31, 32, 8);     expect("sub\tsp, sp, #0x20");
    a64_add_imm(&C, 9, 10, 1, 4);       expect("add\tw9, w10, #0x1");

    a64_alu_reg(&C, '+', 9, 10, 11, 8); expect("add\tx9, x10, x11");
    a64_alu_reg(&C, '-', 9, 10, 11, 8); expect("sub\tx9, x10, x11");
    a64_alu_reg(&C, '&', 9, 10, 11, 8); expect("and\tx9, x10, x11");
    a64_alu_reg(&C, '|', 9, 10, 11, 8); expect("orr\tx9, x10, x11");
    a64_alu_reg(&C, '^', 9, 10, 11, 8); expect("eor\tx9, x10, x11");
    a64_alu_reg(&C, '+', 9, 10, 11, 4); expect("add\tw9, w10, w11");

    a64_mul(&C, 9, 10, 11, 8);          expect("mul\tx9, x10, x11");
    a64_mul(&C, 9, 10, 11, 4);          expect("mul\tw9, w10, w11");
    a64_div(&C, 9, 10, 11, 1, 8);       expect("sdiv\tx9, x10, x11");
    a64_div(&C, 9, 10, 11, 0, 8);       expect("udiv\tx9, x10, x11");
    a64_msub(&C, 9, 10, 11, 12, 8);     expect("msub\tx9, x10, x11, x12");

    a64_shift_reg(&C, '<', 9, 10, 11, 8); expect("lsl\tx9, x10, x11");
    a64_shift_reg(&C, '>', 9, 10, 11, 8); expect("asr\tx9, x10, x11");
    a64_shift_reg(&C, 'u', 9, 10, 11, 8); expect("lsr\tx9, x10, x11");
    a64_shift_reg(&C, '<', 9, 10, 11, 4); expect("lsl\tw9, w10, w11");

    a64_neg(&C, 9, 10, 8);              expect("neg\tx9, x10");
    a64_mvn(&C, 9, 10, 8);              expect("mvn\tx9, x10");
    a64_rev(&C, 9, 10, 8);              expect("rev\tx9, x10");
    a64_rev(&C, 9, 10, 4);              expect("rev\tw9, w10");
    a64_rev(&C, 9, 10, 2);              expect("rev16\tw9, w10");

    a64_cmp_reg(&C, 9, 10, 8);          expect("cmp\tx9, x10");
    a64_cmp_reg(&C, 9, 10, 4);          expect("cmp\tw9, w10");
    a64_cset(&C, 9, A64_EQ);            expect("cset\tx9, eq");
    a64_cset(&C, 9, A64_LT);            expect("cset\tx9, lt");
    a64_cset(&C, 9, A64_HI);            expect("cset\tx9, hi");

    a64_extend(&C, 9, 10, 1, 1, 8);     expect("sxtb\tx9, w10");
    a64_extend(&C, 9, 10, 2, 1, 8);     expect("sxth\tx9, w10");
    a64_extend(&C, 9, 10, 4, 1, 8);     expect("sxtw\tx9, w10");
    a64_extend(&C, 9, 10, 1, 0, 8);     expect("uxtb\tw9, w10");
    a64_extend(&C, 9, 10, 2, 0, 8);     expect("uxth\tw9, w10");
    a64_extend(&C, 9, 9, 4, 0, 8);      expect("mov\tw9, w9");
    a64_extend(&C, 9, 10, 1, 1, 4);     expect("sxtb\tw9, w10");

    a64_ldr(&C, 9, 31, 0, 8, 0, 8);     expect("ldr\tx9, [sp]");
    a64_ldr(&C, 9, 31, 16, 8, 0, 8);    expect("ldr\tx9, [sp, #16]");
    a64_ldr(&C, 9, 10, 8, 4, 0, 4);     expect("ldr\tw9, [x10, #8]");
    a64_ldr(&C, 9, 10, 4, 4, 1, 8);     expect("ldrsw\tx9, [x10, #4]");
    /* A signed 4-byte load into a W register has no encoding of its own;
     * it must come out as a plain 32-bit LDR, not an unallocated word. */
    a64_ldr(&C, 9, 10, 4, 4, 1, 4);     expect("ldr\tw9, [x10, #4]");
    a64_ldr(&C, 9, 10, 2, 2, 1, 4);     expect("ldrsh\tw9, [x10, #2]");
    a64_ldr(&C, 9, 10, 2, 2, 0, 4);     expect("ldrh\tw9, [x10, #2]");
    a64_ldr(&C, 9, 10, 2, 2, 1, 8);     expect("ldrsh\tx9, [x10, #2]");
    a64_ldr(&C, 9, 10, 1, 1, 0, 4);     expect("ldrb\tw9, [x10, #1]");
    a64_ldr(&C, 9, 10, 1, 1, 1, 8);     expect("ldrsb\tx9, [x10, #1]");
    a64_ldr(&C, 9, 10, 1, 1, 1, 4);     expect("ldrsb\tw9, [x10, #1]");
    a64_str(&C, 9, 31, 24, 8);          expect("str\tx9, [sp, #24]");
    a64_str(&C, 9, 10, 4, 4);           expect("str\tw9, [x10, #4]");
    a64_str(&C, 9, 10, 2, 2);           expect("strh\tw9, [x10, #2]");
    a64_str(&C, 9, 10, 1, 1);           expect("strb\tw9, [x10, #1]");

    a64_blr(&C, 12);                    expect("blr\tx12");
    a64_br(&C, 12);                     expect("br\tx12");
    a64_ret(&C);                        expect("ret");
    a64_dmb_ish(&C);                    expect("dmb\tish");

    a64_fldr(&C, 16, 31, 16, 8);        expect("ldr\td16, [sp, #16]");
    a64_fldr(&C, 16, 10, 8, 4);         expect("ldr\ts16, [x10, #8]");
    a64_fstr(&C, 17, 31, 24, 8);        expect("str\td17, [sp, #24]");
    a64_fstr(&C, 17, 10, 4, 4);         expect("str\ts17, [x10, #4]");
    a64_falu(&C, '+', 16, 16, 17, 8);   expect("fadd\td16, d16, d17");
    a64_falu(&C, '-', 16, 16, 17, 8);   expect("fsub\td16, d16, d17");
    a64_falu(&C, '*', 16, 16, 17, 8);   expect("fmul\td16, d16, d17");
    a64_falu(&C, '/', 16, 16, 17, 8);   expect("fdiv\td16, d16, d17");
    a64_falu(&C, '+', 16, 16, 17, 4);   expect("fadd\ts16, s16, s17");
    a64_fneg(&C, 16, 17, 8);            expect("fneg\td16, d17");
    a64_fneg(&C, 16, 17, 4);            expect("fneg\ts16, s17");
    a64_fmov_reg(&C, 0, 16, 8);         expect("fmov\td0, d16");
    a64_fcmp(&C, 16, 17, 8);            expect("fcmp\td16, d17");
    a64_fcmp(&C, 16, 17, 4);            expect("fcmp\ts16, s17");
    a64_fcvt(&C, 16, 17, 4, 8);         expect("fcvt\td16, s17");
    a64_fcvt(&C, 16, 17, 8, 4);         expect("fcvt\ts16, d17");
    a64_cvt_i2f(&C, 16, 9, 1, 8, 8);    expect("scvtf\td16, x9");
    a64_cvt_i2f(&C, 16, 9, 1, 4, 8);    expect("scvtf\td16, w9");
    a64_cvt_i2f(&C, 16, 9, 0, 8, 4);    expect("ucvtf\ts16, x9");
    a64_cvt_f2i(&C, 9, 16, 1, 8, 8);    expect("fcvtzs\tx9, d16");
    a64_cvt_f2i(&C, 9, 16, 1, 4, 8);    expect("fcvtzs\tw9, d16");
    a64_cvt_f2i(&C, 9, 16, 0, 8, 4);    expect("fcvtzu\tx9, s16");

    a64_adrp(&C, 9);                    expect("adrp\tx9, 0");
    a64_add_lo12(&C, 9, 9);             expect("add\tx9, x9, #0x0");

    /* The frame sequences, as whole units. */
    a64_prologue(&C, 32);
    expect("stp\tx29, x30, [sp, #-16]!|mov\tx29, sp|sub\tsp, sp, #0x20");
    a64_epilogue(&C, 32);
    expect("add\tsp, sp, #0x20|ldp\tx29, x30, [sp], #16|ret");

    fwrite(C.p, 1, (size_t)C.len, stdout);
    return 0;
}
