/* A call to a noreturn function (exit here) terminates control flow, so a
 * non-void function may end in one without a return — EmbCC's own code
 * does this at internal-error tails (fprintf+exit). Sound: exit never
 * returns. */
// expect-exit: 42
void exit(int);
static int pick(int x) {
    switch (x) {
    case 1: return 42;
    default:
        exit(7);          /* no return after: control cannot fall through */
    }
}
int main(void) { return pick(1); }
