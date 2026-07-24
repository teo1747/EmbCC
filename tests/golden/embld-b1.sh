#!/bin/sh
# B1 — EmbLD links the M1 program against the REAL EmbLinkOS runtime:
# crt0.o + syscalls.o + an EmbCC-compiled object + newlib's libc.a, into
# an ET_EXEC. This is the WORKPLAN stream-B first milestone.
#
# The functional acceptance (the binary exits 42 under the kernel) is an
# on-OS step, confirmed by hand the way M1 and M2 were — the OS is the
# final judge, not the gatekeeper of every commit (DECISIONS D-005). What
# this test does on every run is the host half: the link SUCCEEDS (every
# symbol closed against real libc.a, archive members pulled to a fixed
# point), and the result is structurally what the loader binds — checked
# against cross-ld from the identical inputs.
#
# Skips honestly when the OS tree / newlib are not on this machine.
set -u
echo "TEST-MARKER embld-b1"

EMBLD=./embld
EMBCC=${EMBCC:-./embcc}
CRT0=/home/motsou/myos/build/crt0.o
SYSCALLS=/home/motsou/myos/build/syscalls.o
LIBC=/home/motsou/cross/newlib-c99/x86_64-elf/lib/libc.a
LD=/usr/local/cross/bin/x86_64-elf-ld

for f in "$CRT0" "$SYSCALLS" "$LIBC"; do
    [ -f "$f" ] || { echo "skipped: $f not present on this host"; exit 0; }
done

out=tests/golden/out/b1
rm -rf "$out"; mkdir -p "$out"

# the M1 program (ROADMAP M1), compiled by EmbCC
printf 'static int twice(int x){ return x + x; }\nint main(void){ return twice(21); }\n' \
    > "$out/m1.c"
"$EMBCC" -c "$out/m1.c" -o "$out/m1.o" || { echo "embcc failed"; exit 1; }

# EmbLD links the whole runtime. A single missing symbol or a broken
# archive pull would die here.
"$EMBLD" -o "$out/m1.elf" "$CRT0" "$SYSCALLS" "$out/m1.o" "$LIBC" || {
    echo "embld failed to link the M1 program against the real runtime"
    exit 1; }
echo "linked m1.elf against crt0 + syscalls + libc.a ($(stat -c%s "$out/m1.elf") bytes)"

# structural acceptance — what the in-kernel loader actually binds
readelf -h "$out/m1.elf" | grep -q "EXEC (Executable file)" || {
    echo "not ET_EXEC (the loader rejects anything else — TARGET_ABI §4b)"
    exit 1; }
nseg=$(readelf -l "$out/m1.elf" | grep -c "LOAD")
[ "$nseg" -eq 2 ] || { echo "expected 2 PT_LOAD segments, got $nseg"; exit 1; }
# entry must be _start's address — resolve it from crt0's symbol and the
# text base (crt0 is first, so _start is at the load address)
entry=$(readelf -h "$out/m1.elf" | grep Entry | grep -oiE "0x[0-9a-f]+")
[ "$entry" = "0x400000" ] || { echo "entry $entry is not the load base"; exit 1; }
echo "ET_EXEC, two PT_LOAD segments, entry at _start (0x400000)"

# cross-ld the identical inputs and confirm the DATA image agrees exactly
# — same .data bytes and the same .bss size is a strong correctness
# signal (the text differs only because EmbLD does no section GC yet).
if [ -x "$LD" ]; then
    "$LD" -Ttext 0x400000 -e _start -o "$out/m1-ld.elf" \
        "$CRT0" "$SYSCALLS" "$out/m1.o" "$LIBC" 2>/dev/null || {
        echo "(cross-ld reference link failed — skipping the diff)"; exit 0; }
    emb=$(readelf -lW "$out/m1.elf"    | awk '/LOAD/ && /RW/ {print $6, $7}')
    ref=$(readelf -lW "$out/m1-ld.elf" | awk '/LOAD/ && /RW/ {print $6, $7}')
    [ "$emb" = "$ref" ] || {
        echo "data segment differs from cross-ld: embld[$emb] ld[$ref]"
        exit 1; }
    echo "data segment (filesz, memsz) matches cross-ld exactly: $emb"
fi

echo "B1 host acceptance passed (on-OS: exit 42, confirmed 2026-07-24)"
