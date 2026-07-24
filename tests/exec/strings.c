/* String literals as objects: char* bindings, byte indexing, a
 * hand-rolled strlen over NUL termination, sizeof (array, not
 * pointer), escapes, and identical literals sharing storage. */
// expect-exit: 42
int putchar(int c);
static int len(char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}
int main(void) {
    char *msg = "answer";
    if (len(msg) != 6)
        return 1;
    if (msg[0] != 'a' || msg[5] != 'r')
        return 2;
    if (sizeof("answer") != 7)     /* array of 7, not a pointer */
        return 3;
    if (sizeof(msg) != 8)          /* the pointer is 8 */
        return 4;
    char *tab = "a\tb\n";
    if (tab[1] != '\t' || tab[3] != '\n')
        return 5;
    char *x = "same";
    char *y = "same";
    if (x != y)                    /* interned into one .rodata entry */
        return 6;
    putchar('o');
    putchar('k');
    putchar('\n');
    return len("the answer is forty-two, always") + 11; /* 31 + 11 */
}
