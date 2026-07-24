/* Arrays of function pointers — `T (*arr[])(args)`. crt0's __init_array
 * brackets are `extern void (*__init_array_start[])(void)` (empty []), and
 * code walks such arrays calling through them. */
// expect-exit: 42
extern int (*ext_table[])(void);   /* crt0 shape: empty [] extern, unused */
static int a(void) { return 40; }
static int b(void) { return 2; }
int main(void) {
    int (*fns[2])(void);           /* a local array of function pointers */
    fns[0] = a;
    fns[1] = b;
    int total = 0;
    for (int i = 0; i < 2; i++)
        total += fns[i]();          /* call through the array */
    return total;                   /* 42 */
}
