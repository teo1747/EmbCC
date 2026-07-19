/* The first call OUTSIDE the file: putchar via prototype, resolved by
 * the linker through R_X86_64_PLT32 relocations. Prints three stars
 * and a newline — golden/agrees-with-gcc.sh diffs the stdout too. */
// expect-exit: 42
int putchar(int c);
static int stars(int n) {
    while (n-- > 0)
        putchar(42);
    putchar(10);
    return 0;
}
int main(void) {
    stars(3);
    return 42;
}
