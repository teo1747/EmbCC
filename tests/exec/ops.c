/* Division and modulo are signed and truncate toward zero (idiv):
 * -7/2 = -3 (not -4), -7%2 = -1, 7%-2 = 1. Unary minus and ~.
 * (-7/2)*(-10) = 30; -7%2 + 1 = 0; 84/2 = 42 through the maze. */
// expect-exit: 42
static int checks(void) {
    int a = -7;
    int ok = 0;
    if (a / 2 == -3)
        ok = ok + 1;
    if (a % 2 == -1)
        ok = ok + 1;
    if (7 % -2 == 1)
        ok = ok + 1;
    if (-(0 - 5) == 5)
        ok = ok + 1;
    if (~0 == -1)
        ok = ok + 1;
    return ok; /* 5 when all hold */
}
int main(void) {
    if (checks() == 5)
        return 84 / 2;
    return 1;
}
