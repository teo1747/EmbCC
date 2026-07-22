/* Local arrays: real frame storage, indexing both ways, char arrays at
 * byte width, 2-D row-major layout, sizeof (no decay), and the C
 * adjustment of an array parameter to a pointer. */
// expect-exit: 42
static int sum(int a[], int n) { /* int a[] adjusts to int * */
    int s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += a[i];
    return s;
}
int main(void) {
    int a[10];
    int i;
    for (i = 0; i < 10; i++)
        a[i] = i;                  /* 0..9, sum 45 */
    if (sizeof a != 40)
        return 1;
    if (sizeof(a[0]) != 4)
        return 2;

    char buf[8];
    buf[0] = 'x';
    buf[7] = 'y';                  /* byte stores at both ends */
    if (buf[0] != 'x' || buf[7] != 'y')
        return 3;

    int m[3][4];
    int r;
    int c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            m[r][c] = r * 4 + c;   /* 0..11 row-major */
    if (sizeof m != 48 || sizeof m[1] != 16)
        return 4;
    if (m[2][3] != 11 || m[0][0] != 0)
        return 5;
    if (*(*(m + 1) + 2) != 6)      /* the sugar written out by hand */
        return 6;

    int *p = a;                    /* decay in initialization */
    if (p[9] != 9)
        return 7;
    return sum(a, 10) - 3;         /* 45 - 3 = 42 */
}
