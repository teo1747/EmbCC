#!/bin/sh
# EmbDBG disassembler (spec §4.8): a native x86-64 decoder over .text, scoped
# to EmbCC's codegen repertoire, with mixed source+asm from the line table.
# The load-bearing property is correct instruction LENGTHS — a wrong length
# desyncs everything after it — so this cross-checks embdbg's instruction
# boundaries against objdump's, byte for byte, and requires the disassembly to
# name the obvious instructions.
set -u
echo "TEST-MARKER embdbg-disasm"
out=tests/golden/out/embdbg-disasm
rm -rf "$out"; mkdir -p "$out"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built"; exit 1; }
command -v objdump >/dev/null 2>&1 || { echo "objdump not present"; exit 1; }

cat > "$out/d.c" <<'CEOF'
int classify(int a, int b)
{
    int r = 0;
    if (a > b) r = a - b; else r = b - a;
    for (int i = 0; i < b; i++) r += i & 3;
    return r;
}
CEOF
"$EMBCC" -g -c "$out/d.c" -o "$out/d.o" || { echo "embcc -g failed"; exit 1; }

fail=0
dis=$("$EMBDBG" "$out/d.o" disassemble classify)

# names the obvious instructions
echo "$dis" | grep -q "push   %rbp"       || { echo "no prologue push"; fail=1; }
echo "$dis" | grep -q "mov    %rsp,%rbp"  || { echo "no frame setup"; fail=1; }
echo "$dis" | grep -qE "ret$"             || { echo "no ret"; fail=1; }
echo "$dis" | grep -qE "j(l|ge|e|ne|mp)"  || { echo "no control flow"; fail=1; }
# mixed source+asm: a source-line marker for the function's body
echo "$dis" | grep -qE "d\.c:3"           || { echo "no mixed source marker"; fail=1; }

# THE decoder-correctness check: instruction boundaries identical to objdump,
# within the symbol's size (objdump appends inter-function padding + wraps the
# bytes of long instructions onto mnemonic-less continuation lines; exclude
# both — real instruction lines have a mnemonic, padding sits past the size).
val=$(readelf -sW "$out/d.o" | awk '$8=="classify"{print $2}')
dsz=$(readelf -sW "$out/d.o" | awk '$8=="classify"{print $3}')      # DECIMAL
end=$(( 0x$val + dsz ))
realadr='s/^ *\([0-9a-f]*\):\t[0-9a-f ]*\t[a-z].*/\1/p'
echo "$dis" | sed -n "$realadr" > "$out/e.txt"
objdump -d "$out/d.o" | awk '/<classify>:/{g=1;next} g&&/^$/{exit} g{print}' | sed -n "$realadr" \
    | while read a; do [ $((0x$a)) -lt $end ] && echo "$a"; done > "$out/o.txt"
if diff -q "$out/e.txt" "$out/o.txt" >/dev/null; then
    echo "boundaries match objdump ($(wc -l < "$out/e.txt") instructions)"
else
    echo "BOUNDARY MISMATCH vs objdump:"; diff "$out/e.txt" "$out/o.txt" | head; fail=1
fi

[ "$fail" -eq 0 ] && echo "embdbg-disasm: decode correct, boundaries == objdump, mixed source" || exit 1
echo "embdbg-disasm golden passed (native x86-64 disassembler)"
