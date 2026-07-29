/* K13 stress: temporary stack-slot coalescing must never let a live temp's
 * slot be reused while it still holds a needed value. This packs many
 * simultaneously-live temporaries, control flow that makes some temps cross
 * basic blocks (?: , && , ||), and loops that reuse temps every iteration —
 * the exact shapes a wrong live-range would corrupt. Every result is checked;
 * the whole file is refereed against gcc by the golden harness. */
// expect-exit: 42

/* eight temps all live at once: their sum must be exact (a clobbered slot
 * would drop or duplicate a term). */
static int wide(int a, int b, int c, int d, int e, int f, int g, int h) {
    int t0 = a * 2, t1 = b * 3, t2 = c * 5, t3 = d * 7;
    int t4 = e * 11, t5 = f * 13, t6 = g * 17, t7 = h * 19;
    /* interleave so all eight are live across the final combine */
    int lo = (t0 + t2) + (t4 + t6);
    int hi = (t1 + t3) + (t5 + t7);
    return lo + hi;
}

/* ?: and && / || create temps live ACROSS block boundaries (non-coalescable);
 * they must keep their values through the branch. */
static int cross(int x, int y, int z) {
    int p = (x > y) ? (x + 100) : (y + 200);   /* p lives across the merge */
    int q = (x && y) ? (z * 3) : (z * 5);
    int r = (x > 0 && y > 0 && z > 0) ? 1 : 0;
    return p + q + r;
}

/* a loop whose body defines temps each iteration: reuse across iterations must
 * not carry a stale value in. */
static long loopsum(int n) {
    long acc = 0;
    for (int i = 0; i < n; i++) {
        int a = i + 1;
        int b = a * a;          /* fresh temps each iteration */
        int c = b - a;
        acc += a + b + c;
    }
    return acc;
}

int main(void) {
    if (wide(1, 1, 1, 1, 1, 1, 1, 1) != (2+3+5+7+11+13+17+19)) return 1;
    if (wide(2, 0, 3, 0, 4, 0, 5, 0) != (4 + 15 + 44 + 85)) return 2;

    /* cross(5,3,4): p = 5+100 = 105; q = (5&&3)?4*3:.. = 12; r = 1 -> 118 */
    if (cross(5, 3, 4) != 118) return 3;
    /* cross(2,9,3): p = (2>9)?..:9+200 = 209; q = (2&&9)?3*3 = 9; r=1 -> 219 */
    if (cross(2, 9, 3) != 219) return 4;
    /* cross(-1,9,3): p = 9+200=209; q=((-1)&&9)?9=9; r=(-1>0..)=0 -> 218 */
    if (cross(-1, 9, 3) != 218) return 5;

    /* loopsum(4): i=0:1+1+0=2 wait a=i+1 so i=0->a1,b1,c0 =2; i=1->a2,b4,c2=8;
     * i=2->a3,b9,c6=18; i=3->a4,b16,c12=32; sum=2+8+18+32=60 */
    if (loopsum(4) != 60) return 6;
    if (loopsum(0) != 0) return 7;

    return 42;
}
