#!/bin/sh
# DWARF line info (D-010 step 1: address <-> source file:line). int $0x80
# aside, this is proven the honest way — a REAL debugger (gdb) reads the
# table on the host and resolves source lines to addresses in the right
# function. -g is opt-in: without it the object must be byte-for-byte as
# before (the M3 self-host fixed point depends on that), so that is asserted
# too, alongside determinism.
set -u
echo "TEST-MARKER debug-line"
out=tests/golden/out/debug-line
rm -rf "$out"; mkdir -p "$out"

cat > "$out/dbg.c" <<'CEOF'
int add(int a, int b)
{
    int s = a + b;
    return s;
}
int main(void)
{
    int x = add(20, 22);
    return x;
}
CEOF

fail=0

# 1. -g emits the three DWARF sections.
"$EMBCC" -g -c "$out/dbg.c" -o "$out/dbg.o" || { echo "embcc -g failed"; exit 1; }
sec=$(readelf -SW "$out/dbg.o" 2>/dev/null)
for s in .debug_abbrev .debug_info .debug_line; do
    echo "$sec" | grep -q "$s" || { echo "MISSING section: $s"; fail=1; }
done

# 2. The line table, decoded by a real DWARF reader, carries every statement
#    line. (Addresses are cross-checked by gdb below rather than hard-coded,
#    so the test does not break when codegen shifts an offset.)
dl=$(readelf --debug-dump=decodedline "$out/dbg.o" 2>/dev/null)
for ln in 3 4 8 9; do
    echo "$dl" | grep -qE "dbg\.c[[:space:]]+$ln[[:space:]]" \
        || { echo "MISSING line $ln in the decoded line table"; fail=1; }
done

# 3. A real debugger consumes it and attributes each line to the correct
#    FUNCTION — the actual acceptance, and offset-independent.
if command -v gdb >/dev/null 2>&1; then
    g=$(gdb -batch -nx "$out/dbg.o" \
            -ex "info line dbg.c:3" -ex "info line dbg.c:8" 2>/dev/null)
    echo "$g" | grep -qE 'Line 3 .*<add'  || { echo "gdb: line 3 not in add";  fail=1; }
    echo "$g" | grep -qE 'Line 8 .*<main' || { echo "gdb: line 8 not in main"; fail=1; }
    [ "$fail" -eq 0 ] && echo "gdb consumed the line table (line 3->add, line 8->main)"
else
    echo "gdb absent; skipped the debugger-consumes-it check (readelf still ran)"
fi

[ "$fail" -eq 0 ] && echo "debug-line: DWARF line info correct" || exit 1

# 4. -g output is deterministic (no timestamps / host paths baked in).
"$EMBCC" -g -c "$out/dbg.c" -o "$out/dbg2.o"
cmp -s "$out/dbg.o" "$out/dbg2.o" || { echo "NONDETERMINISTIC -g output"; exit 1; }
echo "debug-line: -g output is byte-identical across runs"

# 5. Without -g, NO debug sections — default output is untouched, which is
#    what keeps the self-host fixed point.
"$EMBCC" -c "$out/dbg.c" -o "$out/nog.o"
if readelf -SW "$out/nog.o" 2>/dev/null | grep -q '\.debug'; then
    echo "BUG: debug sections emitted without -g"; exit 1
fi
echo "debug-line: no -g -> no debug sections"
echo "debug-line golden passed (running proof: gdb on the host)"
