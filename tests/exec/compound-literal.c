/* Compound literals (C99 6.5.2.5): `(type){ init }` is an unnamed object
 * with automatic storage. Covers taking its address, passing it by value,
 * member access straight off the literal, an array literal that decays and
 * is indexed, a scalar literal, designated fields inside one, and using a
 * literal to initialize a local. gcc referees every value. */
// expect-exit: 42

struct P { int x, y; };

static int psum(struct P *p) { return p->x + p->y; }
static int vsum(struct P p)  { return p.x + p.y; }

int main(void) {
    struct P *q = &(struct P){ .x = 5, .y = 7 };   /* address of a literal */
    if (psum(q) != 12) return 1;

    if (vsum((struct P){ 3, 4 }) != 7) return 2;   /* pass by value */

    if ((struct P){ .x = 10, .y = 20 }.y != 20) return 3;  /* member off it */

    int v = (int[]){ 11, 22, 33 }[1];              /* array decays, indexed */
    if (v != 22) return 4;

    int s = (int){ 42 };                           /* scalar literal */
    if (s != 42) return 5;

    struct P p = (struct P){ 8, 9 };               /* initialize a local */
    if (p.x != 8 || p.y != 9) return 6;

    /* an array literal with a gap left zero by a designator */
    int *a = (int[4]){ [3] = 99 };
    if (a[0] || a[1] || a[2] || a[3] != 99) return 7;

    return 42;
}
