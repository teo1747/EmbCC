/* Inline-asm memory operands (`movq disp(%base), %dst` and the store reverse)
 * and ALU ops (add/sub/and/or/xor/cmp, reg,reg and $imm,reg). These are the K1
 * follow-ups the kernel sweep flagged. THE RULE: a wrong encoding is a silent
 * miscompile, so each form is exercised for its VALUE here and byte-compared to
 * gas in tests/golden/inline-asm-kernel.sh.
 *
 * (Uses only `=r`/`r` operands — EmbCC does not yet wire the read side of a
 * `+r` read-write operand, a separate gap the kernel does not hit.) */
// expect-exit: 42

/* load + store through a base register with a displacement */
static unsigned long load8(unsigned long *p) {
    unsigned long r;
    __asm__ volatile("movq 8(%1), %0\n" : "=r"(r) : "r"(p));
    return r;
}
static void store16(unsigned long *p, unsigned long v) {
    __asm__ volatile("movq %1, 16(%0)\n" : : "r"(p), "r"(v) : "memory");
}

/* a computation mixing loads, ALU reg/reg, ALU imm8/imm32, and a store */
static unsigned long compute(unsigned long *mem, unsigned long a, unsigned long b) {
    unsigned long r;
    __asm__ volatile(
        "movq (%1), %0\n"       /* r  = mem[0]              */
        "addq %2, %0\n"         /* r += a                   */
        "movq 8(%1), %%rcx\n"   /* rcx = mem[1]             */
        "subq %%rcx, %0\n"      /* r -= mem[1]              */
        "andq %3, %0\n"         /* r &= b                   */
        "addq $5, %0\n"         /* r += 5   (imm8)          */
        "addq $0x12345, %0\n"   /* r += 0x12345 (imm32)     */
        "movq %0, 16(%1)\n"     /* mem[2] = r               */
        : "=&r"(r) : "r"(mem), "r"(a), "r"(b) : "rcx", "memory");
    return r;
}

/* reg,reg ALU: or/xor into a fresh output */
static unsigned long orxor(unsigned long a, unsigned long b) {
    unsigned long r;
    __asm__ volatile(
        "movq %1, %0\n"   /* r = a       */
        "orq  %2, %0\n"   /* r |= b      */
        "xorq %1, %0\n"   /* r ^= a  (needs =&r: %0 written before %1's last read) */
        : "=&r"(r) : "r"(a), "r"(b));
    return r;
}

int main(void) {
    unsigned long m[3] = { 100, 30, 0 };

    if (load8(m) != 30) return 1;
    store16(m, 777);
    if (m[2] != 777) return 2;

    /* (((100 + 5) - 30) & 0xff) + 5 + 0x12345 = 80 + 0x12345 = 0x12395 */
    unsigned long r = compute(m, 5, 0xff);
    if (r != 0x12395) return 3;
    if (m[2] != 0x12395) return 4;          /* store landed */

    /* (a|b)^a keeps exactly the bits of b not in a: (0xf0|0x33)^0xf0 = 0x03 */
    if (orxor(0xf0, 0x33) != 0x03) return 5;

    return 42;
}
