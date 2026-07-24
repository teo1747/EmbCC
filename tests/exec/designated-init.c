/* Struct field designators, positional-after-designator, partial init
 * zero-filling, and designators inside a nested/global aggregate — the
 * shape of EmbCC's own bases[7][2] type table. */
// expect-exit: 42
struct type { int kind; int is_unsigned; int rank; };
/* global 2D array of designated structs (type.c's exact pattern) */
static struct type tbl[2][2] = {
    { { .kind = 1 }, { .kind = 1, .is_unsigned = 1 } },
    { { .kind = 2, .rank = 3 }, { .kind = 9 } },
};
int main(void) {
    /* local designated with continue-positionally: .is_unsigned=1 then 7
     * lands in the next member (rank). */
    struct type t = { .kind = 5, .is_unsigned = 1, 7 };
    if (t.kind != 5 || t.is_unsigned != 1 || t.rank != 7) return 1;
    /* partial init: unspecified members are zero */
    struct type z = { .rank = 9 };
    if (z.kind != 0 || z.is_unsigned != 0 || z.rank != 9) return 2;
    /* global table read-back */
    if (tbl[0][0].kind != 1 || tbl[0][0].is_unsigned != 0) return 3;
    if (tbl[0][1].is_unsigned != 1) return 4;
    if (tbl[1][0].kind != 2 || tbl[1][0].rank != 3) return 5;
    if (tbl[1][0].is_unsigned != 0) return 6;      /* zero-filled */
    if (tbl[1][1].kind != 9 || tbl[1][1].rank != 0) return 7;
    return 42;
}
