/* Structs passed and returned BY VALUE — SysV classification.
 *
 * Every shape the ABI treats differently is here, because getting one
 * wrong does not crash: it silently disagrees with gcc-built code.
 *   - 8 bytes, all integer      -> ONE integer register
 *   - 16 bytes, all integer     -> rax + rdx  (two integer regs)
 *   - 16 bytes, all double      -> xmm0 + xmm1 (two SSE regs)
 *   - 16 bytes, mixed           -> one of each, in field order
 *   - 32 bytes                  -> MEMORY: stack in, hidden pointer out
 * agrees-with-gcc.sh compiles this file with BOTH compilers and
 * compares, which is the only way to prove the classification matches. */
// expect-exit: 42
int printf(const char *fmt, ...);

struct Small  { int a; int b; };                 /* 8  : INTEGER */
struct Two    { long a; long b; };               /* 16 : INTEGER, INTEGER */
struct Dbl    { double x; double y; };           /* 16 : SSE, SSE */
struct Mixed  { long n; double d; };             /* 16 : INTEGER, SSE */
struct Big    { long a; long b; long c; long d; };/* 32 : MEMORY */

static struct Small mk_small(int a, int b) {
    struct Small s; s.a = a; s.b = b; return s;
}
static int take_small(struct Small s) { return s.a + s.b; }

static struct Two mk_two(long a, long b) {
    struct Two t; t.a = a; t.b = b; return t;
}
static long take_two(struct Two t) { return t.a * t.b; }

static struct Dbl mk_dbl(double x, double y) {
    struct Dbl d; d.x = x; d.y = y; return d;
}
static double take_dbl(struct Dbl d) { return d.x + d.y; }

static struct Mixed mk_mixed(long n, double d) {
    struct Mixed m; m.n = n; m.d = d; return m;
}
static double take_mixed(struct Mixed m) { return m.n * m.d; }

static struct Big mk_big(long base) {
    struct Big b; b.a = base; b.b = base+1; b.c = base+2; b.d = base+3;
    return b;
}
static long take_big(struct Big b) { return b.a + b.b + b.c + b.d; }

/* a struct argument AFTER other arguments, so the register walk is
 * genuinely offset rather than starting at zero */
static long offset_case(int p, double q, struct Mixed m, int r) {
    return p + (long)q + m.n + (long)m.d + r;
}

int main(void) {
    if (take_small(mk_small(20, 22)) != 42) return 1;
    if (take_two(mk_two(6, 7)) != 42) return 2;
    if (take_dbl(mk_dbl(20.5, 21.5)) != 42.0) return 3;
    if (take_mixed(mk_mixed(21, 2.0)) != 42.0) return 4;
    if (take_big(mk_big(9)) != 42) return 5;   /* 9+10+11+12 */

    /* struct assignment copies the bytes */
    struct Big x = mk_big(9);
    struct Big y;
    y = x;
    x.a = 999;                    /* must NOT disturb y */
    if (take_big(y) != 42) return 6;

    /* nested and by-value round trips keep their values */
    struct Mixed m = mk_mixed(4, 5.0);
    if (offset_case(1, 2.0, m, 30) != 42) return 7;  /* 1+2+4+5+30 */

    printf("%d %ld %.1f %ld\n", take_small(mk_small(1, 2)),
           take_two(mk_two(3, 4)), take_dbl(mk_dbl(1.5, 2.5)),
           take_big(mk_big(0)));
    return take_small(mk_small(20, 22));
}
