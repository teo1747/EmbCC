/* Local stack-slot coalescing by lexical scope (the fix for kernel mega-function
 * frames 3-5x gcc's). Locals in DISJOINT scopes share a slot; nested/overlapping
 * ones must NOT. These are address-taken arrays (their address is passed to
 * helpers), so a coalescing bug that merged overlapping scopes would corrupt a
 * live array. Refereed against gcc by the golden harness. */
// expect-exit: 42

int printf(const char *, ...);

static long sum(const int *a, int n) { long s = 0; for (int i=0;i<n;i++) s += a[i]; return s; }
static void fill(int *a, int n, int base) { for (int i=0;i<n;i++) a[i] = base + i; }

/* Two big arrays in disjoint sibling scopes: gcc reuses one slot; correctness
 * must not depend on that. */
static long two_scopes(void) {
    long r = 0;
    { int a[64]; fill(a, 64, 100); r += sum(a, 64); }     /* scope 1 */
    { int b[64]; fill(b, 64, 200); r += sum(b, 64); }     /* scope 2 (disjoint) */
    { int c[64]; fill(c, 64, 300); r += sum(c, 64); }     /* scope 3 (disjoint) */
    return r;
}

/* Nested scopes: the outer array is LIVE while the inner one is used, so they
 * must NOT share a slot — if they did, using inner would clobber outer. */
static long nested(void) {
    int outer[32];
    fill(outer, 32, 1);
    long r = 0;
    {
        int inner[32];
        fill(inner, 32, 1000);
        r += sum(inner, 32);        /* inner used here */
        r += sum(outer, 32);        /* outer STILL needed after inner — no share */
    }
    r += sum(outer, 32);            /* outer used again after the inner block */
    return r;
}

/* A loop whose body has its own block-scoped array each iteration. */
static long loop_scopes(int iters) {
    long r = 0;
    for (int k = 0; k < iters; k++) {
        int buf[16];
        fill(buf, 16, k * 10);
        r += sum(buf, 16);
    }
    return r;
}

int main(void) {
    long t = two_scopes();   /* 100..163 + 200..263 + 300..363 */
    /* sum 100..163 = 64*100 + (0+..+63) = 6400+2016 = 8416; +200 base = 14816;
     * +300 base = 21216; total = 8416+14816+21216 = 44448 */
    if (t != 44448) return 1;

    long nz = nested();
    /* inner 1000..1031 = 32*1000 + 496 = 32496; outer 1..32 base(1)+i => 1..32?
     * fill(outer,32,1): 1,2,...,32 -> sum = 528. r = 32496 + 528 + 528 = 33552 */
    if (nz != 33552) return 2;

    long ls = loop_scopes(5);
    /* k=0: buf 0..15 sum=120; k=1: base10 -> 10..25 sum=120+160=280; ...
     * each iter sum = (k*10)*16 + 120. total over k=0..4 = 5*120 + 160*(0+1+2+3+4)
     * = 600 + 160*10 = 600 + 1600 = 2200 */
    if (ls != 2200) return 3;

    return 42;
}
