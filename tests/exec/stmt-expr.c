/* GNU statement expressions `({ ... })` — the value is the last statement
 * when that is an expression statement. Used pervasively in kernel macros
 * (a hygienic MAX, timed reads). gcc referees every value. */
// expect-exit: 42

#define MAX(a, b) ({ int _x = (a), _y = (b); _x > _y ? _x : _y; })

int main(void) {
    if (MAX(10, 42) != 42 || MAX(-1, -9) != -1) return 1;

    int n = ({ int s = 0; for (int i = 1; i <= 5; i++) s += i; s; });
    if (n != 15) return 2;

    if (({ 7; }) != 7) return 3;                    /* single-expression form */

    int q = ({ int a = ({ 3; }); a * 2; });        /* nested */
    if (q != 6) return 4;

    /* value used directly in a larger expression */
    if (({ int t = 20; t; }) + 22 != 42) return 5;

    return 42;
}
