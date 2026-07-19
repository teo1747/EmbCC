/* All six register args (r8d/r9d take the REX-prefixed encodings),
 * locals with initializers, precedence, and parentheses.
 * g = 2 + 2*3 = 8; h = (10+8)*2 = 36; 8 + 36 - 1 - 1 = 42. */
// expect-exit: 42
static int mix(int a, int b, int c, int d, int e, int f) {
    int g = a + 2 * b;
    int h = (c + d) * 2;
    return g + h - e - f;
}
int main(void) {
    return mix(2, 3, 10, 8, 1, 1);
}
