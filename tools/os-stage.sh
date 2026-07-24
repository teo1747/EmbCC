#!/bin/sh
# os-stage.sh — build an EmbCC-compiled program FOR EmbLinkOS and stage it into
# the OS image, so the machine can judge it (ROADMAP M1's acceptance).
#
# This is the one place the two repositories touch, and it touches them in the
# safe direction: EmbCC reaches into the OS tree for the ABI objects and drops
# a finished .elf into its build/ staging area. Nothing in EmbLinkOS reaches
# back, so the OS still builds, boots, and passes its suite with this repo
# absent (DECISIONS D-001).
#
# What it does NOT do, on purpose: link. EmbCC has no linker yet (that is M3),
# so the cross toolchain's ld does the linking here — exactly the split
# ROADMAP M1 specifies ("linking is still done by the existing toolchain").
# Claiming otherwise would be the overclaim CONTRIBUTING exists to prevent.
#
#   usage: tools/os-stage.sh [source.c] [name]
#     source.c  program to compile   (default: tests/exec/exit42.c)
#     name      staged binary name   (default: embcc42)
#
#   env: MYOS  path to the EmbLinkOS tree (default: ../myos)
#
# Then, in the OS tree:
#     make STAGED_APPS=build/embcc42.elf
# naming the file so the image actually rebuilds — mkfs packs every build/*.elf
# it finds, but the OS's Makefile only rebuilds the image for prerequisites it
# was told about, and its drift guard exists precisely to catch this.
# It installs at /data/apps/<name>/<name>.elf. Boot, then at the debug console:
#     run /data/apps/embcc42/embcc42.elf
#     ps                      # the spawned pid's EXIT column
# Exit code 42 is M1. `run` does not wait, which is why the answer is read from
# ps rather than from run itself; the kernel's own `[syscall] exit code=0x2A`
# trace is the second, independent witness.
set -eu

cd "$(dirname "$0")/.."
SRC=${1:-tests/exec/exit42.c}
NAME=${2:-embcc42}
MYOS=${MYOS:-$(cd .. && pwd)/myos}

[ -d "$MYOS" ] || { echo "os-stage: no OS tree at $MYOS (set MYOS=)" >&2; exit 1; }

# The OS's Makefile is the ONE source of truth for where its newlib lives — ask
# it rather than hardcoding a second copy of the path that could silently drift.
NEWLIB=$(make -s -C "$MYOS" print-newlib-prefix)
CROSS=${CROSS:-x86_64-elf}
CRT0="$MYOS/build/crt0.o"
SYSCALLS="$MYOS/build/syscalls.o"
LDSCRIPT="$MYOS/user/lib/newlib.ld"

for f in "$CRT0" "$SYSCALLS" "$LDSCRIPT"; do
    [ -f "$f" ] || { echo "os-stage: missing $f — run \`make\` in $MYOS first" >&2; exit 1; }
done

make -s embcc

OUT=build/os
mkdir -p "$OUT"
OBJ="$OUT/$NAME.o"
ELF="$OUT/$NAME.elf"
rm -f "$OBJ" "$ELF"

echo "embcc  -c $SRC -o $OBJ"
./embcc -c "$SRC" -o "$OBJ"

# -nostartfiles: crt0.o provides _start, there are no crtX files here.
# -static: the kernel binds a shared libembk.so only for UI apps; an M1 program
#          has no UI and takes the simpler shape (TARGET_ABI §4a).
echo "$CROSS-gcc -nostartfiles -static -T newlib.ld crt0.o syscalls.o $NAME.o -lc -lgcc"
"$CROSS-gcc" -nostartfiles -static -T "$LDSCRIPT" \
    -L"$NEWLIB/$CROSS/lib" \
    "$CRT0" "$SYSCALLS" "$OBJ" -lc -lgcc -o "$ELF"

# Independent confirmation that the artifact is the shape the OS loader wants,
# checked with a tool that has no stake in EmbCC being right (CONTRIBUTING:
# "compare against gcc/TCC with readelf/objdump/nm").
"$CROSS-readelf" -h "$ELF" | grep -E "Type:|Entry point"

cp "$ELF" "$MYOS/build/$NAME.elf"
echo "staged -> $MYOS/build/$NAME.elf"
echo "next:   make -C $MYOS STAGED_APPS=build/$NAME.elf && boot && \`run /data/apps/$NAME/$NAME.elf\` then \`ps\`  (want EXIT 42)"
