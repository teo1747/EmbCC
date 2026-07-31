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
echo "$err" | grep -qE '^ +\^~*$'                  || { echo "no caret line:"; echo "$err"; exit 1; }
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

# 4. A semantic error points at the expression (a column on AST nodes): a member
#    access on a non-struct carries a caret under the base.
printf 'int main(void){int x=3; return x.y;}\n' > "$out/sema.c"
err=$("$EMBCC" -c "$out/sema.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -qE "$out/sema.c:1:[0-9]+: error:" || { echo "sema error not located to a column:"; echo "$err"; exit 1; }
echo "$err" | grep -qE '^ +\^~*$'                       || { echo "no caret on the semantic error:"; echo "$err"; exit 1; }
echo "semantic errors carry a caret at the expression"

# 5. A redefinition prints the error AND a note at the previous definition, each
#    with its own source line.
printf 'int g = 1;\nint g = 2;\nint main(void){return g;}\n' > "$out/redef.c"
err=$("$EMBCC" -c "$out/redef.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q "redef.c:2: error: redefinition of 'g'"         || { echo "no redefinition error:"; echo "$err"; exit 1; }
echo "$err" | grep -q "redef.c:1: note: previous definition of 'g'"   || { echo "no previous-definition note:"; echo "$err"; exit 1; }
echo "$err" | grep -q 'int g = 1;'                                    || { echo "note's source line not shown:"; echo "$err"; exit 1; }
echo "redefinition: error + note, each with its source line"

# 6. A typo'd name suggests the nearest in-scope symbol; an unrelated name does
#    not (the edit-distance threshold scales with length).
printf 'int counter = 0;\nint main(void){ return countr; }\n' > "$out/typo.c"
err=$("$EMBCC" -c "$out/typo.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q "note: did you mean 'counter'?" || { echo "no did-you-mean suggestion:"; echo "$err"; exit 1; }
printf 'int main(void){ return zzzzzz; }\n' > "$out/nohint.c"
err=$("$EMBCC" -c "$out/nohint.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q 'did you mean' && { echo "bogus suggestion for an unrelated name:"; echo "$err"; exit 1; }
echo "did-you-mean: nearest symbol suggested, no false positives"

# 7. The caret underlines the whole offending identifier (^~~~).
printf 'int counter=0;\nint main(void){ return counterX; }\n' > "$out/span.c"
err=$("$EMBCC" -c "$out/span.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -qE '^ +\^~+$' || { echo "identifier not underlined:"; echo "$err"; exit 1; }
echo "the caret underlines the whole token (^~~~)"

# 8. An error inside a macro expansion is attributed to the macro; an unrelated
#    error merely sharing a line with a macro is NOT (column-precise).
printf '#define BAD undefined_thing\nint main(void){ return BAD; }\n' > "$out/mac.c"
err=$("$EMBCC" -c "$out/mac.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q "note: expanded from macro 'BAD'" || { echo "no expanded-from-macro note:"; echo "$err"; exit 1; }
printf '#define OK 1\nint main(void){ int a = OK; return zzz; }\n' > "$out/mac2.c"
err=$("$EMBCC" -c "$out/mac2.c" -o "$out/x.o" 2>&1 || true)
echo "$err" | grep -q 'expanded from macro' && { echo "misattributed an unrelated error to a macro:"; echo "$err"; exit 1; }
echo "expanded-from-macro note: attributed precisely, no misattribution"

echo "diagnostics: caret+underline + notes + suggestions + macro attribution, TTY-gated colour"
