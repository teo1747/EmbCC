/* Floating point: SSE2 scalar double and float.
 *
 * Pins the things a casual implementation gets wrong: float vs double
 * precision actually differing, truncation toward zero (not rounding),
 * NaN comparing false in EVERY direction including ==, -0.0 surviving
 * negation, the usual arithmetic conversions promoting int to double,
 * and — the one that silently corrupts everything — printf's varargs
 * needing al = the number of xmm registers used. */
// expect-exit: 42
int printf(const char *fmt, ...);

static double half(double x) { return x / 2.0; }
static float  fhalf(float x) { return x / 2.0f; }
static int    trunc_to_int(double x) { return (int)x; }
static double from_int(int n) { return n; }

int main(void) {
    if (half(84.0) != 42.0) return 1;
    if (from_int(21) + from_int(21) != 42.0) return 2;

    /* truncation toward zero, both signs — not rounding */
    if (trunc_to_int(42.9) != 42) return 3;
    if (trunc_to_int(-42.9) != -42) return 4;

    /* int promotes to double in mixed arithmetic */
    int i = 3;
    if (i * 1.5 != 4.5) return 5;

    /* float really is narrower than double */
    float f = 0.1f;
    double d = 0.1;
    if ((double)f == d) return 6;
    if (fhalf(8.0f) != 4.0f) return 7;

    /* every ordered comparison */
    double a = 1.0, b = 2.0;
    if (!(a < b) || !(a <= b) || (a > b) || (a >= b)) return 8;
    if (a == b || !(a != b)) return 9;

    /* NaN is equal to nothing, itself included, and no ordering holds */
    double zero = 0.0;
    double nan = zero / zero;
    if (nan == nan) return 10;
    if (!(nan != nan)) return 11;
    if (nan < 1.0 || nan > 1.0 || nan <= 1.0 || nan >= 1.0) return 12;

    /* negation flips the sign bit: -0.0 is not 0.0's bit pattern */
    double nz = -zero;
    if (nz != 0.0) return 13;          /* but they COMPARE equal */
    if (1.0 / nz > 0.0) return 14;     /* -inf proves the sign survived */

    /* unsigned int -> double is exact (it goes through 64 bits) */
    unsigned int big = 4000000000u;
    if ((double)big != 4000000000.0) return 15;

    /* varargs: al must say how many xmm registers carry arguments */
    printf("%d %.1f %s %.2f\n", 42, 1.5, "mixed", 0.25);
    return trunc_to_int(half(84.0));
}
