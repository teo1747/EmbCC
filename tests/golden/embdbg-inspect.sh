#!/bin/sh
# EmbDBG reads .debug_info/.debug_abbrev too, so it answers "what can I see?" —
# the params and locals in scope, each with its type and frame slot — not just
# "where am I?". Still kernel-free: it is all in the debug info EmbCC emits.
set -u
echo "TEST-MARKER embdbg-inspect"
out=tests/golden/out/embdbg-inspect
rm -rf "$out"; mkdir -p "$out"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built (make embdbg)"; exit 1; }

cat > "$out/w.c" <<'CEOF'
int compute(int a, int b)
{
    int sum = a + b;
    char c = 'X';
    int *p = &sum;
    return sum + c + *p;
}
CEOF
"$EMBCC" -g -c "$out/w.c" -o "$out/w.o" || { echo "embcc -g failed"; exit 1; }

fail=0

# info FUNC: params a,b (int) and locals sum(int), c(char), p(int *), each
# with a frame slot.
inf=$("$EMBDBG" "$out/w.o" info compute)
echo "$inf" | grep -qE "param +int +a +@ rbp" || { echo "missing param a:int"; fail=1; }
echo "$inf" | grep -qE "local +char +c +@ rbp" || { echo "missing local c:char"; fail=1; }
echo "$inf" | grep -qE "local +int \* +p +@ rbp" || { echo "missing local p:int*"; fail=1; }

# where ADDR: symbolizes AND lists the in-scope variables.
la=$("$EMBDBG" "$out/w.o" lines | awk '$2 ~ /w\.c$/ && $3=="3"{print $1; exit}')
if [ -n "$la" ]; then
    whr=$("$EMBDBG" "$out/w.o" where "$la")
    echo "$whr" | grep -qE "compute.*w\.c:3" || { echo "where didn't symbolize: $whr"; fail=1; }
    echo "$whr" | grep -qE "sum +@ rbp"      || { echo "where didn't list locals"; fail=1; }
    echo "embdbg where $la:"; echo "$whr" | sed 's/^/  /'
else
    echo "no line row for w.c:3"; fail=1
fi

[ "$fail" -eq 0 ] && echo "embdbg-inspect: where/info list locals with types + slots" || exit 1
echo "embdbg-inspect golden passed (locals+types read from .debug_info, no gdb)"
