#!/bin/sh
# The symbol table of a real object must be sane (ROADMAP M1's host-side
# bar: "readelf/objdump agree the object is well-formed and the symbol
# table is sane"): static -> LOCAL, extern -> GLOBAL, sizes nonzero,
# and the intra-unit call actually lands on the callee.
set -u
echo "TEST-MARKER symtab"

out_dir="tests/golden/out"
mkdir -p "$out_dir"
obj="$out_dir/symtab.o"
rm -f "$obj"

"$EMBCC" -c tests/exec/exit42.c -o "$obj" || { echo "compile failed"; exit 1; }

syms=$(readelf -s "$obj") || { echo "readelf rejected the object"; exit 1; }
echo "$syms" | grep -q "FUNC    LOCAL  DEFAULT.*twice" || {
    echo "static function 'twice' is not a LOCAL FUNC:"; echo "$syms"; exit 1; }
echo "$syms" | grep -q "FUNC    GLOBAL DEFAULT.*main" || {
    echo "'main' is not a GLOBAL FUNC:"; echo "$syms"; exit 1; }
echo "$syms" | awk '$4 == "FUNC" && $3 == 0 { bad = 1 } END { exit bad }' || {
    echo "a FUNC symbol has size 0:"; echo "$syms"; exit 1; }

dis=$(objdump -d "$obj") || { echo "objdump rejected the object"; exit 1; }
echo "$dis" | grep -q "call.*<twice>" || {
    echo "call in main does not resolve to twice:"; echo "$dis"; exit 1; }

echo "symbol table and disassembly are sane"
