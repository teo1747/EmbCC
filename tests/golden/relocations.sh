#!/bin/sh
# The relocation contract, checked with readelf (DECISIONS D-005: prove
# on the host): external calls are R_X86_64_PLT32 against GLOBAL UNDEF
# symbols with addend -4 — the exact shape the cross ld and the
# EmbLinkOS linker (TARGET_ABI §4a) both resolve in a static link.
# A declared-but-unused external must leave NO trace.
set -u
echo "TEST-MARKER relocations"

out_dir="tests/golden/out"
mkdir -p "$out_dir"

obj="$out_dir/relocs.o"
rm -f "$obj"
"$EMBCC" -c tests/exec/extern-call.c -o "$obj" || { echo "compile failed"; exit 1; }

rel=$(readelf -r "$obj") || { echo "readelf -r rejected the object"; exit 1; }
n=$(echo "$rel" | grep -c "R_X86_64_PLT32.*putchar - 4")
[ "$n" -eq 2 ] || {
    echo "expected 2 PLT32 relocations against putchar, found $n:"
    echo "$rel"; exit 1; }

readelf -s "$obj" | grep -q "NOTYPE  GLOBAL DEFAULT  UND putchar" || {
    echo "putchar is not a GLOBAL UNDEF symbol:"; readelf -s "$obj"; exit 1; }

# The cross ld must accept and resolve the shape too (symbol closure
# is the linker's judgment, not ours).
if [ -x /usr/local/cross/bin/x86_64-elf-ld ]; then
    /usr/local/cross/bin/x86_64-elf-ld -r -o "$out_dir/relocs-combined.o" \
        "$obj" || { echo "cross ld rejected the object"; exit 1; }
    echo "cross ld accepts the relocatable"
fi

# Unused external: no symbol, no relocation.
obj2="$out_dir/unused-extern.o"
rm -f "$obj2"
"$EMBCC" -c tests/exec/proto-order.c -o "$obj2" || { echo "compile failed"; exit 1; }
if readelf -s "$obj2" | grep -q "getchar"; then
    echo "unused external 'getchar' leaked into the symbol table"
    exit 1
fi
echo "relocations, UNDEF symbols, and unused-extern hygiene all check out"
