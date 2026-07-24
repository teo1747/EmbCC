#!/bin/sh
# EmbDBG crash analyzer (spec §5): turn a kernel fault dump into a diagnosis,
# fully offline. It reads a simple crash report (exception, faulting address,
# registers, and the stack words along the rbp chain) plus the binary's debug
# info, and produces: the exception, RIP symbolized to func+file:line, a
# register dump, a SYMBOLIZED backtrace (walking the rbp chain), the locals in
# scope at the crash, and the instructions around RIP with the fault marked.
set -u
echo "TEST-MARKER embdbg-crash"
out=tests/golden/out/embdbg-crash
rm -rf "$out"; mkdir -p "$out"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built"; exit 1; }

cat > "$out/cr.c" <<'CEOF'
int deref(int *p)
{
    int v = *p;
    return v;
}
int main(void)
{
    int *bad = 0;
    return deref(bad);
}
CEOF
"$EMBCC" -g -c "$out/cr.c" -o "$out/cr.o" || { echo "embcc -g failed"; exit 1; }

# Derive the real addresses (robust to codegen changes):
#  - rip  = the NULL-deref instruction `mov (%rax),%eax` in deref
#  - ret  = the return address into main = (addr of `call`) + 5
addr() { sed -n "s/^ *\([0-9a-f]*\):\t.*/\1/p"; }
rip=$("$EMBDBG" "$out/cr.o" disassemble deref | awk -F'\t' '/mov +\(%rax\)/{print $1; exit}' | tr -dc '0-9a-f')
callp=$("$EMBDBG" "$out/cr.o" disassemble main | awk -F'\t' '/call/{print $1; exit}' | tr -dc '0-9a-f')
[ -n "$rip" ] && [ -n "$callp" ] || { echo "could not derive addresses (rip=$rip call=$callp)"; exit 1; }
ret=$(printf '0x%x' $(( 0x$callp + 5 )))

cat > "$out/report.txt" <<EOF
# NULL pointer dereference
exception PAGE_FAULT
fault 0x0
reg rip 0x$rip
reg rsp 0x7000
reg rbp 0x7020
reg rax 0x0
mem 0x7020 0x7040
mem 0x7028 $ret
mem 0x7040 0x0
mem 0x7048 0x0
EOF

rep=$("$EMBDBG" "$out/cr.o" crash "$out/report.txt")
echo "$rep"
echo "--- checks ---"
fail=0
echo "$rep" | grep -q "PAGE_FAULT"                    || { echo "no exception"; fail=1; }
echo "$rep" | grep -q "faulting address 0x0"          || { echo "no fault addr"; fail=1; }
echo "$rep" | grep -qE "RIP:.*deref.*cr\.c:3"         || { echo "RIP not symbolized to deref/cr.c:3"; fail=1; }
echo "$rep" | grep -qE "#0 .*deref.*cr\.c:3"          || { echo "frame #0 wrong"; fail=1; }
echo "$rep" | grep -qE "#1 .*main.*cr\.c:9"           || { echo "frame #1 (main) missing — unwind failed"; fail=1; }
echo "$rep" | grep -qE "param +int \* +p"             || { echo "crash-frame locals missing"; fail=1; }
echo "$rep" | grep -qE "\->.*mov +\(%rax\)"           || { echo "faulting instruction not marked"; fail=1; }

[ "$fail" -eq 0 ] && echo "embdbg-crash: exception+regs+symbolized backtrace+locals+faulting insn" || exit 1
echo "embdbg-crash golden passed (offline fault-dump analysis)"
