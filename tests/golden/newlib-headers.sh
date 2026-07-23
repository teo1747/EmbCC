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
cc -no-pie -o "$out_dir/nl" "$out_dir/nl.o" || { echo "link failed"; exit 1; }
"$out_dir/nl"
got=$?
[ "$got" -eq 42 ] || { echo "exit $got, expected 42"; exit 1; }
echo "newlib stdint.h/stddef.h: compiled, linked, ran, exit 42"

# The M2 goal sentence, literally: a program that #includes <stdio.h>
# and calls the real libc. (Function pointers in struct __sFILE, the
# getc/putc inline maze, string.h — the whole gauntlet.)
cat > "$out_dir/nlstdio.c" << 'CEOF'
#include <stdio.h>
#include <string.h>
int main(void) {
    char buf[32];
    strcpy(buf, "stdio via ");
    strcat(buf, "embcc");
    printf("%s (%d chars)\n", buf, (int)strlen(buf));
    return 42;
}
CEOF
rm -f "$out_dir/nlstdio.o" "$out_dir/nlstdio"
"$EMBCC" -c "$out_dir/nlstdio.c" -o "$out_dir/nlstdio.o" \
    -I include -I "$NEWLIB" || { echo "stdio.h compile failed"; exit 1; }
cc -no-pie -o "$out_dir/nlstdio" "$out_dir/nlstdio.o" || {
    echo "stdio.h link failed"; exit 1; }
out=$("$out_dir/nlstdio"); got=$?
[ "$got" -eq 42 ] || { echo "stdio.h run: exit $got"; exit 1; }
echo "$out" | grep -q "stdio via embcc (15 chars)" || {
    echo "stdio.h run: wrong output: $out"; exit 1; }
echo "newlib stdio.h: compiled, linked, ran — M2's goal sentence works"
