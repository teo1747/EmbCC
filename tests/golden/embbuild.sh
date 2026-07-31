#!/bin/sh
# M4, the host half: EmbCC's build expressed as an EmbBuild manifest, and that
# manifest actually building EmbCC. ROADMAP M4 is "EmbLinkOS builds EmbCC, with
# EmbBuild, from /data/src" — the total loop. EmbBuild (myos docs/BUILD.md) and
# the on-OS self-host already exist; the missing artifact was the manifest, and
# hand-writing its header closures is the staleness trap BUILD.md 6 warns of.
#
# This proves the generated manifest is correct and complete by WALKING it with
# a host reference EmbBuild (tools/embbuild-run.sh) — the same walk the OS's
# EmbBuild performs, mapped onto the host tree — and checking it yields a
# well-formed, fully-resolved embcc.elf. Running that binary is on-OS only (it
# links against newlib for the EmbLink syscall ABI), exactly like the self-host
# stage1. Skips honestly when the OS runtime / newlib are not on this host.
set -eu
echo "TEST-MARKER embbuild"

EMBCC=${EMBCC:-./embcc}
[ -x ./embld ] || { echo "embld not built"; exit 1; }

NEWLIB_INC=${NEWLIB_INC:-/home/motsou/cross/newlib-c99/x86_64-elf/include}
CRT0=${CRT0:-/home/motsou/myos/build/crt0.o}
SYSCALLS=${SYSCALLS:-/home/motsou/myos/build/syscalls.o}
LIBC=${LIBC:-/home/motsou/cross/newlib-c99/x86_64-elf/lib/libc.a}
for f in "$NEWLIB_INC/stdio.h" "$CRT0" "$SYSCALLS" "$LIBC"; do
    [ -e "$f" ] || { echo "skipped: $f not present on this host"; exit 0; }
done

out=tests/golden/out/embbuild
rm -rf "$out"; mkdir -p "$out"

# 1. Generate the manifest — header closures derived, not hand-listed.
tools/gen-embbuild-manifest.sh > "$out/embcc.build.ebm"
n=$(grep -c '^name:' "$out/embcc.build.ebm")
echo "generated a manifest with $n targets (header closures derived)"

# 2. The committed build.ebm must match a fresh generation (else it is stale —
#    re-run tools/gen-embbuild-manifest.sh > build.ebm).
if [ -f build.ebm ]; then
    if cmp -s build.ebm "$out/embcc.build.ebm"; then
        echo "committed build.ebm is up to date"
    else
        echo "committed build.ebm is STALE — re-run tools/gen-embbuild-manifest.sh"
        exit 1
    fi
fi

# 3. Walk it with the host reference EmbBuild — compiles every unit with embcc,
#    links with embld, and asserts a resolved ET_EXEC embcc.elf.
tools/embbuild-run.sh "$out/embcc.build.ebm" "$out/run"

echo "M4 host half: the manifest builds EmbCC (embcc + embld, from the manifest)"
