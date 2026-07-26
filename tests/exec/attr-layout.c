/* __attribute__ honoring: packed removes padding, aligned raises alignment.
 * Getting these WRONG is a silent ABI miscompile (THE RULE), so EmbCC must
 * honor them, not ignore them. Checked against gcc via the golden harness. */
// expect-exit: 42
struct packed { char c; int i; long l; } __attribute__((packed));
struct plain  { char c; int i; long l; };
struct aligned16 { int x; } __attribute__((aligned(16)));
/* aligned(N) on a MEMBER: its offset rounds up to N and the struct's own
 * size/align rise to a multiple of N (this is the kernel `fxsave` case —
 * struct thread's fpu_state[512] aligned(16)). */
struct member_aligned { char c; char buf[512] __attribute__((aligned(16))); };
int printf(const char *, ...);
int main(void) {
    /* packed: 1 + 4 + 8 = 13, no padding */
    if (sizeof(struct packed) != 13) return 1;
    /* plain: 1 + (3 pad) + 4 + 8 = 16 */
    if (sizeof(struct plain) != 16) return 2;
    /* aligned(16): size rounds up to 16 */
    if (sizeof(struct aligned16) != 16) return 3;
    /* packed member offsets are tight */
    struct packed p;
    char *base = (char *)&p;
    if ((char *)&p.i - base != 1) return 4;   /* i right after c, no pad */
    if ((char *)&p.l - base != 5) return 5;
    /* aligned(16) member: buf at offset 16, struct size a multiple of 16 */
    if (__builtin_offsetof(struct member_aligned, buf) != 16) return 6;
    if (sizeof(struct member_aligned) != 528) return 7;
    struct member_aligned m;
    if (((unsigned long)&m.buf & 15) != 0) return 8;  /* buf 16-aligned in RAM */
    /* aligned(16) on a stack LOCAL: its address is 16-aligned */
    unsigned char observed[16] __attribute__((aligned(16)));
    observed[0] = 1;                                   /* touch it (no DCE) */
    if (((unsigned long)observed & 15) != 0) return 9;
    return 42;
}
