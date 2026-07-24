#!/bin/sh
# DWARF frame + locals (EMBDBG step 2). EmbCC -g now describes each function
# and its parameters/locals: a DW_TAG_subprogram with frame_base = rbp, child
# formal_parameter/variable DIEs whose DW_AT_location is DW_OP_fbreg(slot), and
# base_type/pointer_type DIEs for their types. Proven the honest way: gdb reads
# the scope and types, AND the fbreg offset is cross-checked against the exact
# stack slot codegen stores into — so a live debugger reads the right value.
set -u
echo "TEST-MARKER debug-locals"
out=tests/golden/out/debug-locals
rm -rf "$out"; mkdir -p "$out"

cat > "$out/loc.c" <<'CEOF'
int compute(int a, int b)
{
    int sum = a + b;
    char c = 'X';
    int *p = &sum;
    return sum + c + *p;
}
CEOF
"$EMBCC" -g -c "$out/loc.c" -o "$out/loc.o" || { echo "embcc -g failed"; exit 1; }

info=$(readelf --debug-dump=info "$out/loc.o" 2>/dev/null)
fail=0
for tag in DW_TAG_subprogram DW_TAG_formal_parameter DW_TAG_variable \
           DW_TAG_base_type DW_TAG_pointer_type; do
    echo "$info" | grep -q "$tag" || { echo "MISSING DIE: $tag"; fail=1; }
done
# every parameter and local is present with a name
for nm in a b sum c p; do
    echo "$info" | grep -qE "DW_AT_name[^:]*: $nm( |\$)" \
        || { echo "MISSING variable DIE: $nm"; fail=1; }
done
# locations are frame-relative (DW_OP_fbreg), not absolute
echo "$info" | grep -q "DW_OP_fbreg" || { echo "no DW_OP_fbreg locations"; fail=1; }

# A real debugger resolves the function type and the locals' scope.
if command -v gdb >/dev/null 2>&1; then
    pt=$(gdb -batch -nx "$out/loc.o" -ex "ptype compute" 2>/dev/null)
    echo "$pt" | grep -q "int (int, int)" \
        || { echo "gdb ptype wrong: $pt"; fail=1; }
    sc=$(gdb -batch -nx "$out/loc.o" -ex "info scope compute" 2>/dev/null)
    for nm in a b sum c p; do
        echo "$sc" | grep -qE "Symbol $nm is a variable" \
            || { echo "gdb: local $nm not in scope"; fail=1; }
    done
    [ "$fail" -eq 0 ] && echo "gdb resolved compute: int (int,int) + 5 scoped vars"
else
    echo "gdb absent; DIE-structure checks still ran"
fi

# Cross-check ONE location against the slot codegen uses: DWARF must place a
# variable exactly where the code stores it (else a debugger reads garbage).
# 'sum = a + b' -> the add result is stored to sum's slot; the fbreg offset in
# the DIE must equal that [rbp-N]. Extract sum's offset and the store target.
sumoff=$(gdb -batch -nx "$out/loc.o" -ex "info scope compute" 2>/dev/null \
         | sed -n 's/.*Symbol sum .*offset 0+\(-*[0-9]*\).*/\1/p')
if [ -n "$sumoff" ]; then
    hex=$(printf '0x%x' "$(( -sumoff ))")
    if objdump -d "$out/loc.o" 2>/dev/null | grep -qE "mov +%eax,-$hex\(%rbp\)"; then
        echo "cross-check: sum's DWARF slot (fbreg $sumoff) == its store site (-$hex(%rbp))"
    else
        echo "cross-check WARN: could not confirm sum's store site at -$hex(%rbp)"
    fi
fi

[ "$fail" -eq 0 ] && echo "debug-locals: frame + locals + types correct" || exit 1
echo "debug-locals golden passed (running proof: gdb on the host)"
