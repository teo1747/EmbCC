#!/bin/sh
# -O2 register allocation differential: every exec/ program is compiled at -O2
# (which turns on allocation of vregs to callee-saved registers) and must exit
# with — and print — exactly what the host gcc does. A register-allocation bug
# (a value's register clobbered while still live, a wrong extension, a callee
# reg not preserved across a call) shows up here as a divergence from gcc. -O0
# and -O1 are unaffected by -O2 codegen, so this is purely the allocator's net.
set -u
echo "TEST-MARKER regalloc-O2"

EMBCC=${EMBCC:-./embcc}
out_dir="tests/golden/out/regalloc-O2"
rm -rf "$out_dir"; mkdir -p "$out_dir"

n=0
for c in tests/exec/*.c; do
    [ -e "$c" ] || continue
    name=$(basename "$c" .c)
    "$EMBCC" -O2 -c "$c" -o "$out_dir/$name.o" || {
        echo "$name: embcc -O2 failed to compile"; exit 1; }
    cc -no-pie -o "$out_dir/$name.embcc" "$out_dir/$name.o" || {
        echo "$name: link of -O2 object failed"; exit 1; }
    cc -std=c99 -no-pie -o "$out_dir/$name.gcc" "$c" || {
        echo "$name: not valid C99 (needed for the gcc reference)"; exit 1; }
    out_a=$("$out_dir/$name.embcc"); a=$?
    out_b=$("$out_dir/$name.gcc"); b=$?
    if [ "$a" -ne "$b" ]; then
        echo "$name: embcc -O2 exits $a, gcc exits $b — register-alloc miscompile"
        exit 1
    fi
    if [ "$out_a" != "$out_b" ]; then
        echo "$name: stdout differs at -O2:"
        printf 'embcc: %s\ngcc:   %s\n' "$out_a" "$out_b"
        exit 1
    fi
    n=$((n + 1))
done

echo "all $n exec programs agree with gcc when built -O2 (register allocation)"
echo "regalloc-O2 acceptance passed"
