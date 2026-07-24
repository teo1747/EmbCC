/* Adjacent string literals concatenate (C translation phase 6) — used
 * all over EmbCC's own source for long diagnostic messages, so a
 * self-hosting requirement. Multi-way and length must be right. */
// expect-exit: 42
int printf(const char *fmt, ...);
static int len(const char *s){ int n=0; while(s[n]) n++; return n; }
int main(void) {
    const char *two = "ab" "cd";
    const char *many = "The " "answer " "is " "42";
    if (len(two) != 4) return 1;
    if (two[0]!='a'||two[1]!='b'||two[2]!='c'||two[3]!='d') return 2;
    if (len(many) != 16) return 3;
    if (sizeof("x" "y" "z") != 4) return 4;       /* 3 chars + NUL */
    printf("%s\n", "concat " "works");
    return len(two) * 10 + 2;                      /* 4*10+2 = 42 */
}
