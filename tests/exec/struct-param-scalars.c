/* Regression: a struct-by-value parameter followed by scalar parameters. The
 * struct copy in the prologue used an INTEGER ARGUMENT register (rcx) as its
 * destination-pointer scratch, clobbering a later scalar param that arrives in
 * that register -- so `f(struct{long,long}, long a, long b)` silently dropped b
 * (and the MEMORY-class path had the same bug on a bigger struct). Both copies
 * now use a non-argument scratch. Refereed against gcc; run at -O0/-O1/-O2. */
// expect-exit: 42

struct Two { long x, y; };            /* 16 bytes -> two INTEGER eightbytes (regs) */
struct Big { long a, b, c; };         /* 24 bytes -> MEMORY class (passed on stack) */

/* register-class struct then 1, 2, 3 scalars (2+ was the broken case) */
static long r1(struct Two s, long a)                 { return s.x + s.y + a; }
static long r2(struct Two s, long a, long b)         { return s.x + s.y + a + b; }
static long r3(struct Two s, long a, long b, long c) { return s.x + s.y + a + b + c; }
/* two structs back to back (the first copy must not clobber the second's regs) */
static long ss(struct Two a, struct Two b)           { return a.x + a.y + b.x + b.y; }
/* a scalar, then a struct, then a scalar */
static long sas(long p, struct Two s, long q)        { return p + s.x + s.y + q; }
/* MEMORY struct then four scalars (d arrives in rcx) */
static long mem(struct Big s, long a, long b, long c, long d)
{ return s.a + s.b + s.c + a + b + c + d; }

int main(void)
{
    struct Two t = { 10, 20 }, u = { 30, 40 };
    struct Big g = { 100, 200, 300 };
    if (r1(t, 1)          != 31)  return 1;
    if (r2(t, 1, 2)       != 33)  return 2;
    if (r3(t, 1, 2, 3)    != 36)  return 3;
    if (ss(t, u)          != 100) return 4;
    if (sas(1, t, 2)      != 33)  return 5;
    if (mem(g, 1, 2, 3, 4) != 610) return 6;
    return 42;
}
