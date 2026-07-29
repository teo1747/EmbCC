/* Inline-asm operand allocation must EXCLUDE clobbered registers AND any hard
 * register the template writes explicitly (K12). Otherwise an allocatable "r"
 * operand can land in a register the asm destroys before it is used, silently
 * corrupting the value — this is the kernel's `iretq` ring-3 trampoline bug
 * (operands moved into rdi/rsi/rdx while others must survive). THE RULE: a
 * wrong register here is a silent miscompile, so it must match gcc exactly.
 *
 * Checked against gcc via the golden agrees-with-gcc harness. */
// expect-exit: 42

/* CLOBBER-LIST path: if %1 (a) were placed in rdx, `movq %2,%%rdx` (b -> rdx)
 * would overwrite it before `movq %1,%0`, so r would read b, not a. rdx is in
 * the clobber list, so a must avoid it. */
static unsigned long via_clobber(unsigned long a, unsigned long b) {
    unsigned long r;
    __asm__ volatile("movq %2,%%rdx\n movq %1,%0\n"
                     : "=r"(r) : "r"(a), "r"(b) : "rdx");
    return r;
}

/* TEMPLATE-WRITTEN path: rsi is written by the template but is NOT in the
 * clobber list. The allocator must still keep %1 (a) out of rsi, or the
 * `movq %2,%%rsi` clobbers it before `movq %1,%0`. */
static unsigned long via_written(unsigned long a, unsigned long b) {
    unsigned long r;
    __asm__ volatile("movq %2,%%rsi\n movq %1,%0\n"
                     : "=r"(r) : "r"(a), "r"(b) : /* no clobber list */ "memory");
    return r;
}

int main(void) {
    if (via_clobber(42, 999) != 42) return 1;
    if (via_written(42, 999) != 42) return 2;
    /* a few more value pairs, both paths */
    if (via_clobber(7, 8) != 7)     return 3;
    if (via_written(123, 456) != 123) return 4;
    return 42;
}
