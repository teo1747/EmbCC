#!/bin/sh
# EmbLD emits .embdbg at link time — the producer seam the format needs
# (EMBDBG spec §2/§3). EmbCC emits ET_REL with .text-RELATIVE debug addresses;
# the LINK is what assigns final vaddrs, so EmbLD is where an absolute-addressed,
# image-bound .embdbg is born. It reuses the very same format writer the embdbg
# tool uses (compiled in with -DEMBDBG_NO_MAIN — one implementation).
set -u
echo "TEST-MARKER embld-embdbg"
out=tests/golden/out/embld-embdbg
rm -rf "$out"; mkdir -p "$out"
EMBLD="$(dirname "$EMBCC")/embld"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBLD" ] && [ -x "$EMBDBG" ] || { echo "embld/embdbg not built"; exit 1; }

cat > "$out/p.c" <<'CEOF'
int helper(int x)
{
    int y = x * 2;
    return y;
}
int _start(void)
{
    return helper(21);
}
CEOF
"$EMBCC" -g -c "$out/p.c" -o "$out/p.o" || { echo "embcc -g failed"; exit 1; }
"$EMBLD" "$out/p.o" -o "$out/p.elf" 2>/dev/null || { echo "embld failed"; exit 1; }

fail=0
[ -f "$out/p.elf.embdbg" ] || { echo "embld did not write .embdbg"; exit 1; }

# structural integrity
"$EMBDBG" "$out/p.elf.embdbg" verify | grep -q "verify OK" || { echo "verify failed"; fail=1; }

# build_id is bound to the LINKED image
if command -v sha256sum >/dev/null 2>&1; then
    want=$(sha256sum "$out/p.elf" | cut -d' ' -f1)
    got=$("$EMBDBG" "$out/p.elf.embdbg" verify | awk '/build_id/{print $2}')
    [ "$want" = "$got" ] || { echo "build_id != sha256(p.elf)"; fail=1; }
fi

# addresses are ABSOLUTE now (.text based at 0x400000), not .text-relative.
fns=$("$EMBDBG" "$out/p.elf.embdbg" funcs)
echo "$fns" | grep -qE "helper +0x400000" || { echo "helper not at absolute 0x400000"; fail=1; }
# the bias is exactly the .text vaddr: linked helper == .o helper + 0x400000
orel=$("$EMBDBG" "$out/p.o" funcs | awk '$1=="helper"{print $2}')
[ "$orel" = "0x0" ] || { echo "expected .o helper at 0x0, got $orel"; fail=1; }

# a line-table address symbolizes to the right function:line, absolutely.
la=$("$EMBDBG" "$out/p.elf.embdbg" lines | awk '$2 ~ /p\.c$/ && $3=="3"{print $1; exit}')
if [ -n "$la" ]; then
    case "$la" in 0x4000*) : ;; *) echo "line addr not absolute: $la"; fail=1 ;; esac
    sym=$("$EMBDBG" "$out/p.elf.embdbg" symbolize "$la")
    echo "$sym" | grep -qE "helper.*p\.c:3" || { echo "symbolize wrong: $sym"; fail=1; }
    echo "embld->embdbg: $la -> $(echo "$sym" | sed 's/^[^ ]*  //')"
else
    echo "no line row for p.c:3"; fail=1
fi

[ "$fail" -eq 0 ] && echo "embld-embdbg: link-time .embdbg is absolute + image-bound" || exit 1
echo "embld-embdbg golden passed (EmbLD wrote the absolute-addressed .embdbg)"
