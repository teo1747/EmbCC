/* File-scope variables: .data (initialized), .bss (zero — the loader
 * must hand us zeroed memory), static locals-of-the-file, extern
 * declaration merging with a later definition, global arrays, and
 * state that survives across calls. */
// expect-exit: 42
int printf(char *fmt, ...);

int counter = 30;              /* .data */
static char tag = 'X';         /* .data, LOCAL symbol */
long big;                      /* .bss */
int hits[8];                   /* .bss, zeroed by the loader */
static long neginit = -7;      /* negative initializer */
extern int shared;             /* declaration... */
int shared = 10;               /* ...merged with this definition */

static int bump(void) {
    counter++;
    big = big + 1000000000000L;
    hits[3] = hits[3] + 2;
    return 0;
}
static int zeros(void) {
    int i;
    int z = 0;
    for (i = 0; i < 8; i++)
        if (i != 3 && hits[i] != 0)
            z = 1;             /* .bss was not zeroed */
    return z;
}
int main(void) {
    bump();
    bump();
    if (zeros())
        return 1;
    if (neginit != -7)
        return 2;
    if (tag != 'X')
        return 3;
    long *pb = &big;           /* address of a global */
    if (*pb != 2000000000000L)
        return 4;
    printf("%d %ld %d %d\n", counter, big / 1000000000000L,
           hits[3], shared);
    return counter + hits[3] + shared - 4; /* 32+4+10-4 = 42 */
}
