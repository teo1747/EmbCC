/* A void function may legally fall off its end — sema only demands a
 * return from value-returning ones. Codegen must still emit the
 * epilogue, or execution runs straight into the NEXT function's code.
 * Nothing crashes at the fall-through itself, so this is exactly the
 * silent kind of wrong THE RULE exists for: it surfaced only when a
 * real program (EmbLinkOS's value_record_set) returned into hyperspace. */
// expect-exit: 42
int printf(const char *fmt, ...);
static int marker;

static void set_it(int v) {
    marker = v;          /* no return statement — falls off the end */
}
static void nested(int v) {
    if (v > 0) {
        marker += v;     /* falls off the end of an if, too */
    }
}
static void loops(int n) {
    for (int i = 0; i < n; i++)
        marker++;
}
static void early(int v) {
    if (v < 0)
        return;          /* an explicit return AND a fall-through path */
    marker += 10;
}
int main(void) {
    set_it(5);
    if (marker != 5) return 1;
    nested(7);
    if (marker != 12) return 2;
    loops(20);
    if (marker != 32) return 3;
    early(-1);
    if (marker != 32) return 4;
    early(1);
    if (marker != 42) return 5;
    printf("void fall-through ok: %d\n", marker);
    return marker;
}
