#!/bin/sh
# embbuild-run.sh — a host-side reference EmbBuild: walk a `.ebm` manifest and
# execute each target's recipe, exactly as the OS's EmbBuild will (myos
# docs/BUILD.md — a typed-manifest walker, one spawn per target, content-stamp
# staleness). It exists to PROVE a generated manifest actually builds, before
# the artifact ships to the OS, and as the host half of BUILD.md 10's
# two-implementation oracle (a host build and the on-OS build agreeing).
#
# The manifest carries on-OS absolute paths (/data/src/embcc, /system/abi, ...).
# This runner maps them back onto the host tree so the SAME manifest that ships
# to /data/src/embcc/build.ebm is what runs here — the structure (units,
# header closures, flags, link order) is what gets validated, not the paths.
#
#   usage: tools/embbuild-run.sh MANIFEST.ebm [OUTDIR]
#   env:   NEWLIB_INC, CRT0, SYSCALLS, LIBC  (the on-OS /system/abi stand-ins)
set -eu

cd "$(dirname "$0")/.."
MANIFEST=${1:?usage: embbuild-run.sh MANIFEST.ebm [OUTDIR]}
OUT=${2:-tests/golden/out/embbuild}
NEWLIB_INC=${NEWLIB_INC:-/home/motsou/cross/newlib-c99/x86_64-elf/include}
CRT0=${CRT0:-/home/motsou/myos/build/crt0.o}
SYSCALLS=${SYSCALLS:-/home/motsou/myos/build/syscalls.o}
LIBC=${LIBC:-/home/motsou/cross/newlib-c99/x86_64-elf/lib/libc.a}
HOST=$(pwd)

for f in "$NEWLIB_INC/stdio.h" "$CRT0" "$SYSCALLS" "$LIBC"; do
    [ -e "$f" ] || { echo "skipped: $f not present on this host"; exit 0; }
done

rm -rf "$OUT"; mkdir -p "$OUT/stage"
STAGE="$OUT/stage"

# Map one on-OS path token to its host equivalent (most specific first). A
# leading -I flag is carried across (the path is glued to it in the argv).
maphost() {
    pre=""; p="$1"
    case "$p" in -I*) pre="-I"; p=${p#-I} ;; esac
    case "$p" in
        /data/apps/embcc/embcc.elf) p="$HOST/embcc" ;;
        /data/apps/embld/embld.elf) p="$HOST/embld" ;;
        /data/apps/embcc/include)   p="$HOST/include" ;;
        /data/apps/embcc/include/*) p="$HOST/include/${p#/data/apps/embcc/include/}" ;;
        /system/abi/include)        p="$NEWLIB_INC" ;;
        /system/abi/include/*)      p="$NEWLIB_INC/${p#/system/abi/include/}" ;;
        /system/abi/crt0.o)         p="$CRT0" ;;
        /system/abi/syscalls.o)     p="$SYSCALLS" ;;
        /system/abi/libc.a)         p="$LIBC" ;;
        /data/src/embcc/*)          p="$HOST/src/${p#/data/src/embcc/}" ;;
        /data/build/out/embcc/*)    p="$STAGE/${p#/data/build/out/embcc/}" ;;
        /data/build/out/embcc)      p="$STAGE" ;;
    esac
    echo "$pre$p"
}

# Walk the manifest: on each blank line, run the accumulated target's `args`.
run_target() {
    [ -n "${T_kind:-}" ] || return 0
    # Rebuild the argv with every token path-mapped.
    set --
    for tok in $T_args; do set -- "$@" "$(maphost "$tok")"; done
    echo "  [$T_kind] $T_name"
    "$@" || { echo "embbuild-run: recipe failed for $T_name"; exit 1; }
}

echo "TEST-MARKER embbuild"
T_name=; T_kind=; T_args=; project=
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        '#'*) continue ;;
        'project:'*) project=${line#project: } ;;
        'name: '*)   run_target; T_name=${line#name: }; T_kind=; T_args= ;;
        'kind: '*)   T_kind=${line#kind: } ;;
        'args: '*)   T_args=${line#args: } ;;
        'inputs:'*|'output:'*) : ;;
        '') run_target; T_name=; T_kind=; T_args= ;;
    esac
done < "$MANIFEST"
run_target      # last target if the file has no trailing blank line

echo "EmbBuild(host) ran project '$project' from $MANIFEST"

# Acceptance: the manifest produced a well-formed, fully-resolved ET_EXEC — the
# same structural gate as the self-host build. (Running it is on-OS only: it is
# linked against newlib for the EmbLink syscall ABI, not this host's glibc.)
ELF="$STAGE/embcc.elf"
[ -f "$ELF" ] || { echo "manifest produced no embcc.elf"; exit 1; }
readelf -h "$ELF" | grep -q 'EXEC (Executable file)' || { echo "embcc.elf is not ET_EXEC"; exit 1; }
und=$(readelf -sW "$ELF" 2>/dev/null | awk '$7=="UND" && $8!="" {print $8}' | grep -v '^$' || true)
[ -z "$und" ] || { echo "embcc.elf has unresolved symbols:"; echo "$und"; exit 1; }
sz=$(stat -c%s "$ELF")
echo "manifest built embcc.elf ($sz bytes): ET_EXEC, every symbol resolved"
