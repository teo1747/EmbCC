/* _Static_assert (C11 6.7.10): a compile-time check that emits no code.
 * Legal at file scope, inside a struct body, and in a block. A true
 * assertion produces nothing; the message is optional. (A false one is a
 * fatal error — see the reject suite.) */
// expect-exit: 42

_Static_assert(sizeof(int) == 4, "int must be 4 bytes");
_Static_assert(sizeof(long) == 8 && sizeof(void *) == 8, "LP64");
_Static_assert(1, "always true");
_Static_assert(2 + 2 == 4);              /* no-message form */

struct S {
    int a;
    _Static_assert(sizeof(char) == 1, "char is 1");
    int b;
};

int main(void) {
    _Static_assert(sizeof(struct S) == 8, "two ints, no padding");
    return 42;
}
