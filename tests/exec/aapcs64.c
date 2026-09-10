/* The AAPCS64 rules the aarch64 backend recomputes rather than inheriting
 * from irgen's SysV classification (D-011), each in a form where getting it
 * wrong changes the answer:
 *
 *   - a composite of 16 bytes or fewer returned in x0/x1, and one larger
 *     than 16 returned through the hidden x8 pointer;
 *   - a call with NINE integer arguments, so the ninth lands on the stack
 *     (SysV would have spilled from the seventh — six registers, not eight);
 *   - a composite argument small enough for registers, and one too large,
 *     passed by value on the stack;
 *   - doubles on their own register file, interleaved with integers so the
 *     two counters have to advance independently.
 *
 * Plain portable C, so x86-64 referees the expected values.
 */
// expect-exit: 42
int printf(char *fmt, ...);

struct Small { int x; long y; };            /* 16 bytes: x0, x1 */
struct Big   { long a, b, c, d; };          /* 32 bytes: through x8 */

static struct Small mk_small(int a, long b) {
    struct Small s; s.x = a; s.y = b; return s;
}
static struct Big mk_big(long n) {
    struct Big b; b.a = n; b.b = n * 2; b.c = n * 3; b.d = n * 4; return b;
}
static long take_small(struct Small s, long k) { return s.x + s.y + k; }
static long take_big(struct Big b, long k)     { return b.a + b.d + k; }

static long nine(int a, int b, int c, int d, int e,
                 int f, int g, int h, int i) {
    return a + b + c + d + e + f + g + h + i;
}
/* Integers and doubles advance NGRN and NSRN independently. */
static double mixed(int a, double x, int b, double y, int c, double z) {
    return (a + b + c) + (x + y + z);
}

int main(void) {
    struct Small s = mk_small(7, 35);
    if (s.x != 7 || s.y != 35) return 1;
    if (take_small(s, 8) != 50) return 2;

    struct Big g = mk_big(10);
    if (g.a != 10 || g.b != 20 || g.c != 30 || g.d != 40) return 3;
    if (take_big(g, 5) != 55) return 4;

    if (nine(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45) return 5;

    double m = mixed(1, 0.5, 2, 0.25, 3, 0.25);
    if (m != 7.0) return 6;

    printf("aapcs64: %d %ld %ld %ld %.2f\n", s.x, s.y, g.d,
           nine(1, 2, 3, 4, 5, 6, 7, 8, 9), m);
    return 42;
}
