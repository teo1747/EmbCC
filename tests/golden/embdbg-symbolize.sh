#!/bin/sh
# EmbDBG v0 reads the debug info EmbCC emits — the whole toolchain is ours,
# no gdb. EmbCC -g writes DWARF; embdbg parses .symtab + .debug_line and
# turns a raw address (the kind a kernel fault handler prints) into
# FUNCTION+offset  file:line. This is the "where am I?" half of a debugger,
# which needs no live process or kernel support.
set -u
echo "TEST-MARKER embdbg-symbolize"
out=tests/golden/out/embdbg-symbolize
rm -rf "$out"; mkdir -p "$out"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built (make embdbg)"; exit 1; }

cat > "$out/s.c" <<'CEOF'
int helper(int x)
{
    return x * 2;
}
int main(void)
{
    return helper(21);
}
CEOF
"$EMBCC" -g -c "$out/s.c" -o "$out/s.o" || { echo "embcc -g failed"; exit 1; }

fail=0

# funcs: both functions, from .symtab
f=$("$EMBDBG" "$out/s.o" funcs)
echo "$f" | grep -q "helper" || { echo "funcs missing helper"; fail=1; }
echo "$f" | grep -q "main"   || { echo "funcs missing main";   fail=1; }

# symbolize: helper's line-3 address resolves back to helper + s.c:3.
la=$("$EMBDBG" "$out/s.o" lines | awk '$2 ~ /s\.c$/ && $3=="3"{print $1; exit}')
[ -n "$la" ] || { echo "no line row for s.c:3"; fail=1; }
if [ -n "$la" ]; then
    sym=$("$EMBDBG" "$out/s.o" symbolize "$la")
    echo "$sym" | grep -qE "helper.*s\.c:3" || { echo "symbolize wrong: $sym"; fail=1; }
    echo "embdbg: $la -> $(echo "$sym" | sed 's/^[^ ]*  //')"
fi

# main's line-7 address resolves to main (a different function) + s.c:7.
ma=$("$EMBDBG" "$out/s.o" lines | awk '$2 ~ /s\.c$/ && $3=="7"{print $1; exit}')
if [ -n "$ma" ]; then
    sym=$("$EMBDBG" "$out/s.o" symbolize "$ma")
    echo "$sym" | grep -qE "main.*s\.c:7" || { echo "symbolize main wrong: $sym"; fail=1; }
fi

[ "$fail" -eq 0 ] && echo "embdbg-symbolize: addr -> func:file:line correct" || exit 1
echo "embdbg-symbolize golden passed (EmbCC emits DWARF, EmbDBG reads it — no gdb)"
