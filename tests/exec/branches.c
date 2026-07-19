/* if/else chains, both-arms-return path analysis, comparisons as
 * values, dangling else. classify: 40 -> 3; sign(-5 via 0-5) = 0-1;
 * (3 * 14) = 42 exercised through the branches. */
// expect-exit: 42
static int classify(int x) {
    if (x < 10) {
        return 1;
    } else if (x < 30) {
        return 2;
    } else {
        return 3;
    }
}
static int eq_as_value(int a, int b) {
    return (a == b) + (a != b) * 2; /* always 1 or 2 */
}
static int pick(int c, int a, int b) {
    if (c)
        return a;
    return b;
}
int main(void) {
    int base = classify(40) * 14;            /* 42 */
    int adjust = eq_as_value(5, 5) - 1;      /* 0 */
    return pick(adjust == 0, base, 0);       /* 42 */
}
