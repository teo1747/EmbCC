/* A global table of string pointers — the exact shape of EmbCC's own
 * predef_macros[] and keywords[]. Each pointer slot is an R_X86_64_64
 * relocation into .rodata that the linker resolves; if it doesn't
 * resolve, strcmp walks garbage and the lookup fails. */
// expect-exit: 42
struct kw { const char *word; int val; };
static const struct kw keywords[] = {
    { "int", 10 },
    { "char", 12 },
    { "return", 20 },
};
static const char *const names[] = { "alpha", "beta", "gamma" };
static int seq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
int main(void) {
    int total = 0;
    for (int i = 0; i < 3; i++)
        if (seq(keywords[i].word, keywords[i].word))
            total += keywords[i].val;              /* 10+12+20 = 42 */
    /* prove the .rodata targets are the real strings, not aliased */
    if (!seq(keywords[0].word, "int")) return 1;
    if (!seq(keywords[2].word, "return")) return 2;
    if (!seq(names[1], "beta")) return 3;
    if (seq(names[0], names[2])) return 4;         /* distinct strings */
    return total;
}
