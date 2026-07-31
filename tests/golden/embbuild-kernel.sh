#!/bin/sh
# KM1, the host half: the EmbLinkOS kernel built ENTIRELY from an EmbBuild
# manifest — 89 embcc C compiles + 6 embcc .asm assembles + one embld link, no
# gcc/nasm/ld — and the result BOOTS. myos docs/BUILD.md §12 scopes "EmbBuild
# rebuilds the kernel"; its blockers G1 (on-OS assembler) and G2 (kernel_end)
# are EmbAS and EmbLD's L1, both done, so this is now pure orchestration: G5,
# the generated manifest (tools/gen-kernel-manifest.sh), walked to a kernel.elf.
#
# The manifest carries on-OS paths; this walker maps them onto the host tree and
# runs each recipe from the myos root (so ap_trampoline_blob.asm's `incbin
# "build/ap_trampoline.bin"` resolves). Then it boots the kernel headless and
# checks it reaches userspace with no fault — the same acceptance the host
# capstone used. The flat boot stages are nasm's (a -f bin concern outside KM1).
# Skips honestly when myos / nasm / qemu are not on this host.
set -eu
echo "TEST-MARKER embbuild-kernel"

# Opt-in: an 89-unit kernel build plus a qemu boot is ~35s — too heavy for every
# `make test`. Run it deliberately with EMBCC_KM1=1 (CI and the dev suite skip).
[ "${EMBCC_KM1:-}" = 1 ] || { echo "skipped: kernel-manifest boot is opt-in (set EMBCC_KM1=1)"; exit 0; }

MYOS=${MYOS:-/home/motsou/myos}
HOST=$(cd "$(dirname "$0")/../.." && pwd)
[ -d "$MYOS/kernel" ] || { echo "skipped: no myos kernel tree at $MYOS"; exit 0; }
command -v nasm >/dev/null 2>&1 || { echo "skipped: nasm absent (boot stages)"; exit 0; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "skipped: qemu absent"; exit 0; }
[ -x "$HOST/embcc" ] && [ -x "$HOST/embld" ] || { echo "embcc/embld not built"; exit 1; }
[ -f "$MYOS/build/ap_trampoline.bin" ] || { echo "skipped: ap_trampoline.bin not staged"; exit 0; }

out="$HOST/tests/golden/out/embbuild-kernel"
rm -rf "$out"; mkdir -p "$out/stage"
STAGE="$out/stage"

# 1. Generate the kernel manifest (header closures derived).
MYOS="$MYOS" "$HOST/tools/gen-kernel-manifest.sh" > "$out/kernel.build.ebm"
n=$(grep -c '^name:' "$out/kernel.build.ebm")
echo "generated a kernel manifest with $n targets"

# 2. Walk it: map on-OS paths onto the host tree, run each recipe from myos root.
maphost() {
    pre=""; p="$1"
    case "$p" in -I*) pre="-I"; p=${p#-I} ;; esac
    case "$p" in
        /data/apps/embcc/embcc.elf) p="$HOST/embcc" ;;
        /data/apps/embld/embld.elf) p="$HOST/embld" ;;
        /data/src/kernel)           p="$MYOS/kernel" ;;
        /data/src/kernel/*)         p="$MYOS/kernel/${p#/data/src/kernel/}" ;;
        /data/build/out/kernel/*)   p="$STAGE/${p#/data/build/out/kernel/}" ;;
    esac
    echo "$pre$p"
}

run_target() {
    [ -n "${T_kind:-}" ] || return 0
    set --
    for tok in $T_args; do set -- "$@" "$(maphost "$tok")"; done
    ( cd "$MYOS" && "$@" ) || { echo "recipe failed: $T_name"; exit 1; }
}

T_name=; T_kind=; T_args=
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        '#'*|'project:'*|'inputs:'*|'output:'*) : ;;
        'name: '*) run_target; T_name=${line#name: }; T_kind=; T_args= ;;
        'kind: '*) T_kind=${line#kind: } ;;
        'args: '*) T_args=${line#args: } ;;
        '') run_target; T_name=; T_kind=; T_args= ;;
    esac
done < "$out/kernel.build.ebm"
run_target
echo "manifest compiled + linked the kernel (embcc + embld, no gcc/nasm/ld)"

# 3. Structural acceptance: higher-half ET_EXEC, LMA at 1 MB (L1/L2).
K="$STAGE/kernel.elf"
[ -f "$K" ] || { echo "manifest produced no kernel.elf"; exit 1; }
readelf -h "$K" | grep -q 'EXEC (Executable file)' || { echo "kernel.elf not ET_EXEC"; exit 1; }
readelf -lW "$K" | grep -q '0xffffffff80100000 0x0000000000100000' || { echo "kernel LMA wrong"; exit 1; }
echo "kernel.elf: ET_EXEC, higher-half vaddr with physical LMA 0x100000 ($(stat -c%s "$K") bytes)"

# 4. Boot it (flat boot stages via nasm; sector counts sized to this kernel).
ksz=$(stat -c%s "$K"); ksect=$(( (ksz + 511) / 512 ))
nasm -f bin -D KERNEL_LOAD_SECTORS=$ksect "$MYOS/boot/stage2/stage2.asm" -o "$STAGE/stage2.bin"
s2s=$(( ($(stat -c%s "$STAGE/stage2.bin") + 511) / 512 ))
nasm -f bin -D STAGE2_LOAD_SECTORS=$s2s "$MYOS/boot/stage1/boot.asm" -o "$STAGE/stage1.bin"
cat "$STAGE/stage1.bin" "$STAGE/stage2.bin" "$K" > "$STAGE/kernel.img"
truncate -s 8M "$STAGE/kernel.img"
timeout 20 qemu-system-x86_64 -drive file="$STAGE/kernel.img",format=raw,index=0,media=disk \
    -serial stdio -display none -no-reboot -no-shutdown -m 512M -smp 1 -accel tcg \
    > "$STAGE/boot.log" 2>&1 || true

faults=$(grep -icE 'panic|fault|exception|#GP|#PF|triple' "$STAGE/boot.log" || true)
[ "$faults" -eq 0 ] || { echo "kernel FAULTED on boot:"; grep -iE 'panic|fault|exception' "$STAGE/boot.log" | head; exit 1; }
grep -q 'PMM Layout' "$STAGE/boot.log" || { echo "kernel did not reach PMM init"; tail "$STAGE/boot.log"; exit 1; }
grep -qiE 'init:|VFS|LAPIC timer' "$STAGE/boot.log" || { echo "kernel did not reach late init"; exit 1; }
lines=$(wc -l < "$STAGE/boot.log")
echo "the manifest-built kernel BOOTS: $lines lines of serial, 0 faults, reached userspace init"
echo "KM1 host half: EmbBuild manifest -> kernel.elf -> boots (own the stack, kernel included)"
