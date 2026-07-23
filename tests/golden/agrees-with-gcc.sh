#!/bin/sh
# The golden cross-check (tests/README): every exec/ program is also
# built with the host gcc, and both binaries must exit with the same
# code. This is what catches EmbCC accepting a different language than C
# or computing a different answer than the reference compiler.
set -u
echo "TEST-MARKER agrees-with-gcc"

out_dir="tests/golden/out"
mkdir -p "$out_dir"

for c in tests/exec/*.c; do
    name=$(basename "$c" .c)
    rm -f "$out_dir/$name.embcc" "$out_dir/$name.gcc" \
          "$out_dir/$name.embcc.o"
    "$EMBCC" -c "$c" -o "$out_dir/$name.embcc.o" || {
        echo "$name: embcc failed"; exit 1; }
    cc -no-pie -o "$out_dir/$name.embcc" "$out_dir/$name.embcc.o" || {
        echo "$name: link of embcc object failed"; exit 1; }
    cc -std=c99 -no-pie -o "$out_dir/$name.gcc" "$c" || {
        echo "$name: not valid C99 — the M1 subset must stay a strict"
        echo "subset of C, or golden comparisons are impossible"
        exit 1; }
    out_a=$("$out_dir/$name.embcc"); a=$?
    out_b=$("$out_dir/$name.gcc"); b=$?
    if [ "$a" -ne "$b" ]; then
        echo "$name: embcc exits $a, gcc exits $b"
        exit 1
    fi
    if [ "$out_a" != "$out_b" ]; then
        echo "$name: stdout differs between embcc and gcc builds:"
        printf 'embcc: %s\ngcc:   %s\n' "$out_a" "$out_b"
        exit 1
    fi
    echo "$name: both exit $a, same output"
done
