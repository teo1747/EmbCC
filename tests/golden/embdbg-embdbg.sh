#!/bin/sh
# The native .embdbg format (myos EMBDBG_Specification v1) — the owned debug
# form (D-010: DWARF is the bridge, .embdbg the destination, derived from
# EmbDBG's real needs). The honest proof is a ROUND TRIP against the real
# consumer: embdbg writes .embdbg from the DWARF it parsed, reads it back, and
# every command produces byte-identical output either way. Plus the spec's
# integrity guarantees: a SHA-256 build_id bound to the image, CRC32C on the
# header and every section, and byte-for-byte determinism.
set -u
echo "TEST-MARKER embdbg-embdbg"
out=tests/golden/out/embdbg-embdbg
rm -rf "$out"; mkdir -p "$out"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBDBG" ] || { echo "embdbg not built"; exit 1; }

cat > "$out/e.c" <<'CEOF'
int compute(int a, int b)
{
    int sum = a + b;
    char c = 'X';
    int *p = &sum;
    return sum + c + *p;
}
int main(void)
{
    return compute(3, 4);
}
CEOF
"$EMBCC" -g -c "$out/e.c" -o "$out/e.o" || { echo "embcc -g failed"; exit 1; }
fail=0

# emit + magic
"$EMBDBG" "$out/e.o" emit "$out/e.embdbg" || { echo "emit failed"; exit 1; }
magic=$(od -An -tx1 -N8 "$out/e.embdbg" | tr -d ' \n')
[ "$magic" = "7f454d4442470a1a" ] || { echo "bad magic: $magic"; fail=1; }

# verify: header + every section CRC32C
"$EMBDBG" "$out/e.embdbg" verify | grep -q "verify OK" || { echo "verify failed"; fail=1; }

# build_id == sha256 of the described binary
if command -v sha256sum >/dev/null 2>&1; then
    want=$(sha256sum "$out/e.o" | cut -d' ' -f1)
    got=$("$EMBDBG" "$out/e.embdbg" verify | awk '/build_id/{print $2}')
    [ "$want" = "$got" ] || { echo "build_id != sha256(e.o): $got vs $want"; fail=1; }
fi

# determinism
"$EMBDBG" "$out/e.o" emit "$out/e2.embdbg"
cmp -s "$out/e.embdbg" "$out/e2.embdbg" || { echo "non-deterministic .embdbg"; fail=1; }

# THE ROUND TRIP: identical answers from DWARF (.o) and native (.embdbg).
for cmd in "funcs" "symbolize 0x13 0x32 0x5b" "info compute" "where 0x32"; do
    a=$("$EMBDBG" "$out/e.o" $cmd)
    b=$("$EMBDBG" "$out/e.embdbg" $cmd)
    [ "$a" = "$b" ] || { echo "ROUND-TRIP DIFFERS for '$cmd'"; fail=1; }
done

[ "$fail" -eq 0 ] && echo "embdbg-embdbg: emit/verify/round-trip all correct" || exit 1
echo "embdbg-embdbg golden passed (.o DWARF and native .embdbg agree byte for byte)"
