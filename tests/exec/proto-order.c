/* Prototypes decouple declaration order from definition order: main
 * calls twice() and answer() before their definitions, through
 * prototypes — one with a named parameter, one unnamed. Also a
 * repeated (legal) prototype and an unused external declaration,
 * which must NOT produce an UNDEF symbol or relocation. */
// expect-exit: 42
static int twice(int x);
static int answer(int);
int getchar(void); /* declared, never called: must cost nothing */
static int twice(int x); /* saying it twice is fine */

int main(void) {
    return answer(twice(21));
}
static int twice(int x) { return x + x; }
static int answer(int v) { return v; }
