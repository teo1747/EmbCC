/* The Types II payoff: string literals in .rodata reached through
 * PC32 relocations, puts, and variadic printf with al=0 per SysV.
 * Output is diffed against gcc's build by golden/agrees-with-gcc.sh. */
// expect-exit: 42
int puts(char *s);
int printf(char *fmt, ...);
int main(void) {
    puts("Hello, EmbLinkOS!");
    printf("%d %s %c!\n", 42, "stars", '*');
    printf("chained: %d %d %d\n", 1, 2, 3);
    return 42;
}
