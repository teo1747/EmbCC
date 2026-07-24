/* A variable is in scope inside its own initializer (C11 6.2.1p7). The
 * idiom `T *p = alloc(sizeof *p)` appears throughout EmbCC's own source
 * (xcalloc(1, sizeof *c)), so self-hosting depends on it. */
// expect-exit: 42
typedef unsigned long size_t;
void *calloc(size_t, size_t);
struct node { int v; struct node *next; };
int main(void) {
    struct node *a = calloc(1, sizeof *a);   /* self-ref in init */
    a->v = 40;
    struct node *b = calloc(1, sizeof *b);
    b->v = 2;
    a->next = b;
    return a->v + a->next->v;                 /* 42 */
}
