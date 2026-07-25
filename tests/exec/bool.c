/* The real _Bool type (C99): sizeof is 1, and a store normalizes any
 * nonzero scalar to 1 — integers, pointers, and floats alike. <stdbool.h>
 * just makes `bool` a macro for it (mirrored here so the test needs no
 * header). gcc referees every value. */
// expect-exit: 42
#define bool _Bool
#define true 1
#define false 0

static _Bool ret_bool(int x) { return x; }   /* return also normalizes */

int main(void) {
    if (sizeof(_Bool) != 1 || sizeof(bool) != 1) return 1;

    _Bool a = 5;                 /* -> 1 */
    if (a != 1) return 2;
    bool b = 0;
    if (b) return 3;
    b = 42;                      /* -> 1 */
    if (b != 1) return 4;

    int *p = (int *)0;
    _Bool nb = p;                /* NULL pointer -> false */
    if (nb) return 5;
    int x = 100;
    _Bool pb = &x;               /* non-null pointer -> true */
    if (!pb) return 6;

    _Bool fb = 0.5;              /* float 0.5 -> true (not truncated to 0) */
    if (fb != 1) return 7;
    _Bool zb = 0.0;
    if (zb) return 8;

    if (ret_bool(77) != 1 || ret_bool(0) != 0) return 9;

    bool arr[3];                 /* an array of them is 3 bytes */
    arr[0] = true; arr[1] = false; arr[2] = 7;
    if (sizeof(arr) != 3) return 10;
    if (arr[0] != 1 || arr[1] != 0 || arr[2] != 1) return 11;

    /* a leading statement before the declarations once mis-ordered the
     * normalize compare — guard it stays fixed */
    if (sizeof(char) != 1) return 12;
    _Bool c = 9; _Bool d = 0;
    if (c != 1 || d != 0) return 13;

    return 42;
}
