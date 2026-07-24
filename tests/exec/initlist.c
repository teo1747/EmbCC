/* Aggregate initializers and block scoping — the last two constructs
 * the sval SDK needs (wire.c's `struct rd r = { payload, len, 0 }` and
 * sval.c's `const char *units[] = { "B", "KB", ... }`).
 *
 * Pins: unlisted elements are ZERO (C's rule, not garbage), runtime
 * values are legal initializers for a local aggregate, an omitted
 * array size comes from the element count, nesting works, and an inner
 * block may shadow a name its sibling also used. */
// expect-exit: 42
int printf(const char *fmt, ...);
struct Rd { const unsigned char *p; long len; long pos; };
struct Pair { int a; int b; };
struct Outer { struct Pair p; int tag; };

static long consume(const unsigned char *payload, long len) {
    struct Rd r = { payload, len, 0 };   /* runtime values */
    return r.len - r.pos + (r.p[0] - 'A');
}
int main(void) {
    /* an omitted size comes from the element count */
    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    if (sizeof units / sizeof units[0] != 5) return 1;
    if (units[0][0] != 'B' || units[4][0] != 'T') return 2;

    /* unlisted elements are zero, not garbage */
    int part[5] = { 7, 8 };
    if (part[0] != 7 || part[1] != 8) return 3;
    if (part[2] || part[3] || part[4]) return 4;

    /* nested aggregates */
    struct Outer o = { { 3, 4 }, 9 };
    if (o.p.a != 3 || o.p.b != 4 || o.tag != 9) return 5;
    struct Outer z = { { 1 } };            /* the rest zero */
    if (z.p.a != 1 || z.p.b != 0 || z.tag != 0) return 6;

    /* runtime values in a local aggregate */
    const unsigned char bytes[3] = { 'A', 'B', 'C' };
    if (consume(bytes, 40) != 40) return 7;

    /* sibling blocks may reuse a name, and an inner block may shadow */
    int total = 0;
    { int i = 10; total += i; }
    { int i = 20; total += i; }
    for (int i = 0; i < 3; i++) total += i;   /* +3 */
    for (int i = 0; i < 2; i++) total += i;   /* +1 */
    if (total != 34) return 8;
    {
        int total = 100;                      /* shadows the outer one */
        if (total != 100) return 9;
    }
    if (total != 34) return 10;

    printf("%s %s %d\n", units[1], units[2], part[0]);
    return total + part[0] + 1;               /* 34 + 7 + 1 = 42 */
}
