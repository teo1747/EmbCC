/* while and for, assignment statements, compound conditions.
 * sum 1..6 = 21, doubled by the for-loop = 42. */
// expect-exit: 42
static int sum_to(int n) {
    int s = 0;
    int i = 1;
    while (i <= n) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
int main(void) {
    int total = 0;
    int j;
    for (j = 0; j < 2; j = j + 1) {
        total = total + sum_to(6);
    }
    return total;
}
