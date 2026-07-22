#!/bin/sh
# The data-section contract, checked with readelf: initialized globals
# are OBJECT symbols in .data, zero ones in .bss (NOBITS), static ones
# LOCAL, sizes real, and every global access relocates PC32 against the
# global's OWN symbol (so other objects' references resolve the same
# way ld resolves gcc's).
set -u
echo "TEST-MARKER data-symbols"

out_dir="tests/golden/out"
mkdir -p "$out_dir"
obj="$out_dir/globals.o"
rm -f "$obj"
"$EMBCC" -c tests/exec/globals.c -o "$obj" || { echo "compile failed"; exit 1; }

s=$(readelf -sSW "$obj") || { echo "readelf rejected the object"; exit 1; }
echo "$s" | grep -q "\.data.*PROGBITS" || { echo "no .data section"; exit 1; }
echo "$s" | grep -q "\.bss.*NOBITS"    || { echo ".bss is not NOBITS"; exit 1; }
echo "$s" | grep -qE "4 OBJECT  GLOBAL DEFAULT +[0-9]+ counter" || {
    echo "counter is not a 4-byte GLOBAL OBJECT:"; echo "$s"; exit 1; }
echo "$s" | grep -qE "1 OBJECT  LOCAL  DEFAULT +[0-9]+ tag" || {
    echo "static tag is not a LOCAL OBJECT:"; echo "$s"; exit 1; }
echo "$s" | grep -qE "32 OBJECT  GLOBAL DEFAULT +[0-9]+ hits" || {
    echo "hits[8] has the wrong size:"; echo "$s"; exit 1; }

readelf -r "$obj" | grep -q "R_X86_64_PC32.*counter" || {
    echo "no PC32 relocation against counter"; exit 1; }
readelf -r "$obj" | grep -q "R_X86_64_PC32.*big" || {
    echo "no PC32 relocation against big"; exit 1; }

if [ -x /usr/local/cross/bin/x86_64-elf-ld ]; then
    /usr/local/cross/bin/x86_64-elf-ld -r -o "$out_dir/globals-combined.o" \
        "$obj" || { echo "cross ld rejected the object"; exit 1; }
    echo "cross ld accepts the relocatable"
fi
echo "data symbols, sections, and relocations all check out"
