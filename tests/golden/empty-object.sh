#!/bin/sh
# The ELF writer skeleton (ROADMAP M0). This proves the writer, not the
# compiler (CONTRIBUTING is explicit about that distinction) — but it proves
# it with the same tools the real objects will face: readelf and objdump
# must both accept the file, per "prove on the host" (DECISIONS D-005).
set -u
echo "TEST-MARKER empty-object"

out_dir="tests/golden/out"
mkdir -p "$out_dir"
obj="$out_dir/empty.o"
rm -f "$obj"

"$EMBCC" --emit-empty-object "$obj" || { echo "emit exited nonzero"; exit 1; }
[ -s "$obj" ] || { echo "no object written"; exit 1; }

h=$(readelf -h "$obj") || { echo "readelf rejected the header"; exit 1; }
echo "$h" | grep -q "Class: *ELF64"                  || { echo "not ELF64"; exit 1; }
echo "$h" | grep -q "little endian"                  || { echo "not LSB"; exit 1; }
echo "$h" | grep -q "REL (Relocatable file)"         || { echo "not ET_REL"; exit 1; }
echo "$h" | grep -q "X86-64"                         || { echo "not EM_X86_64"; exit 1; }

s=$(readelf -S "$obj") || { echo "readelf rejected the section table"; exit 1; }
for sec in .text .symtab .strtab .shstrtab; do
    echo "$s" | grep -q "$sec" || { echo "missing $sec"; exit 1; }
done

readelf -s "$obj" | grep -q "FILE" || { echo "symbol table lost the FILE entry"; exit 1; }
objdump -d "$obj" >/dev/null || { echo "objdump rejected the object"; exit 1; }
echo "readelf and objdump both accept the object"
