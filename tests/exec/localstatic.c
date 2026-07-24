/* The constructs real EmbLinkOS source uses that the subset lacked:
 * a static local with a string initializer (tally.c's error message),
 * compound assignment through a member and a pointer (wire.c's
 * b->len += n), and a declaration in for-init (value.c's loops). */
// expect-exit: 42
int printf(const char *fmt, ...);
struct Buf { char *data; long len; };

static long counter(void) {
    static long calls = 0;   /* keeps its value between calls */
    calls += 1;
    return calls;
}
static long sum_to(int n) {
    long s = 0;
    for (int i = 1; i <= n; i++)   /* declaration in for-init */
        s += i;
    return s;
}
int main(void) {
    static const char msg[] = "static-local ok";
    if (sizeof msg != 16) return 1;   /* size inferred from the literal */
    if (msg[0] != 's' || msg[14] != 'k' || msg[15] != 0) return 2;

    if (counter() != 1) return 3;
    if (counter() != 2) return 4;
    if (counter() != 3) return 5;     /* proves it is NOT re-initialized */

    char storage[8];
    struct Buf b;
    b.data = storage;
    b.len = 0;
    b.len += 5;                       /* through a member */
    b.len *= 4;
    b.len -= 8;                       /* 12 */
    if (b.len != 12) return 6;

    int v = 100;
    int *p = &v;
    *p += 8;                          /* through a pointer */
    *p /= 2;                          /* 54 */
    if (v != 54) return 7;

    char *cp = storage;
    cp += 3;                          /* pointer compound arithmetic */
    if (cp - storage != 3) return 8;

    if (sum_to(8) != 36) return 9;
    printf("%s %ld\n", msg, sum_to(4));
    return (int)(b.len + v - 12 - 12); /* 12 + 54 - 24 = 42 */
}
