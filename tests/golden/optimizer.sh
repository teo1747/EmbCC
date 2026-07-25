#!/bin/sh
# The optimizer at -O1: the IR passes (src/opt) AND codegen's RAX residency
# cache that elides redundant reloads. Three properties, each load-bearing:
#
#  1. -O0 is byte-for-byte the no-flag output. The self-host fixed point
#     rests on this, so it is asserted here directly, not just assumed.
#  2. -O1 preserves semantics. Every tests/exec program is recompiled at
#     -O1 and must still produce its `// expect-exit` value — the same
#     differential net the -O0 suite is, now over the optimized path.
#  3. -O1 actually optimizes: a program full of foldable/dead/copy work,
#     plus store/reload traffic, compiles to a strictly smaller object.
set -u
echo "TEST-MARKER optimizer"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/optimizer
rm -rf "$out"; mkdir -p "$out"

# 1. -O0 == no flag, byte for byte.
prog="$out/id.c"
cat > "$prog" <<'EOF'
int f(int x){ int a = 2 + 3; int b = x * 1; int c = a + 0; return b + c + x; }
int main(void){ return f(20); }
EOF
"$EMBCC" -c "$prog" -o "$out/none.o"    || { echo "compile (no flag) failed"; exit 1; }
"$EMBCC" -c -O0 "$prog" -o "$out/o0.o"  || { echo "compile -O0 failed"; exit 1; }
cmp -s "$out/none.o" "$out/o0.o" || { echo "-O0 differs from no-flag output"; exit 1; }
echo "-O0 is byte-identical to unoptimized output"

# 3. -O1 optimizes: fold (2+3), identity (x*1, a+0), and the dead temps they
#    leave must shrink the object.
"$EMBCC" -c -O1 "$prog" -o "$out/o1.o"  || { echo "compile -O1 failed"; exit 1; }
s0=$(stat -c%s "$out/o0.o"); s1=$(stat -c%s "$out/o1.o")
[ "$s1" -lt "$s0" ] || { echo "-O1 did not shrink the object ($s0 -> $s1)"; exit 1; }
echo "-O1 folds/eliminates: object $s0 -> $s1 bytes"

# 2. -O1 preserves the semantics of every exec test.
n=0
for c in tests/exec/*.c; do
    [ -e "$c" ] || continue
    exp=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
    [ -z "$exp" ] && continue
    name=$(basename "$c" .c)
    o="$out/$name.o"; e="$out/$name"
    "$EMBCC" -c -O1 "$c" -o "$o" || { echo "-O1 failed to compile $c"; exit 1; }
    cc -no-pie -o "$e" "$o"      || { echo "host link failed for $c"; exit 1; }
    "$e" >/dev/null 2>&1; got=$?
    [ "$got" -eq "$exp" ] || { echo "-O1 $name: got $got, want $exp"; exit 1; }
    n=$((n + 1))
done
echo "-O1 preserved semantics across all $n exec programs"
echo "optimizer acceptance passed"
