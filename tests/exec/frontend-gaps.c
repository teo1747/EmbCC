/* The front-end features real vendored C (fdlibm) needs, which EmbCC gained to
 * fold math into the self-host set: goto+labels, the null statement, multiple
 * declarators in one declaration (function prototypes included), block-scope
 * struct/union definitions, the __builtin float specials, and a hardware sqrt
 * via an SSE inline-asm "x"/"=x" operand. Each check returns a distinct code;
 * success returns 42. */
// expect-exit: 42
#include <stdint.h>

/* multiple declarators, function-typed, sharing one base */
static double dbl(double), trp(double);
static double dbl(double a) { return a * 2.0; }
static double trp(double a) { return a * 3.0; }

/* block-scope union: the fdlibm bit-access idiom */
static double my_fabs(double x)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    v.u &= 0x7fffffffffffffffUL;
    return v.d;
}

/* hardware sqrt via an SSE inline-asm operand */
static double hw_sqrt(double x)
{
    double r;
    __asm__("sqrtsd %1,%0" : "=x"(r) : "x"(x));
    return r;
}

/* goto: a retry loop and a forward jump */
static int digits(long n)
{
    int c = 0;
    if (n < 0) n = -n;
    if (n == 0) return 1;
count:
    c++;
    n /= 10;
    if (n) goto count;
    return c;
}

int main(void)
{
    /* multi-declarator prototypes resolve to the right functions */
    if (dbl(2.5) != 5.0 || trp(2.5) != 7.5) return 1;

    /* block-scope union bit-access */
    if (my_fabs(-3.5) != 3.5 || my_fabs(2.25) != 2.25) return 2;

    /* the null statement as a loop body */
    int a[5] = { 1, 2, 3, 0, 9 };
    int k;
    for (k = 0; a[k]; k++)
        ;                       /* empty body */
    if (k != 3) return 3;

    /* goto */
    if (digits(0) != 1 || digits(12345) != 5 || digits(-999) != 3) return 4;

    /* __builtin float specials produce the right bit patterns */
    double inf = __builtin_huge_val();
    double nan = __builtin_nan("");
    if (!(inf > 1e308)) return 5;
    if (nan == nan) return 6;           /* NaN != itself */
    if (!(-inf < -1e308)) return 7;     /* -inf is below every finite value */

    /* stdint exact widths */
    if (sizeof(int8_t) != 1 || sizeof(int16_t) != 2 ||
        sizeof(int32_t) != 4 || sizeof(int64_t) != 8) return 8;
    uint64_t big = UINT64_MAX;
    if (big + 1 != 0) return 9;

    /* hardware sqrt is exact for perfect squares */
    if (hw_sqrt(1024.0) != 32.0 || hw_sqrt(0.25) != 0.5) return 10;

    /* unary plus is identity */
    double v = 6.25;
    if (+v != 6.25) return 11;

    return 42;
}
