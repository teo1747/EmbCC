/* Floating point, part 2: the features that complete float support for a real
 * math.c — static/global initializers with float bit patterns, hex-float
 * literals (0x1.8p3, exact), va_arg of a floating type (SSE class read from the
 * register save area AND the overflow area), and unary plus. Each check returns
 * a distinct nonzero code so a failure names itself; success returns 42.
 *
 * The hex-float constants are compared against their exact decimal twins so a
 * mis-scan (or a truncated bit pattern) is caught, not just "it compiled". */
// expect-exit: 42
#include <stdarg.h>

/* --- static / global initializers: the bytes must be the IEEE-754 image --- */
double g_pi      = 3.141592653589793;      /* a full-precision double literal   */
double g_third   = 1.0 / 3.0;              /* a folded constant expression      */
float  g_ef      = 2.5f;                   /* a float slot: 4 bytes, exact       */
double g_arr[4]  = { 1.5, 2.5, 3.5, 4.5 }; /* an aggregate of doubles            */
double g_hexpi   = 0x1.921fb54442d18p+1;   /* pi, written exactly in hex float   */
double g_frombig = 5;                      /* an int constant in a double slot   */
struct Mix { double x; int n; double y; } g_mix = { 1.25, 7, 6.5 };

static double g_sum_arr(void) { double s = 0; for (int i = 0; i < 4; i++) s += g_arr[i]; return s; }

/* --- va_arg of double: register-save-area path, overflow path, and mixed --- */
static double dsum(int n, ...) {
    va_list ap; va_start(ap, n);
    double s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return s;
}
static double mixed(int n, ...) {          /* int and double interleaved: gp/fp advance apart */
    va_list ap; va_start(ap, n);
    double acc = 0;
    for (int i = 0; i < n; i++) {
        if (va_arg(ap, int)) acc += va_arg(ap, double);
        else                 acc += (double)va_arg(ap, int);
    }
    va_end(ap);
    return acc;
}

int main(void) {
    /* static initializers hold the right values */
    if (g_pi != 3.141592653589793) return 1;
    if (g_third * 3.0 <= 0.999999 || g_third * 3.0 >= 1.000001) return 2;
    if (g_ef != 2.5f) return 3;
    if (g_sum_arr() != 12.0) return 4;
    if ((double)g_frombig != 5.0) return 5;
    if (g_mix.x != 1.25 || g_mix.n != 7 || g_mix.y != 6.5) return 6;

    /* hex floats are exact and equal their decimal twins */
    if (g_hexpi != 3.141592653589793) return 7;
    if (0x1p-4 != 0.0625) return 8;             /* 2^-4              */
    if (0x1.8p3 != 12.0) return 9;              /* 1.5 * 2^3         */
    if (0x1p+10 != 1024.0) return 10;           /* 2^10              */
    static double loc = 0x1.0p-1;               /* static local hex float = 0.5 */
    if (loc != 0.5) return 11;

    /* unary plus is identity */
    double v = 3.5;
    if (+v != 3.5 || +g_pi != g_pi) return 12;

    /* va_arg of double: in-register, overflow (9 > 8 xmm regs), and mixed */
    if (dsum(5, 1.5, 2.5, 3.5, 4.5, 5.5) != 17.5) return 13;
    if (dsum(9, 1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0) != 45.0) return 14;
    if (mixed(4, 1, 2.5, 0, 10, 1, 0.25, 0, 100) != 112.75) return 15;

    return 42;
}
