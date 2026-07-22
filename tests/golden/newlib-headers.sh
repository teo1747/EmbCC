#!/bin/sh
# ARCHITECTURE §5's judge: REAL newlib headers. The predefined macro
# table plus the preprocessor must satisfy <stdint.h>/<stddef.h> —
# the exact chain (cdefs.h, _default_types.h) that broke TCC until
# patch 0002. Skips honestly when the cross toolchain is absent.
set -u
echo "TEST-MARKER newlib-headers"

NEWLIB=/usr/local/cross/x86_64-elf/include
if [ ! -d "$NEWLIB" ]; then
    echo "skipped: cross newlib headers not present on this host"
    exit 0
fi
out_dir="tests/golden/out"
mkdir -p "$out_dir"
cat > "$out_dir/nl.c" << 'CEOF'
#include <stdint.h>
#include <stddef.h>
int main(void) {
    int32_t x = 40;
    uint8_t b = 2;
    size_t sz = sizeof(intptr_t);
    uint64_t wide = UINT32_MAX;
    if (wide + 1 != 4294967296UL)
        return 1;
    return (int)(x + b + (int)sz - 8); /* 42 */
}
CEOF
rm -f "$out_dir/nl.o" "$out_dir/nl"
"$EMBCC" -c "$out_dir/nl.c" -o "$out_dir/nl.o" \
    -I include -I "$NEWLIB" || { echo "compile failed"; exit 1; }
cc -o "$out_dir/nl" "$out_dir/nl.o" || { echo "link failed"; exit 1; }
"$out_dir/nl"
got=$?
[ "$got" -eq 42 ] || { echo "exit $got, expected 42"; exit 1; }
echo "newlib stdint.h/stddef.h: compiled, linked, ran, exit 42"
