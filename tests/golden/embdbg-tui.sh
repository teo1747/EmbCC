#!/bin/sh
# EmbDBG's TUI — an interactive browser (function list + a detail pane with
# source and typed locals). Interactive mode needs a real terminal, so it
# can't run under a test harness; but the command auto-falls-back to a plain
# full dump when stdout is not a tty, which IS testable and keeps the tool
# scriptable. (The interactive path is smoke-tested under a pty separately.)
set -u
echo "TEST-MARKER embdbg-tui"
out=tests/golden/out/embdbg-tui
rm -rf "$out"; mkdir -p "$out"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built"; exit 1; }

cat > "$out/t.c" <<'CEOF'
int twice(int n)
{
    int r = n + n;
    return r;
}
int main(void)
{
    return twice(21);
}
CEOF
"$EMBCC" -g -c "$out/t.c" -o "$out/t.o" || { echo "embcc -g failed"; exit 1; }

fail=0
# piping makes stdout a non-tty -> plain dump
dump=$("$EMBDBG" "$out/t.o" tui)

echo "$dump" | grep -q "== twice ==" || { echo "TUI dump missing twice"; fail=1; }
echo "$dump" | grep -q "== main ==" || { echo "TUI dump missing main"; fail=1; }
# the detail pane content: source line text and a typed local
echo "$dump" | grep -qE "int r = n \+ n" || { echo "TUI missing source line"; fail=1; }
echo "$dump" | grep -qE "param +int +n +@ rbp" || { echo "TUI missing typed param"; fail=1; }
echo "$dump" | grep -qE "local +int +r +@ rbp" || { echo "TUI missing typed local"; fail=1; }

[ "$fail" -eq 0 ] && echo "embdbg-tui: browser dump lists functions with source + typed locals" || exit 1
echo "embdbg-tui golden passed (interactive browser; plain dump when piped)"
