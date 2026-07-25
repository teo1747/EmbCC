/* Designated array initializers: `int a[5] = { [2]=9, [4]=1 }`. Covers
 * file-scope and local arrays, unsized arrays whose size is the highest
 * index reached, gaps left zero, a designator that repositions the running
 * index with positional elements continuing after it, and a later write to
 * a slot overriding an earlier one. gcc referees every value. */
// expect-exit: 42

int g[5] = { [2] = 9, [4] = 1 };            /* gaps are zero */
int gu[]  = { [3] = 7, [1] = 2 };           /* unsized -> 4 elements */
int over[3] = { [0] = 1, [0] = 8, [2] = 3 };/* last write to [0] wins */

int main(void) {
    if (sizeof(gu) != 16) return 1;
    if (g[0] || g[1] || g[2] != 9 || g[3] || g[4] != 1) return 2;
    if (gu[0] || gu[1] != 2 || gu[2] || gu[3] != 7) return 3;
    if (over[0] != 8 || over[1] || over[2] != 3) return 4;

    int a[5] = { [2] = 9, [4] = 1 };
    if (a[0] || a[1] || a[2] != 9 || a[3] || a[4] != 1) return 5;

    int b[] = { [3] = 7, [1] = 2 };
    if (sizeof(b) != 16 || b[0] || b[1] != 2 || b[2] || b[3] != 7) return 6;

    /* positional, then jump to [4], then continue positionally into [5] */
    int mix[6] = { 1, 2, [4] = 5, 6 };
    if (mix[0] != 1 || mix[1] != 2 || mix[2] || mix[3] ||
        mix[4] != 5 || mix[5] != 6) return 7;

    return 42;
}
