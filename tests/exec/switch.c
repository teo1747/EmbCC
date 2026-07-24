/* switch and do-while — the two constructs the real EmbLinkOS SDK
 * (value.c/wire.c/sval.c) uses that the subset lacked.
 *
 * Exercises what a compare-chain lowering gets wrong if done casually:
 * FALLTHROUGH (no break between labels), default placed in the MIDDLE
 * rather than last, break-inside-switch-inside-loop hitting the switch
 * and not the loop, continue inside a switch reaching the LOOP, and
 * enum + negative + expression case labels. */
// expect-exit: 42
enum Kind { K_A, K_B, K_C = 10 };

static int classify(int x) {
    int n = 0;
    switch (x) {
    case 0:          /* fallthrough into case 1 */
    case 1: n = 1; break;
    default: n = 99; break;   /* default in the MIDDLE, not last */
    case K_C: n = 10; break;  /* an enum constant */
    case -3: n = 7; break;    /* a negative label */
    case 2 * 20 + 1: n = 41; break; /* a constant expression */
    }
    return n;
}
static int loop_break(void) {
    int total = 0;
    int i;
    for (i = 0; i < 6; i++) {
        switch (i) {
        case 3: continue;      /* continues the FOR, not the switch */
        case 4: break;         /* leaves the SWITCH, so the += runs */
        default: break;
        }
        total += i;            /* 0+1+2+4+5 = 12; i==3 skipped */
    }
    return total;
}
static int countdown(int n) {
    int steps = 0;
    do {                        /* body runs at least once */
        steps++;
        n--;
    } while (n > 0);
    return steps;
}
int main(void) {
    if (classify(0) != 1 || classify(1) != 1) return 1;
    if (classify(5) != 99) return 2;
    if (classify(K_C) != 10) return 3;
    if (classify(-3) != 7) return 4;
    if (classify(41) != 41) return 5;
    if (loop_break() != 12) return 6;
    if (countdown(0) != 1) return 7;   /* do-while ALWAYS runs once */
    if (countdown(5) != 5) return 8;
    return classify(41) + countdown(0); /* 41 + 1 = 42 */
}
