/* The preprocessor: include guards (double include), object and
 * function-like macros, stringize, paste via the double-expansion
 * idiom, variadic macros, conditionals with defined(), __FILE__,
 * multi-declarators and free specifier order (header idioms), and
 * const-as-ignored. gcc referees the expansion via output diff. */
// expect-exit: 42
#include "inc/defs.h"
#include "inc/defs.h"
int printf(const char *fmt, ...);
#define NAME embcc
int XGLUE(NAME, _helper)(void) { return DOUBLE(10) + 1; }
long unsigned int reversed_specifiers = 9;
int multi_a = 1, *multi_p, multi_b = 2;
int main(void) {
    Pair p;
    p.a = embcc_helper();          /* 21 */
    p.b = ANSWER - DOUBLE(p.a) / 2;
    multi_p = &multi_a;
#ifdef ARCH64
    printf("%s %d %s\n", STR(NAME), FIRST_OF(8, 9, 10) *
           (int)sizeof(void *) / 8, __FILE__);
#endif
#if defined(ANSWER) && ANSWER > 40 && !defined(NO_SUCH_MACRO)
    return p.a + p.b - *multi_p - multi_b + (int)reversed_specifiers
           - 9 + 3;               /* 21+21-1-2+9-9+3 = 42 */
#else
    return 1;
#endif
}
