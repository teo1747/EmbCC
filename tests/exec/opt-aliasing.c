/* Guards the codegen register (RAX) cache against the aliasing it must not
 * break: a local read after being modified through a pointer, two pointers
 * to the same local, pointer chains, and array access in a loop. The cache
 * only keeps TEMPS in a register (their slots are never address-taken) and
 * always reloads a variable from memory, so all of this must still match
 * gcc — at -O0 (make test) and at -O1 (the optimizer golden test). */
// expect-exit: 42

struct N { int v; struct N *next; };

static int sumlist(struct N *h) {
    int s = 0;
    while (h) { s += h->v; h = h->next; }
    return s;
}

int main(void) {
    int x = 10; int *p = &x;
    *p = 20; if (x != 20) return 1;          /* write via ptr, read via name */
    x = 5;   if (*p != 5) return 2;          /* write via name, read via ptr */
    *p += 7; if (x != 12) return 3;

    struct N c = {3, 0}, b = {2, &c}, a = {1, &b};
    if (sumlist(&a) != 6) return 4;

    int arr[6]; int *q = arr;
    for (int i = 0; i < 6; i++) q[i] = i * i;
    int t = 0; for (int i = 0; i < 6; i++) t += arr[i];
    if (t != 0 + 1 + 4 + 9 + 16 + 25) return 5;

    long y = 100; long *r1 = &y, *r2 = &y;    /* two aliases of one local */
    *r1 = 40; *r2 += 2; if (y != 42) return 6;

    if (sumlist(&a) + sumlist(&b) + sumlist(&c) != 6 + 5 + 3) return 7;
    return 42;
}
