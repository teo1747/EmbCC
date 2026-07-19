/* Compound assignment and ++/-- in both positions. Post/pre value
 * semantics matter: x++ yields the OLD value, ++x the new one. */
// expect-exit: 42
int main(void) {
    int x = 10;
    x += 20;      /* 30 */
    x -= 5;       /* 25 */
    x *= 2;       /* 50 */
    x /= 5;       /* 10 */
    x %= 7;       /* 3 */
    x <<= 4;      /* 48 */
    x >>= 1;      /* 24 */
    x |= 3;       /* 27 */
    x &= 30;      /* 26 */
    x ^= 5;       /* 31 */

    int post = x++;    /* post = 31, x = 32 */
    int pre = ++x;     /* pre = 33, x = 33 */
    int down = x--;    /* down = 33, x = 32 */
    --x;               /* x = 31 */

    /* 31 + 31 + 33 + 33 - 32 - 54 = 42 */
    return x + post + pre + down - 32 - 54;
}
