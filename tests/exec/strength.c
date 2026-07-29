/* Strength reduction: multiply/divide/modulo by a power of two become shift/and.
 * Signedness matters — signed division rounds toward zero, so `x/4` on a signed
 * negative is NOT a plain arithmetic shift and must stay a real divide. Refereed
 * against gcc, which settles the exact semantics. */
// expect-exit: 42

static int   mul8(int x)            { return x * 8; }       /* x << 3 */
static int   mul_l(int x)           { return 16 * x; }      /* const on the left */
static unsigned udiv(unsigned x)    { return x / 4; }       /* x >> 2 */
static unsigned umod(unsigned x)    { return x % 8; }       /* x & 7 */
static int   sdiv(int x)            { return x / 4; }       /* stays idiv */
static int   smod(int x)            { return x % 8; }       /* stays idiv */
static int   mul_np(int x)          { return x * 6; }       /* not 2^k: real mul */

int main(void) {
    if (mul8(5) != 40) return 1;
    if (mul_l(3) != 48) return 2;
    if (udiv(23) != 5) return 3;
    if (umod(23) != 7) return 4;
    if (mul_np(7) != 42) return 5;

    /* signed negatives — the shift-vs-divide distinction */
    if (sdiv(-9) != -2) return 6;      /* toward zero, not -3 */
    if (smod(-9) != -1) return 7;
    if (sdiv(9) != 2) return 8;
    if (mul8(-3) != -24) return 9;

    return 42;
}
