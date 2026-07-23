#!/bin/sh
# embread against REAL EMBX images — the ones EmbLinkOS's own producer
# made and its kernel loads (myos/build/*.embx).
#
# Two halves, and the second is the one that matters: an image the
# kernel accepts must pass every guard, and a CORRUPTED image must be
# refused at the right step. A verifier that only ever says yes has
# verified nothing (CONTRIBUTING: a green test that cannot fail is a
# lie waiting to happen).
#
# Skips honestly when the OS tree is not on this machine; the
# not-an-EMBX case always runs, because it needs no fixture.
set -u
echo "TEST-MARKER embx-embread"

EMBREAD=./embread
[ -x "$EMBREAD" ] || { echo "embread not built"; exit 1; }

out_dir="tests/golden/out"
mkdir -p "$out_dir"

# --- always runs: a non-EMBX file is refused (§6.2) ---
if "$EMBREAD" -q "$EMBREAD" 2>/dev/null; then
    echo "accepted a non-EMBX file as EMBX"
    exit 1
fi
"$EMBREAD" -q "$EMBREAD" 2>&1 | grep -q "EMAGIC" || {
    echo "wrong diagnostic for a non-EMBX file"; exit 1; }
echo "non-EMBX file: refused with EMAGIC"

IMG=/home/motsou/myos/build/capchild.embx
if [ ! -f "$IMG" ]; then
    echo "skipped the real-image half: $IMG not present on this host"
    exit 0
fi

# --- the accept case: the kernel loads this exact image ---
"$EMBREAD" -q "$IMG" || { echo "a real, kernel-loadable image failed"; exit 1; }
echo "real image: every §8 guard passed"

# The dump must actually report the capability that is the format's
# whole reason for existing (§5).
"$EMBREAD" "$IMG" | grep -q "FILESYSTEM" || {
    echo "declared capability missing from the dump"; exit 1; }
echo "real image: declared capability reported"

# --- the refuse cases: corruption must be caught, in the right place ---
corrupt() { # name byte-offset expected-error
    python3 -c "
import sys
d = bytearray(open('$IMG','rb').read())
d[$2] ^= 0xFF
open('$out_dir/embx-$1.embx','wb').write(d)" || {
        echo "could not build the $1 fixture"; exit 1; }
    if "$EMBREAD" -q "$out_dir/embx-$1.embx" 2>/dev/null; then
        echo "$1: corruption was NOT caught"
        exit 1
    fi
    "$EMBREAD" -q "$out_dir/embx-$1.embx" 2>&1 | grep -q "$3" || {
        echo "$1: caught, but not as $3:"
        "$EMBREAD" -q "$out_dir/embx-$1.embx" 2>&1 | sed 's/^/    /'
        exit 1
    }
    echo "$1: refused with $3"
}

corrupt header-field 48 ECHECKSUM   # entry_point (0x30): header CRC catches it
corrupt payload 5000 ECHECKSUM      # a text byte: the segment CRC catches it

# truncation is its own guard (§6.8)
python3 -c "
d = bytearray(open('$IMG','rb').read())[:60000]
open('$out_dir/embx-trunc.embx','wb').write(d)"
if "$EMBREAD" -q "$out_dir/embx-trunc.embx" 2>/dev/null; then
    echo "truncation was NOT caught"
    exit 1
fi
"$EMBREAD" -q "$out_dir/embx-trunc.embx" 2>&1 | grep -q "ETRUNC" || {
    echo "truncation caught, but not as ETRUNC"; exit 1; }
echo "truncated image: refused with ETRUNC"
