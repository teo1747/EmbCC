/* va_arg reading INTEGER-class varargs (ints, longs, pointers) from the
 * SysV register save area and the overflow area. syscalls.c's open() uses
 * va_arg(ap, int), so the rim self-host needs it. Reads enough varargs to
 * spill past the 6 GP registers into the overflow area. */
// expect-exit: 42
#include <stdarg.h>
static long suml(int count, ...) {
    va_list ap;
    va_start(ap, count);
    long total = 0;
    for (int i = 0; i < count; i++)
        total += va_arg(ap, int);
    va_end(ap);
    return total;
}
static int first_ptr_len(int n, ...) {
    va_list ap; va_start(ap, n);
    const char *s = va_arg(ap, const char *);   /* pointer vararg */
    va_end(ap);
    int k = 0; while (s[k]) k++; return k;
}
int main(void) {
    /* 10 int args -> the last few land in the overflow area (only 5 GP regs
     * left after `count`), exercising both paths */
    long s = suml(10, 1,2,3,4,5,6,7,8,9,10);   /* 55 */
    if (s != 55) return 1;
    if (suml(3, 100, -50, -8) != 42) return 2;
    if (first_ptr_len(1, "abcd") != 4) return 3;
    return (int)(suml(2, 20, 22));             /* 42 */
}
