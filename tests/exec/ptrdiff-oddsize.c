/* ptr - ptr for a non-power-of-two element size (24 bytes here) must
 * divide the byte gap by the size, not shift — EmbCC's own code does
 * `m - ty->members` on 24-byte struct member, so self-hosting needs it. */
// expect-exit: 42
struct three { long a; int b; int c; };   /* 8+4+4 = 16... force 24 */
struct s24 { const char *p; int x; int y; };  /* 8+4+4 = 16 */
struct s24b { const char *p; int x; long y; }; /* 8+4(pad)+8 = 24 */
static struct s24b arr[8];
int main(void) {
    struct s24b *base = arr;
    struct s24b *e = &arr[5];
    long d = e - base;                 /* must be exactly 5 */
    if (d != 5) return (int)d;
    struct s24b *e2 = &arr[7];
    if ((e2 - e) != 2) return 90;
    if ((base - e2) != -7) return 91;  /* negative difference, exact */
    return (int)(d * 8 + 2);           /* 42 */
}
