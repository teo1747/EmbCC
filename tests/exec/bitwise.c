/* Bitwise ops, shifts (>> is arithmetic: int is signed), and C's
 * precedence: & binds tighter than ^ tighter than |, shifts tighter
 * than comparisons. gcc agreement is the real referee here. */
// expect-exit: 42
static int checks(void) {
    int ok = 0;
    if ((12 & 10) == 8)
        ok++;
    if ((12 | 10) == 14)
        ok++;
    if ((12 ^ 10) == 6)
        ok++;
    if ((1 << 5) + (128 >> 2) == 64)
        ok++;
    if ((-8 >> 1) == -4) /* sar, not shr */
        ok++;
    if ((3 | 1 & 2) == 3) /* & first: 3 | 0 */
        ok++;
    if ((1 ^ 2 | 4) == 7) /* ^ then | */
        ok++;
    return ok; /* 7 when all hold */
}
int main(void) {
    if (checks() == 7)
        return 32 | 8 | 2;
    return 1;
}
