/* Short-circuit && and || — observable through side effects: bump()
 * must NOT run when the left side already decides. If short-circuit is
 * broken, counter comes back wrong and the exit code moves. */
// expect-exit: 42
static int bump(int x) { return x + 1; } /* used as a side-effect marker */
static int guard(int c, int x) {
    int hits = 0;
    if (c && (hits = bump(hits)) > 0) {
        x = x + hits; /* only when c is true: adds 1 */
    }
    if (c == 0 || (hits = hits + 100) > 0) {
        x = x + hits; /* c true: adds 101; c false: adds 0-ish */
    }
    return x;
}
int main(void) {
    /* guard(1, 0): hits=1 -> x=1; then hits=101 -> x=102
     * guard(0, 0): first if skipped (hits stays 0); second: left true,
     *              right not evaluated -> x = 0 + 0 = 0 */
    int a = guard(1, 0);   /* 102 */
    int b = guard(0, 0);   /* 0 */
    int nots = !0 * 2 + !5;            /* 2 */
    return a - 62 + b + nots + 0 * 100; /* 102-62+0+2 = 42 */
}
