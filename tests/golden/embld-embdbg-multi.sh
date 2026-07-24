#!/bin/sh
# Multi-CU merge: several -g objects linked together, all their debug info
# merged into ONE .embdbg. Each object is biased by its own final .text vaddr
# (so addresses are absolute), and the models are merged without collision —
# per-object type-offset bands and remapped file indices — so a function from
# each object appears with ITS file's source and ITS own types.
set -u
echo "TEST-MARKER embld-embdbg-multi"
out=tests/golden/out/embld-embdbg-multi
rm -rf "$out"; mkdir -p "$out"
EMBLD="$(dirname "$EMBCC")/embld"
EMBDBG="$(dirname "$EMBCC")/embdbg"
[ -x "$EMBLD" ] && [ -x "$EMBDBG" ] || { echo "embld/embdbg not built"; exit 1; }

cat > "$out/a.c" <<'CEOF'
int add(int x, int y)
{
    int s = x + y;
    return s;
}
CEOF
cat > "$out/b.c" <<'CEOF'
extern int add(int, int);
int _start(void)
{
    int r = add(20, 22);
    char *msg = "hi";
    return r + msg[0];
}
CEOF
"$EMBCC" -g -c "$out/a.c" -o "$out/a.o" || { echo "embcc a failed"; exit 1; }
"$EMBCC" -g -c "$out/b.c" -o "$out/b.o" || { echo "embcc b failed"; exit 1; }
"$EMBLD" "$out/a.o" "$out/b.o" -o "$out/m.elf" 2>/dev/null || { echo "embld failed"; exit 1; }

fail=0
[ -f "$out/m.elf.embdbg" ] || { echo "no merged .embdbg"; exit 1; }
"$EMBDBG" "$out/m.elf.embdbg" verify | grep -q "verify OK" || { echo "verify failed"; fail=1; }

# both functions, from different objects, at absolute addresses
fns=$("$EMBDBG" "$out/m.elf.embdbg" funcs)
echo "$fns" | grep -qE "add +0x40" || { echo "add missing/relative"; fail=1; }
echo "$fns" | grep -qE "_start +0x40" || { echo "_start missing/relative"; fail=1; }

# a.o's function: a.c source + its local (int s)
la=$("$EMBDBG" "$out/m.elf.embdbg" lines | awk '$2 ~ /a\.c$/ && $3=="3"{print $1; exit}')
wa=$("$EMBDBG" "$out/m.elf.embdbg" where "$la")
echo "$wa" | grep -qE "add.*a\.c:3"          || { echo "add not symbolized to a.c"; fail=1; }
echo "$wa" | grep -qE "local +int +s +@ rbp" || { echo "add's local s missing"; fail=1; }

# b.o's function: b.c source + a DIFFERENT type (char *) — proves the type
# graphs from the two objects merged without collision.
lb=$("$EMBDBG" "$out/m.elf.embdbg" lines | awk '$2 ~ /b\.c$/ && $3=="4"{print $1; exit}')
wb=$("$EMBDBG" "$out/m.elf.embdbg" where "$lb")
echo "$wb" | grep -qE "_start.*b\.c:4"          || { echo "_start not symbolized to b.c"; fail=1; }
echo "$wb" | grep -qE "local +char \* +msg +@ rbp" || { echo "_start's char* msg missing"; fail=1; }

# build_id bound to the linked image
if command -v sha256sum >/dev/null 2>&1; then
    want=$(sha256sum "$out/m.elf" | cut -d' ' -f1)
    got=$("$EMBDBG" "$out/m.elf.embdbg" verify | awk '/build_id/{print $2}')
    [ "$want" = "$got" ] || { echo "build_id != sha256(m.elf)"; fail=1; }
fi

[ "$fail" -eq 0 ] && echo "embld-embdbg-multi: two -g objects merged into one .embdbg" || exit 1
echo "embld-embdbg-multi golden passed (multi-CU merge: add from a.c, _start from b.c)"
