/* long: real 64-bit arithmetic (values a 32-bit path would corrupt),
 * int->long promotion, long->int truncation, and the unsigned
 * behaviors that differ from signed: division, >>, comparison. */
// expect-exit: 42
static int checks(void) {
    int ok = 0;
    long big = 1L << 40;                 /* needs 64-bit shl */
    if (big / (1L << 20) == (1L << 20))
        ok++;
    long prod = 3000000000L + 3000000000L;
    if (prod == 6000000000L)             /* would wrap in 32-bit */
        ok++;
    int i = -1;
    long l = i;                          /* movsxd: sign extends */
    if (l == -1L)
        ok++;
    long trunc = 4294967296L + 7;        /* 2^32 + 7 */
    int t = (int)trunc;                  /* low 32 bits = 7 */
    if (t == 7)
        ok++;
    unsigned int ui = 0;
    ui = ui - 1;                         /* wraps to 4294967295 */
    if (ui > 4294967294u)                /* unsigned compare */
        ok++;
    if (ui / 2u == 2147483647u)          /* div, not idiv */
        ok++;
    int neg = -8;
    unsigned int uneg = (unsigned int)neg;
    if (uneg >> 1 == 2147483644u)        /* shr, not sar */
        ok++;
    if (neg >> 1 == -4)                  /* sar for signed */
        ok++;
    if (sizeof(long) == 8 && sizeof(int) == 4)
        ok++;
    return ok; /* 9 when all hold */
}
int main(void) {
    if (checks() == 9)
        return 42;
    return checks();
}
