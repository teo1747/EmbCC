#!/bin/sh
# The cross-ABI test: half the program compiled by GCC, half by EmbCC,
# linked together and run. Struct passing is the one area where a
# mistake does not crash — it silently puts an argument in the wrong
# register and the callee reads garbage — so agreeing with ourselves
# proves nothing. Only calling gcc's code, and being called BY it,
# proves the SysV classification is right.
set -u
echo "TEST-MARKER sysv-abi"

out_dir="tests/golden/out/abi"
rm -rf "$out_dir"; mkdir -p "$out_dir"

cat > "$out_dir/abi.h" << 'EOF'
struct Small  { int a; int b; };
struct Two    { long a; long b; };
struct Dbl    { double x; double y; };
struct Mixed  { long n; double d; };
struct Big    { long a; long b; long c; long d; };
long gcc_small(struct Small s);
long gcc_two(struct Two t);
double gcc_dbl(struct Dbl d);
double gcc_mixed(struct Mixed m);
long gcc_big(struct Big b);
struct Small gcc_mk_small(int a, int b);
struct Mixed gcc_mk_mixed(long n, double d);
struct Big gcc_mk_big(long base);
long gcc_offset(int p, double q, struct Mixed m, int r);
EOF

# --- the GCC half: it defines, EmbCC calls ---
cat > "$out_dir/gcchalf.c" << 'EOF'
#include "abi.h"
long gcc_small(struct Small s) { return s.a + s.b; }
long gcc_two(struct Two t) { return t.a * t.b; }
double gcc_dbl(struct Dbl d) { return d.x + d.y; }
double gcc_mixed(struct Mixed m) { return m.n * m.d; }
long gcc_big(struct Big b) { return b.a + b.b + b.c + b.d; }
struct Small gcc_mk_small(int a, int b) { struct Small s = {a, b}; return s; }
struct Mixed gcc_mk_mixed(long n, double d) { struct Mixed m = {n, d}; return m; }
struct Big gcc_mk_big(long base) {
    struct Big b = {base, base+1, base+2, base+3}; return b;
}
long gcc_offset(int p, double q, struct Mixed m, int r) {
    return p + (long)q + m.n + (long)m.d + r;
}
EOF

# --- the EmbCC half: it calls gcc's, both directions ---
cat > "$out_dir/embhalf.c" << 'EOF'
#include "abi.h"
int printf(const char *fmt, ...);
int main(void) {
    struct Small s; s.a = 20; s.b = 22;
    if (gcc_small(s) != 42) return 1;
    struct Two t; t.a = 6; t.b = 7;
    if (gcc_two(t) != 42) return 2;
    struct Dbl d; d.x = 20.5; d.y = 21.5;
    if (gcc_dbl(d) != 42.0) return 3;
    struct Mixed m; m.n = 21; m.d = 2.0;
    if (gcc_mixed(m) != 42.0) return 4;
    struct Big b; b.a = 9; b.b = 10; b.c = 11; b.d = 12;
    if (gcc_big(b) != 42) return 5;

    /* now the other direction: gcc RETURNS structs to us */
    struct Small rs = gcc_mk_small(20, 22);
    if (rs.a != 20 || rs.b != 22) return 6;
    struct Mixed rm = gcc_mk_mixed(21, 2.0);
    if (rm.n != 21 || rm.d != 2.0) return 7;
    struct Big rb = gcc_mk_big(9);
    if (rb.a != 9 || rb.d != 12) return 8;

    /* a struct argument after other arguments of both classes */
    if (gcc_offset(1, 2.0, rm, 16) != 42) return 9;

    /* and hand gcc's returned structs straight back to gcc */
    if (gcc_small(rs) != 42) return 10;
    if (gcc_big(rb) != 42) return 11;
    printf("cross-abi ok\n");
    return gcc_small(rs);
}
EOF

cc -std=c99 -c "$out_dir/gcchalf.c" -o "$out_dir/gcchalf.o" -I "$out_dir" || {
    echo "gcc half failed to build"; exit 1; }
"$EMBCC" -c "$out_dir/embhalf.c" -o "$out_dir/embhalf.o" -I "$out_dir" || {
    echo "embcc half failed to build"; exit 1; }
cc -no-pie -o "$out_dir/prog" "$out_dir/embhalf.o" "$out_dir/gcchalf.o" || {
    echo "link failed"; exit 1; }
out=$("$out_dir/prog"); got=$?
[ "$got" -eq 42 ] || {
    echo "cross-ABI run exited $got (a nonzero N is the Nth check above)"
    exit 1; }
[ "$out" = "cross-abi ok" ] || { echo "wrong output: $out"; exit 1; }
echo "EmbCC and GCC agree on SysV struct passing, both directions"
