/* Unsigned 64-bit <-> floating conversion. SSE2's cvtsi2sd/cvttsd2si are
 * signed-only, so a u64 with its top bit set needs the split-and-add (up) and
 * the 2^63-bias (down) fixups. Pins the boundary at 2^63 where signed conversion
 * would flip, and 2^64 rounding. Each check returns its own code; success 42.
 * (emlibc's stdio %f formatter needs both directions.) */
// expect-exit: 42
static double u2d(unsigned long x) { return (double)x; }
static float  u2f(unsigned long x) { return (float)x; }
static unsigned long d2u(double x) { return (unsigned long)x; }

int main(void)
{
    /* u64 -> double: exact where representable, correctly rounded past 2^53 */
    if (u2d(0UL) != 0.0) return 1;
    if (u2d(1000UL) != 1000.0) return 2;
    if (u2d(9223372036854775808UL) != 9223372036854775808.0) return 3;   /* 2^63, top bit set */
    if (u2d(18446744073709551615UL) != 18446744073709551616.0) return 4; /* 2^64-1 rounds to 2^64 */
    if (u2d(4294967296UL) != 4294967296.0) return 5;                     /* 2^32 exact */

    /* double -> u64: truncates toward zero, correct above 2^63 */
    if (d2u(0.0) != 0UL) return 6;
    if (d2u(1000.9) != 1000UL) return 7;
    if (d2u(9223372036854775808.0) != 9223372036854775808UL) return 8;   /* exactly 2^63 */
    if (d2u(1.0e19) != 10000000000000000000UL) return 9;                 /* > 2^63 */
    if (d2u(1.5e19) != 15000000000000000000UL) return 10;

    /* round-trips below 2^53 are exact both ways */
    unsigned long r = 123456789012345UL;
    if (d2u(u2d(r)) != r) return 11;

    /* u64 -> float: 2^63 is exactly representable as a float */
    if ((double)u2f(9223372036854775808UL) != 9223372036854775808.0) return 12;

    return 42;
}
