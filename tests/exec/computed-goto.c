/* GNU computed goto: &&label yields a void* to a code location, and `goto *p`
 * jumps to it. The optimizer bails on functions that use it (an indirect jump
 * makes the CFG imprecise), and codegen keeps them in the memory model (no
 * regalloc, no slot coalescing) — so `keep` below, live ACROSS the indirect
 * jumps, must survive. Refereed against gcc; run at -O0/-O1/-O2. */
// expect-exit: 42

/* A tiny threaded-code dispatcher: each "op" jumps directly to the next. */
static int run(int start)
{
    long keep = 1000;                 /* live across every indirect jump */
    void *ops[3] = { &&ADD1, &&ADD10, &&STOP };
    long acc = 0;
    int pc = start;
    goto *ops[pc];
ADD1:  acc += 1;   pc = 1; goto *ops[pc];
ADD10: acc += 10;  pc = 2; goto *ops[pc];
STOP:  return (int)(acc + keep);
}

/* Label address in a plain local, and a self-loop through a computed goto. */
static int loopy(void)
{
    long n = 0;
    void *again = &&L;
L:  n += 7;
    if (n < 35) goto *again;
    return (int)n;                    /* 35 */
}

int main(void)
{
    if (run(0) != 1011) return 1;     /* ADD1(+1) -> ADD10(+10) -> STOP: 11+1000 */
    if (run(1) != 1010) return 2;     /* start at ADD10: +10 then STOP -> 1010 */
    if (run(2) != 1000) return 3;     /* start at STOP: 0 + 1000 */
    if (loopy() != 35)  return 4;
    return 42;
}
