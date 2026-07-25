/* Bitfields: little-endian gcc-compatible layout and access. Covers
 * unsigned and signed fields, packing into one storage unit, an anonymous
 * padding field, a zero-width separator, `__attribute__((packed))`, access
 * through a pointer, long (>32-bit) fields, ++/+= on a field, and the
 * truncation an out-of-range store must produce. gcc referees every value
 * and sizeof; the whole thing returns 42 only if all of them agree. */
// expect-exit: 42

struct F { unsigned a:3; unsigned b:5; int c:20; };  /* one 4-byte unit */
struct G { unsigned char x:1; unsigned char y:7; };  /* one byte */
struct S { int s:4; };                               /* signed -8..7 */
struct P { unsigned a:5; unsigned b:5; } __attribute__((packed));
struct Z { int a:5; int :0; int b:5; };              /* :0 forces a new unit */
struct R { unsigned en:1; unsigned mode:3; unsigned :4; unsigned prio:8; };
struct L { unsigned long lo:40; unsigned long hi:20; };
struct M { char tag; int flag:1; int val:20; char tail; };

static int viaptr(struct R *r) {
    r->en = 1; r->mode = 6; r->prio = 200;
    return r->en + r->mode + r->prio;
}

int main(void) {
    struct F f; f.a = 5; f.b = 17; f.c = 100000;
    if (sizeof(struct F) != 4) return 1;
    if (f.a != 5 || f.b != 17 || f.c != 100000) return 2;
    f.a++; f.b += 20;                 /* a:6  b:(17+20)&31 = 5 */
    if (f.a != 6 || f.b != 5) return 3;

    struct G g; g.x = 1; g.y = 100;
    if (sizeof(struct G) != 1 || g.x != 1 || g.y != 100) return 4;

    struct S s; s.s = 5; s.s += 4;    /* 9 -> wraps signed to -7 */
    if (sizeof(struct S) != 4 || s.s != -7) return 5;
    struct S s2; s2.s = -3; if (s2.s != -3) return 6;

    struct P p; p.a = 17; p.b = 9;
    if (sizeof(struct P) != 2 || p.a != 17 || p.b != 9) return 7;

    if (sizeof(struct Z) != 8) return 8;

    struct R r; r.en = 0; r.mode = 0; r.prio = 0;
    if (viaptr(&r) != 1 + 6 + 200) return 9;
    if (r.en != 1 || r.mode != 6 || r.prio != 200) return 10;
    r.mode = 100;                     /* 100 & 7 = 4 */
    if (r.mode != 4) return 11;

    struct L l; l.lo = 1000000000000UL; l.hi = 500000;
    if (sizeof(struct L) != 8) return 12;
    if (l.lo != 1000000000000UL || l.hi != 500000) return 13;

    struct M m; m.tag = 'A'; m.flag = 1; m.val = -5; m.tail = 'Z';
    if (m.tag != 'A' || m.tail != 'Z') return 14;
    if (m.flag != -1 || m.val != -5) return 15;   /* int:1 is signed */

    return 42;
}
