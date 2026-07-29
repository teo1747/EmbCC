/* Local value numbering (CSE). Redundant computations, address arithmetic, and
 * loads within a basic block are computed once — but a store between two loads
 * of the same location must FORCE a reload (memory versioning), and a `volatile`
 * access must never be eliminated (MMIO). Refereed against gcc. */
// expect-exit: 42

int printf(const char *, ...);

/* a[i] used repeatedly: address + load computed once, value reused. */
static int reuse(int *a, int i) {
    return a[i] * a[i] + a[i] - a[i];   /* = a[i]^2 */
}

/* a store between two reads of the SAME slot must reload (no stale CSE). */
static int store_between(int *a, int i, int v) {
    int x = a[i];      /* load 1 */
    a[i] = v;          /* store — invalidates the cached a[i] */
    int y = a[i];      /* load 2: must see v, not x */
    return x + y;      /* old + new */
}

/* volatile: two reads must both happen (a caller could change it between). Here
 * we just check the value is right; the point is EmbCC must not fold the reads. */
static volatile int vflag;
static int vol_reads(void) {
    vflag = 5;
    return vflag + vflag;   /* 10 — both reads of the volatile */
}

int main(void) {
    int arr[4] = { 3, 7, 11, 2 };
    if (reuse(arr, 1) != 49) return 1;        /* 7^2 */

    /* store_between(arr,2,99): x=11, then arr[2]=99, y=99 -> 110 */
    if (store_between(arr, 2, 99) != 110) return 2;
    if (arr[2] != 99) return 3;               /* the store really happened */

    if (vol_reads() != 10) return 4;

    return 42;
}
