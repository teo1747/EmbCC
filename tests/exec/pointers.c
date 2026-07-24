/* Pointers: & and *, writes through them, pointers as parameters and
 * return values, pointer-to-pointer, p[i] sugar, null comparison. */
// expect-exit: 42
static int swap(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
    return 0;
}
static int *pick(int *a, int *b, int which) {
    if (which)
        return a;
    return b;
}
static int through(int **pp) {
    return **pp;
}
int main(void) {
    int x = 40;
    int y = 2;
    swap(&x, &y);              /* x=2 y=40 */
    int *p = pick(&x, &y, 0);  /* p = &y */
    *p = *p + 1;               /* y = 41 */
    p[0] = p[0] + 1;           /* y = 42 via index sugar */
    int *q = 0;
    if (q != 0)
        return 1;
    int **pp = &p;
    return through(pp);        /* 42 */
}
