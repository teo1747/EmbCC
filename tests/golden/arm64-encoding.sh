#!/bin/sh
# Every aarch64 encoding EmbCC emits, disassembled by aarch64-elf-objdump and
# diffed against what the emitter CLAIMS each call produces.
#
# A backend that assembles its own instructions has no assembler to catch a
# wrong bit, and a wrong bit is a silently wrong program rather than a build
# failure. This is the only thing standing between the two, so it checks the
# displacement of every branch as well as the opcode of every instruction.
set -eu
echo "TEST-MARKER arm64-encoding"

cd "$(dirname "$0")/../.."
OBJDUMP="${EMBCC_AARCH64_OBJDUMP:-aarch64-elf-objdump}"
OBJCOPY="${EMBCC_AARCH64_OBJCOPY:-aarch64-elf-objcopy}"
CC="${CC:-cc}"

command -v "$OBJDUMP" >/dev/null 2>&1 || {
    echo "arm64-encoding: $OBJDUMP not found — cannot referee the encodings" >&2
    exit 1
}

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

$CC -std=c99 -Wall -Wextra -Werror -o "$out/a64check" \
    tools/a64check/a64check.c src/asm/emit_arm64.c src/asm/emit.c \
    src/driver/util.c

"$out/a64check" > "$out/bin" 2> "$out/want"

"$OBJCOPY" -I binary -O elf64-littleaarch64 -B aarch64 "$out/bin" "$out/elf"
"$OBJDUMP" -D -m aarch64 "$out/elf" \
    | sed -n '/^ *[0-9a-f]*:	/p' \
    | sed -E 's/^[^	]*	[0-9a-f]{8} *	//' \
    | sed -E 's/[[:blank:]]*\/\/.*$//' \
    | sed -E 's/[[:blank:]]*<[^>]*>$//' \
    | sed -E 's/[[:blank:]]+$//' > "$out/got"

if diff -u "$out/want" "$out/got"; then
    echo "arm64-encoding: $(wc -l < "$out/got" | tr -d ' ') encodings agree with objdump"
else
    echo "arm64-encoding: emitter disagrees with objdump (above)" >&2
    exit 1
fi
