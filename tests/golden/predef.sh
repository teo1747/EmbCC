#!/bin/sh
# The predefined-macro table (ARCHITECTURE.md §5). Golden test: when the
# reference compiler is available, EmbCC's table must match it exactly
# (minus the excluded __GNUC*/__STDC* families — see tools/gen-predef.sh).
# Without it, fall back to the macros whose absence made newlib's headers
# hard-#error under TCC.
set -u
echo "TEST-MARKER predef"

out=$("$EMBCC" --dump-predef) || { echo "--dump-predef exited nonzero"; exit 1; }

GCC="${EMBCC_REF_GCC:-x86_64-elf-gcc}"
command -v "$GCC" >/dev/null 2>&1 || GCC=/usr/local/cross/bin/x86_64-elf-gcc

if command -v "$GCC" >/dev/null 2>&1; then
    ref=$("$GCC" -dM -E - </dev/null | LC_ALL=C sort \
          | grep -v -E '^#define (__GNUC|__VERSION__|__STDC)')
    if [ "$out" != "$ref" ]; then
        echo "table disagrees with $GCC -dM -E:"
        printf '%s\n' "$out" > "${TMPDIR:-/tmp}/predef.embcc.$$"
        printf '%s\n' "$ref" | diff -u - "${TMPDIR:-/tmp}/predef.embcc.$$"
        rm -f "${TMPDIR:-/tmp}/predef.embcc.$$"
        exit 1
    fi
    echo "matches $GCC -dM -E ($(printf '%s\n' "$out" | wc -l) macros)"
else
    echo "reference gcc not found; checking the known-fatal macros only"
    for m in __INT64_TYPE__ __INTPTR_TYPE__ __SIZE_TYPE__ __PTRDIFF_TYPE__ \
             __CHAR_BIT__ __SIZEOF_POINTER__ __SIZEOF_LONG__ __LP64__ \
             __x86_64__ __ELF__; do
        echo "$out" | grep -q "^#define $m " || {
            echo "missing $m (this is the TCC-patch-0002 class of break)"
            exit 1
        }
    done
    n=$(printf '%s\n' "$out" | wc -l)
    [ "$n" -ge 300 ] || { echo "only $n macros — table looks truncated"; exit 1; }
fi
