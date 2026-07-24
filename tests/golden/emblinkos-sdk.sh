#!/bin/sh
# M2's acceptance target, compiled by EmbCC: the real EmbLinkOS sval SDK
# (value.c + wire.c + sval.c, ~970 lines) and tally.c, from the OS tree
# and unmodified.
#
# Compiling is not the claim — RUNNING is. A gcc-built harness drives the
# EmbCC-compiled objects through a full round trip: build a table of
# records holding ints, strings, filesizes and doubles, serialize it
# through the varint/zigzag/little-endian-double codec, deserialize, and
# check the values survived. Then the SAME harness is built against
# gcc-compiled objects and the two outputs are compared, so "it works"
# means "it does what gcc's build does", not "it did not crash".
#
# Skips honestly when the OS tree is not on this machine.
set -u
echo "TEST-MARKER emblinkos-sdk"

OS=/home/motsou/myos
NEWLIB=/home/motsou/cross/newlib-c99/x86_64-elf/include
[ -d "$OS/shell/sval" ] || { echo "skipped: the EmbLinkOS tree is not here"; exit 0; }
[ -d "$NEWLIB" ] || NEWLIB=/usr/local/cross/x86_64-elf/include
[ -d "$NEWLIB" ] || { echo "skipped: no newlib headers"; exit 0; }

out=tests/golden/out/sdk
rm -rf "$out"; mkdir -p "$out"
INC="-I$OS/shell -I$OS/user/lib -Iinclude -I$NEWLIB"

for u in value/value wire/wire sval/sval; do
    n=$(basename "$u")
    "$EMBCC" -c "$OS/shell/$u.c" -o "$out/$n.o" $INC || {
        echo "embcc failed on $n.c"; exit 1; }
done
"$EMBCC" -c "$OS/shell/tools/tally.c" -o "$out/tally.o" $INC || {
    echo "embcc failed on tally.c"; exit 1; }
echo "compiled: value.c wire.c sval.c tally.c"

# tally must LINK into a complete static program for the OS, exactly as
# the OS's own Makefile links it (crt0 + syscalls + newlib).
if [ -f "$OS/build/crt0.o" ] && [ -f "$OS/build/syscalls.o" ] && \
   command -v x86_64-elf-gcc >/dev/null 2>&1; then
    x86_64-elf-gcc -nostartfiles -static -T "$OS/user/lib/newlib.ld" \
        -L/home/motsou/cross/newlib-c99/x86_64-elf/lib \
        "$OS/build/crt0.o" "$OS/build/syscalls.o" \
        "$out/tally.o" "$out/sval.o" "$out/value.o" "$out/wire.o" \
        -lc -lgcc -o "$out/tally.elf" 2>"$out/link.err" || {
        echo "tally.elf failed to link:"; sed 's/^/    /' "$out/link.err"
        exit 1; }
    readelf -h "$out/tally.elf" | grep -q "EXEC" || {
        echo "tally.elf is not an executable"; exit 1; }
    echo "linked: tally.elf for EmbLinkOS ($(stat -c%s "$out/tally.elf") bytes)"
fi

# The functional half: run the SDK, and demand it match gcc's build.
cc -std=c99 -I"$OS/shell" -c tests/golden/sdk/harness.c -o "$out/h.o" || {
    echo "harness failed to build"; exit 1; }
cc -no-pie -o "$out/emb" "$out/h.o" "$out/value.o" "$out/wire.o" || {
    echo "harness failed to link against embcc objects"; exit 1; }
emb_out=$("$out/emb"); emb_rc=$?

cc -std=c99 -I"$OS/shell" -c "$OS/shell/value/value.c" -o "$out/gv.o"
cc -std=c99 -I"$OS/shell" -c "$OS/shell/wire/wire.c" -o "$out/gw.o"
cc -no-pie -o "$out/gcc" "$out/h.o" "$out/gv.o" "$out/gw.o"
gcc_out=$("$out/gcc"); gcc_rc=$?

if [ "$emb_rc" -ne "$gcc_rc" ] || [ "$emb_out" != "$gcc_out" ]; then
    echo "EmbCC's build behaves differently from gcc's:"
    printf 'embcc (%d):\n%s\ngcc   (%d):\n%s\n' \
        "$emb_rc" "$emb_out" "$gcc_rc" "$gcc_out"
    exit 1
fi
[ "$emb_rc" -eq 42 ] || { echo "round trip failed (exit $emb_rc)"; exit 1; }
echo "value/wire round trip: EmbCC's build matches gcc's exactly"
