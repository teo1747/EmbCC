/* Recursion needs control flow to terminate — first possible here.
 * No division in the subset yet, so gcd reduces by subtraction.
 * fib(9) = 34; gcd(24, 16) = 8; 34 + 8 = 42. */
// expect-exit: 42
static int fib(int n) {
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}
static int gcd(int a, int b) {
    while (b != 0) {
        while (a >= b) {
            a = a - b;
        }
        int t = a;
        a = b;
        b = t;
    }
    return a;
}
int main(void) {
    return fib(9) + gcd(24, 16);
}
