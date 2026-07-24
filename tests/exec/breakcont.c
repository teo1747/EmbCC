/* break and continue, including the classic trap: continue in a for
 * loop must still run the STEP (or the loop never advances and this
 * program hangs instead of exiting 42). Also for(;;) with break. */
// expect-exit: 42
static int evens_sum_to(int limit) {
    int s = 0;
    int i;
    for (i = 0; i < limit; i++) {
        if (i % 2 == 1)
            continue; /* must reach i++ or this never terminates */
        s += i;
    }
    return s; /* 0+2+4+6+8+10 = 30 for limit 12 */
}
static int count_until(int stop) {
    int n = 0;
    for (;;) {
        if (n == stop)
            break;
        n++;
    }
    return n;
}
static int nested(void) {
    int hits = 0;
    int i = 0;
    while (i < 5) {
        int j = 0;
        while (j < 5) {
            j++;
            if (j == 3)
                break;      /* leaves inner loop only */
            hits++;
        }
        i++;
        if (i == 4)
            continue;       /* skips nothing vital, still terminates */
    }
    return hits; /* 2 per outer pass, 5 passes = 10 */
}
int main(void) {
    return evens_sum_to(12) + count_until(2) + nested(); /* 30+2+10 */
}
