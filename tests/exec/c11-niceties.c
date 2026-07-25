/* Two C11 conveniences: _Generic type-directed selection and anonymous
 * struct/union members. gcc referees every value. */
// expect-exit: 42

/* _Generic: the arm whose type matches the controlling expression is
 * selected at compile time; the controlling expression is NOT evaluated. */
#define kind(x) _Generic((x), \
    int: 1, long: 2, double: 3, char: 4, int *: 5, default: 0)

int calls;
static int bump(void) { calls++; return 0; }

/* Anonymous members: the inner names are reached through the outer object. */
struct Var {
    int tag;
    union { long i; double d; char bytes[8]; };  /* anonymous union */
    struct { int lo, hi; };                       /* anonymous struct */
};

int main(void) {
    int i = 0; long l = 0; double d = 0; char c = 0; int *p = 0;
    if (kind(i) != 1 || kind(l) != 2 || kind(d) != 3) return 1;
    if (kind(c) != 4 || kind(p) != 5) return 2;
    if (kind("s") != 0) return 3;                 /* char* not listed */

    if (_Generic(bump(), int: 7, default: 0) != 7 || calls != 0) return 4;

    struct Var v;
    v.tag = 9;
    v.i = 0x1122334455667788L;
    if (v.bytes[0] != (char)0x88) return 5;       /* union aliases i */
    v.d = 2.5; if (v.d != 2.5) return 6;
    v.lo = 11; v.hi = 22;
    if (v.lo != 11 || v.hi != 22 || v.tag != 9) return 7;

    return 42;
}
