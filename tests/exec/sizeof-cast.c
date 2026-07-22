/* sizeof (types and expressions, LP64 answers) and explicit casts:
 * narrowing, sign changes, pointer<->pointer, and the int<->pointer
 * round-trip that needs explicit casts on both legs. */
// expect-exit: 42
static int checks(void) {
    int ok = 0;
    if (sizeof(char) == 1 && sizeof(short) == 2 && sizeof(int) == 4)
        ok++;
    if (sizeof(long) == 8 && sizeof(char *) == 8 && sizeof(void *) == 8)
        ok++;
    int x = 5;
    if (sizeof x == 4 && sizeof(x + 0L) == 8)
        ok++;
    if (sizeof(unsigned long) == sizeof(long))
        ok++;
    if ((char)300 == 44)
        ok++;
    if ((unsigned char)(0 - 1) == 255)
        ok++;
    if ((long)7 == 7L)
        ok++;
    short s = (short)70000;              /* 70000 - 65536 = 4464 */
    if (s == 4464)
        ok++;
    int y = 7;
    void *vp = &y;
    int *ip = (int *)vp;                 /* void* needs no cast; back is free too */
    if (*ip == 7)
        ok++;
    long addr = (long)&y;                /* ptr -> int needs the cast */
    int *back = (int *)addr;
    if (*back == 7)
        ok++;
    return ok; /* 10 when all hold */
}
int main(void) {
    if (checks() == 10)
        return 42;
    return checks();
}
