#!/bin/sh
# Diagnostics: a located heading (file:line:col), the offending SOURCE LINE, and
# a caret under the exact column — across #includes, with colour only on a
# terminal. VISION_LONGTERM calls actionable diagnostics the one quality axis
# that is NOT deferred; this is the caret core of it.
set -eu
echo "TEST-MARKER diagnostics"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/diagnostics
rm -rf "$out"; mkdir -p "$out"

# 1. A parse error at a known column (the missing ';' is before `return`, col 5).
cat > "$out/miss.c" << 'EOF'
int main(void)
{
    int x = 5
    return x;
}
EOF
err=$("$EMBCC" -c "$out/miss.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q "$out/miss.c:4:5: error:" || { echo "no located error at 4:5:"; echo "$err"; exit 1; }
echo "$err" | grep -q 'return x;'                || { echo "source line not shown:"; echo "$err"; exit 1; }
echo "$err" | grep -qE '^ +\^$'                  || { echo "no caret line:"; echo "$err"; exit 1; }
echo "located error: file:line:col + source line + caret at the column"

# 2. An error inside a header is located to the HEADER, showing the header's own
#    line — the source registry spans #includes.
printf 'int bad = ;\n' > "$out/h.h"
printf '#include "h.h"\nint main(void){return 0;}\n' > "$out/u.c"
err=$("$EMBCC" -c "$out/u.c" -I"$out" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q "$out/h.h:1:"  || { echo "header error not located to the header:"; echo "$err"; exit 1; }
echo "$err" | grep -q 'int bad = ;'  || { echo "header source line not shown:"; echo "$err"; exit 1; }
echo "header diagnostics show the header's own source line"

# 3. Colour is gated on a terminal: piped output (as here) carries no ESC bytes.
if printf '%s' "$err" | grep -q "$(printf '\033')"; then
    echo "escape codes leaked into non-terminal output"; exit 1
fi
echo "no colour codes when stderr is not a terminal"

echo "diagnostics: caret + source line + column, across includes, TTY-gated colour"
