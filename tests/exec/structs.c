/* Structs: SysV layout with padding, member access both ways, nested
 * structs, arrays of structs, self-referential types through pointers,
 * unions sharing storage, enums, and typedef (incl. anonymous-struct
 * typedef). gcc referees layout through the sizeof checks and output. */
// expect-exit: 42
int printf(char *fmt, ...);

typedef struct Point { int x; int y; } Point;
struct Node { int val; struct Node *next; };   /* 4 + pad + 8 = 16 */
struct Mixed { char c; long l; short s; };     /* 1+7pad+8+2+6pad = 24 */
struct Outer { struct Mixed m; Point p; };     /* 24 + 8 = 32 */
union Both { int i; char bytes[4]; };
enum Color { RED, GREEN = 5, BLUE };
typedef struct { int a[3]; } Triple;           /* anonymous tag */

struct Node pool[4];                            /* global struct array */

static int sum_list(struct Node *n) {
    int s = 0;
    while (n != 0) {
        s += n->val;
        n = n->next;
    }
    return s;
}
static int checks(void) {
    int ok = 0;
    if (sizeof(Point) == 8 && sizeof(struct Node) == 16)
        ok++;
    if (sizeof(struct Mixed) == 24 && sizeof(struct Outer) == 32)
        ok++;
    if (sizeof(union Both) == 4 && sizeof(Triple) == 12)
        ok++;
    if (RED == 0 && GREEN == 5 && BLUE == 6)
        ok++;
    return ok; /* 4 */
}
int main(void) {
    if (checks() != 4)
        return 1;

    Point p;
    p.x = 12;
    p.y = 30;

    /* linked list through a global array of structs */
    pool[0].val = 20;
    pool[0].next = &pool[2];
    pool[2].val = 22;
    pool[2].next = 0;
    if (sum_list(&pool[0]) != 42)
        return 2;

    struct Outer o;
    o.m.c = 'x';
    o.m.l = 1000000000000L;
    o.p.x = 7;
    if (o.m.c != 'x' || o.m.l != 1000000000000L || o.p.x != 7)
        return 3;

    union Both u;
    u.i = 0x41424344;
    if (u.bytes[0] != 0x44 || u.bytes[3] != 0x41) /* little-endian */
        return 4;

    Triple t;
    t.a[0] = 1;
    t.a[2] = 3;
    if (t.a[0] + t.a[2] != 4)
        return 5;

    Point *pp = &p;
    pp->y = 30;
    printf("%d %d %d\n", pp->x, pp->y, sum_list(&pool[0]));
    return pp->x + pp->y; /* 42 */
}
