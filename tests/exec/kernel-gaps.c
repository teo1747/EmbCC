/* Front-end gaps found compiling the EmbLinkOS kernel (docs/todo.md K4-K9):
 * the full escape set, address-of an array object, and functions with more
 * than the old 12-parameter cap. gcc referees every value. */
// expect-exit: 42

static int gdt[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static int many(int a, int b, int c, int d, int e, int f, int g,
                int h, int i, int j, int k, int l, int m, int n) {
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n;  /* 6 regs + 8 stack */
}

int main(void) {
    /* K5: escapes — simple, GNU \e, hex, octal */
    if ('\v' != 11 || '\f' != 12 || '\a' != 7 || '\b' != 8) return 1;
    if ('\?' != 63 || '\e' != 27) return 2;
    if ('\x41' != 65 || '\101' != 65) return 3;
    char s[] = "\tx\v\x41\102\n";
    if (s[0] != 9 || s[1] != 'x' || s[2] != 11 || s[3] != 65 ||
        s[4] != 66 || s[5] != 10) return 4;

    /* K6: &array is the array's own address, of type int(*)[8] */
    if ((unsigned long)&gdt != (unsigned long)gdt) return 5;

    /* K4: a 14-argument call (spills past the 6 GP registers) */
    if (many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14) != 105) return 6;

    return 42;
}
