/* Function pointers: designator decay, &f == f, calls through fp vars,
 * (*fp)() syntax, fp parameters (qsort-style callbacks), arrays of
 * function pointers (newlib's atexit shape), fp struct members, and
 * variadic printf through a pointer (al=0 through r11). */
// expect-exit: 42
int printf(const char *fmt, ...);
typedef int (*binop_fn)(int, int);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

static int apply(binop_fn op, int a, int b) { return op(a, b); }

struct calc { const char *name; binop_fn fn; };

int main(void) {
    binop_fn f = add;
    if (f(20, 22) != 42) return 1;
    f = &sub;                        /* &f is f */
    if ((*f)(50, 8) != 42) return 2; /* (*fp)() is fp() */

    int (*table[3])(int, int);
    table[0] = add;
    table[1] = sub;
    table[2] = mul;
    if (table[2](6, 7) != 42) return 3;
    if (apply(table[0], 40, 2) != 42) return 4;

    struct calc c;
    c.name = "mul";
    c.fn = mul;
    if (c.fn(21, 2) != 42) return 5;

    int (*pf)(const char *, ...) = printf;
    pf("%s %d\n", c.name, table[1](50, 8));
    return apply(mul, 6, 7);
}
