#!/bin/sh
# EmbAS, the standalone NASM/Intel-syntax assembler (A1, ARCHITECTURE §4):
# the last external tool in the EmbLinkOS kernel build was nasm, for the
# kernel's hand-written `.asm`. EmbCC grows its own assembler so the
# toolchain owns the whole build.
#
# The correctness bar is byte-identical-to-nasm — the only reference an
# assembler can be checked against on the host. This exercises the encoder
# and directives on a fixture that mirrors the kernel corpus (sections,
# global/extern, local-label scoping, jump relaxation, db/dw/dd/dq, resb,
# a movabs+ABS64 relocation, a call+PC32 relocation, and incbin) and
# compares the link-relevant object content: code bytes, the symbol table,
# and the relocations. Skips honestly when nasm is not on this host.
set -u
echo "TEST-MARKER assembler"

EMBCC=${EMBCC:-./embcc}
[ -x ./embas ] || { echo "embas not built"; exit 1; }

command -v nasm >/dev/null 2>&1 || { echo "skipped: nasm not on this host (no reference)"; exit 0; }
command -v readelf >/dev/null 2>&1 || { echo "skipped: readelf not on this host"; exit 0; }

out=tests/golden/out/assembler
rm -rf "$out"; mkdir -p "$out"

# A raw blob to incbin — the AP-trampoline embedding technique in miniature.
printf 'EMBCC-A1\001\002\003\004' > "$out/blob.bin"

cat > "$out/fix.asm" << 'EOF'
BITS 64

section .bss
scratch:
    resb 64
global stack_top
stack_top:

section .rodata
global table
table:
    db 1, 2, 3, 0x40
    dw 0x1122, 0x3344
    dd 0xdeadbeef
    dq 0x1122334455667788
global blob_start
blob_start:
    incbin "OUTDIR/blob.bin"
blob_end:

section .text
global _start
extern external_fn
_start:
    mov rsp, stack_top          ; movabs + R_X86_64_64 (defined -> .bss + addend)
    xor eax, eax
    cmp eax, 0
    je .done                    ; short forward jump (rel8 after relaxation)
    call external_fn            ; R_X86_64_PC32 against an extern
    add eax, 1
.done:
    ret
EOF
sed "s#OUTDIR#$out#" "$out/fix.asm" > "$out/fix.real.asm"

nasm -f elf64 "$out/fix.real.asm" -o "$out/n.o" || { echo "nasm failed on fixture"; exit 1; }
./embas -f elf64 "$out/fix.real.asm" -o "$out/e.o" || { echo "embas failed on fixture"; exit 1; }

fail=0

# 1. Code bytes: every allocatable section identical.
for s in .text .rodata .data; do
    objcopy -O binary --only-section=$s "$out/n.o" "$out/n.$s.bin" 2>/dev/null
    objcopy -O binary --only-section=$s "$out/e.o" "$out/e.$s.bin" 2>/dev/null
    if ! cmp -s "$out/n.$s.bin" "$out/e.$s.bin"; then
        echo "$s differs from nasm"; fail=1
    fi
done
[ $fail -eq 0 ] && echo "code bytes (.text/.rodata/.data) byte-identical to nasm"

# 2. Symbol table: same entries, order, values, types, binds, names.
readelf -sW "$out/n.o" | sed 's/  */ /g' > "$out/n.sym"
readelf -sW "$out/e.o" | sed 's/  */ /g' > "$out/e.sym"
if diff "$out/n.sym" "$out/e.sym" >/dev/null; then
    echo "symbol table byte-identical to nasm"
else
    echo "symbol table differs from nasm:"; diff "$out/n.sym" "$out/e.sym" | head; fail=1
fi

# 3. Relocations: same offsets, types, symbol references, addends.
readelf -rW "$out/n.o" | grep -iE '^[0-9a-f]{8}' > "$out/n.rel"
readelf -rW "$out/e.o" | grep -iE '^[0-9a-f]{8}' > "$out/e.rel"
if diff "$out/n.rel" "$out/e.rel" >/dev/null; then
    echo "relocations byte-identical to nasm"
else
    echo "relocations differ from nasm:"; diff "$out/n.rel" "$out/e.rel" | head; fail=1
fi

# 4. The driver dispatches `.asm` to the same assembler: embcc -c == embas.
"$EMBCC" -c "$out/fix.real.asm" -o "$out/via-embcc.o" || {
    echo "embcc -c fix.asm failed"; fail=1; }
if cmp -s "$out/e.o" "$out/via-embcc.o"; then
    echo "embcc -c foo.asm produces the same object as embas"
else
    echo "embcc -c foo.asm differs from embas"; fail=1
fi

[ $fail -eq 0 ] && echo "assembler byte-identical to nasm on the fixture"
exit $fail
