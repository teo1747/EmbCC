/* char: literals with escapes, signed narrowing (300 -> 44 on x86),
 * unsigned char wraparound, sign-extension on load, char through a
 * pointer at byte width, and real output via putchar. */
// expect-exit: 42
int putchar(int c);
int main(void) {
    char c = 'A';
    putchar(c);         /* A */
    putchar('B');       /* B */
    putchar('\n');

    char n = 300;              /* (char)300 = 44 */
    unsigned char u = 255;
    u = u + 1;                 /* wraps to 0 */
    char m = -2;               /* sign-extends on load */

    char *pc = &c;
    *pc = 'Z';                 /* byte store through pointer */
    if (c != 'Z')
        return 1;
    if (u != 0)
        return 2;
    if (m + 2 != 0)
        return 3;
    return n - 2;              /* 42 */
}
