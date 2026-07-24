#!/bin/sh
# Generate the self-hosting reference objects (ref/*.o) and relink stage1
# (embcc.elf) from them, for the on-OS fixed point (docs/SELFHOST_ONOS.md).
# The references are the gcc-built embcc's output for EmbCC's own sources;
# STT_FILE is the basename, so they match what the OS embcc produces.
set -eu
NEWLIB_INC=${NEWLIB_INC:-/home/motsou/cross/newlib-c99/x86_64-elf/include}
OS_BUILD=${OS_BUILD:-/home/motsou/myos/build}
LIBC=${LIBC:-/home/motsou/cross/newlib-c99/x86_64-elf/lib/libc.a}
INCS="-I include -I $NEWLIB_INC"
SRCS="src/driver/main.c src/driver/util.c src/lex/lex.c src/parse/parse.c
      src/sema/sema.c src/sema/type.c src/ir/irgen.c src/codegen/codegen.c
      src/asm/emit.c src/cpp/predef.c src/cpp/cpp.c src/elf/write.c"
rm -rf ref; mkdir -p ref
for f in $SRCS; do
    ./embcc -c "$f" $INCS -o "ref/$(basename "$f" .c).o"
done
echo "wrote $(ls ref/*.o | wc -l) reference objects"
./embld -o "$OS_BUILD/embcc.elf" "$OS_BUILD/crt0.o" "$OS_BUILD/syscalls.o" \
    ref/*.o "$LIBC"
echo "relinked stage1 -> $OS_BUILD/embcc.elf ($(stat -c%s "$OS_BUILD/embcc.elf") bytes)"
