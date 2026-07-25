#!/bin/sh
# os-build-embld.sh — cross-build the EmbLD linker itself INTO an EmbLinkOS
# binary (embld.elf) and stage it into the OS image tree.
#
# This is what makes the self-hosting loop close ON THE OS: with embld.elf on
# the image, the OS can compile its own compiler's 14 sources (embcc) AND link
# them into a runnable embcc — no host toolchain, no tcc, in the loop. Until
# now embld linked OS-targeted binaries but only while running on the host
# (gen-selfhost-ref.sh). Same linker, same inputs, run on the metal.
#
# Same safe direction as os-stage.sh (DECISIONS D-001): EmbCC reaches into the
# OS tree for the ABI objects and drops a finished .elf into build/. Nothing in
# EmbLinkOS reaches back.
#
#   usage: tools/os-build-embld.sh
#   env:   MYOS   path to the EmbLinkOS tree (default: ../myos)
#          CROSS  cross triple (default: x86_64-elf)
#
# After it stages build/embld.elf, rebuild the OS image (with EMBK_EMBCC_ROOT
# set so the sources+refs pack too) and boot: `test embld`, `test embcc self`,
# `test embcc selfhost`.
set -eu

cd "$(dirname "$0")/.."
MYOS=${MYOS:-$(cd .. && pwd)/myos}
CROSS=${CROSS:-x86_64-elf}

[ -d "$MYOS" ] || { echo "os-build-embld: no OS tree at $MYOS (set MYOS=)" >&2; exit 1; }

# The OS's Makefile is the ONE source of truth for its newlib prefix.
NEWLIB=$(make -s -C "$MYOS" print-newlib-prefix)
CRT0="$MYOS/build/crt0.o"
SYSCALLS="$MYOS/build/syscalls.o"
LDSCRIPT="$MYOS/user/lib/newlib.ld"
for f in "$CRT0" "$SYSCALLS" "$LDSCRIPT"; do
    [ -f "$f" ] || { echo "os-build-embld: missing $f — run \`make\` in $MYOS first" >&2; exit 1; }
done

# EmbLD's source set is exactly the host embld target's (Makefile), minus its
# host main: the linker library (src/link/link.c), the diag chokepoint
# (src/driver/util.c), the CLI driver (tools/embld/embld.c), and the EmbDBG
# core (tools/embdbg/embdbg.c, -DEMBDBG_NO_MAIN) so a link can emit a native
# .embdbg through the same writer the embdbg tool uses.
SRCS="tools/embld/embld.c src/link/link.c src/driver/util.c tools/embdbg/embdbg.c"

# Match the OS's own user-app compile flags (myos Makefile NEWLIB_CFLAGS).
CFLAGS="-std=c99 -mno-red-zone -fno-stack-protector -O2 -DEMBDBG_NO_MAIN -Wno-unused-function \
    -isystem $NEWLIB/$CROSS/include"

OUT=build/os
mkdir -p "$OUT"
OBJS=""
for f in $SRCS; do
    o="$OUT/$(basename "$f" .c).embld.o"
    echo "$CROSS-gcc -c $f -> $o"
    "$CROSS-gcc" $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

# -nostartfiles/-static/-T newlib.ld: the EmbLink static-app contract
# (TARGET_ABI §4a) — crt0.o provides _start, no interpreter, no crtX.
ELF="$MYOS/build/embld.elf"
echo "$CROSS-gcc -nostartfiles -static -T newlib.ld crt0.o syscalls.o <objs> -lc -lgcc -o embld.elf"
"$CROSS-gcc" -nostartfiles -static -T "$LDSCRIPT" \
    -L"$NEWLIB/$CROSS/lib" \
    "$CRT0" "$SYSCALLS" $OBJS -lc -lgcc -o "$ELF"

# Independent shape check with a tool that has no stake in EmbCC being right.
"$CROSS-readelf" -h "$ELF" | grep -E "Type:|Machine:|Entry point"
echo "staged -> $ELF ($(stat -c%s "$ELF") bytes)"
