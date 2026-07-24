/* Nested calls, calls as call arguments, recursion-legal ordering
 * (every function defined before its callers — M1 has no prototypes).
 * dbl(20) = 40; sub2(4) = 4 - 0 - 4 + 4 - 2 = 2; add(40, 2) = 42. */
// expect-exit: 42
static int mul(int a, int b) { return a * b; }
static int dbl(int x) { return mul(x, 2); }
static int sub2(int x) { return x - x * 0 - x + x - 2; }
static int add(int a, int b) { return a + b; }
int main(void) {
    return add(dbl(20), sub2(4));
}
