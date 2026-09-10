#!/bin/sh
# Link one embcc-produced aarch64 object into a bare-metal image the QEMU
# virt harness can run, then leave it at $2.
#
#   usage: link.sh OBJECT OUTPUT.elf
#
# The harness pieces (entry, page tables, the semihosting syscall floor) are
# built with aarch64-elf-gcc on purpose: they are scaffolding, not the thing
# under test. Only OBJECT came from embcc.
set -eu

obj=$1
out=$2
here=$(dirname "$0")
work=${EMBCC_HARNESS_WORK:-$(dirname "$out")}

NEWLIB="${EMBCC_AARCH64_NEWLIB:-$HOME/cross/newlib-aarch64-c99/aarch64-elf}"
GCC="${EMBCC_AARCH64_GCC:-aarch64-elf-gcc}"
LD="${EMBCC_AARCH64_LD:-aarch64-elf-ld}"

[ -d "$NEWLIB/lib" ] || {
    echo "link.sh: no aarch64 newlib at $NEWLIB" >&2
    echo "link.sh: set EMBCC_AARCH64_NEWLIB to its <triple> directory" >&2
    exit 1
}

# The two harness objects are the same for every test; rebuild them only
# when missing or stale.
for part in start semihost; do
    src=$here/$part.$( [ "$part" = start ] && echo S || echo c )
    if [ ! -f "$work/harness-$part.o" ] || [ "$src" -nt "$work/harness-$part.o" ]; then
        $GCC -ffreestanding -isystem "$NEWLIB/include" \
             -c "$src" -o "$work/harness-$part.o"
    fi
done

LIBGCC=$(dirname "$($GCC -print-libgcc-file-name)")
$LD -T "$here/link.ld" -o "$out" \
    "$work/harness-start.o" "$obj" "$work/harness-semihost.o" \
    -L"$NEWLIB/lib" -lc -lm -L"$LIBGCC" -lgcc 2>&1 \
    | grep -v 'LOAD segment with RWX permissions' >&2 || true
[ -f "$out" ]
