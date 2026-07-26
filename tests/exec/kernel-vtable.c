/* Kernel gaps K3 and K7: __builtin_offsetof folded to an integer constant
 * (usable in _Static_assert), a static function called before its
 * definition, and a static initializer holding function pointers (a vtable)
 * — which relocates each slot to the function's address. gcc referees it. */
// expect-exit: 42
#include <stddef.h>

struct rec { int a; char b; long c; int arr[3]; };
_Static_assert(offsetof(struct rec, a) == 0, "a");
_Static_assert(offsetof(struct rec, c) == 8, "c");
_Static_assert(offsetof(struct rec, arr[2]) == 24, "arr2");

struct ops { int (*add)(int, int); int (*mul)(int, int); };

static int use_later(int x);          /* K7: forward + defined below */
static int add(int x, int y) { return x + y; }
static int mul(int x, int y) { return x * y; }

/* the vtable: function pointers in a static aggregate initializer */
static const struct ops O = { .add = add, .mul = mul };

static int use_later(int x) { return add(x, 2); }

int main(void) {
    if (O.add(20, 20) != 40) return 1;
    if (O.mul(3, 7) != 21) return 2;
    if (use_later(40) != 42) return 3;
    return 42;
}
