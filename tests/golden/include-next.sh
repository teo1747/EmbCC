#!/bin/sh
# #include_next: a header wrapping the SYSTEM header of the same name,
# resuming the search after its OWN directory. EmbLinkOS does exactly
# this (user/lib/sys/stat.h wraps newlib's to add lstat), so it is not
# an exotic feature here — it is on the path to compiling the userland.
set -u
echo "TEST-MARKER include-next"

out_dir="tests/golden/out/incnext"
rm -rf "$out_dir"
mkdir -p "$out_dir/first/sys" "$out_dir/second/sys"

# the "system" header, found second
cat > "$out_dir/second/sys/thing.h" << 'EOF'
#define THING_BASE 40
EOF
# the wrapper, found first, pulling in the system one and adding to it
cat > "$out_dir/first/sys/thing.h" << 'EOF'
#ifndef WRAP_THING_H
#define WRAP_THING_H
#include_next <sys/thing.h>
#define THING_EXTRA 2
#endif
EOF
cat > "$out_dir/prog.c" << 'EOF'
#include <sys/thing.h>
int main(void) { return THING_BASE + THING_EXTRA; }
EOF

"$EMBCC" -c "$out_dir/prog.c" -o "$out_dir/prog.o" \
    -I "$out_dir/first" -I "$out_dir/second" || {
    echo "compile failed"; exit 1; }
cc -no-pie -o "$out_dir/prog" "$out_dir/prog.o" || {
    echo "link failed"; exit 1; }
"$out_dir/prog"
got=$?
[ "$got" -eq 42 ] || { echo "exit $got, expected 42 (wrapper+system)"; exit 1; }
echo "wrapper reached the system header via #include_next"

# From the PRIMARY source file, #include_next behaves like #include —
# there is no "current directory in the search path" to resume after.
# That is gcc's documented behaviour and this matches it.
cat > "$out_dir/frommain.c" << 'EOF'
#include_next <sys/thing.h>
int main(void) { return THING_BASE + 2; }
EOF
"$EMBCC" -c "$out_dir/frommain.c" -o "$out_dir/frommain.o" \
    -I "$out_dir/second" || {
    echo "#include_next from the main file should act like #include"
    exit 1; }
echo "from the primary file: behaves like #include (as gcc does)"

# The honest failure: a header in the LAST search directory has nothing
# after it, so its #include_next cannot be satisfied — refused, not
# silently skipped.
cat > "$out_dir/second/sys/tail.h" << 'EOF'
#include_next <sys/tail.h>
EOF
cat > "$out_dir/lonely.c" << 'EOF'
#include <sys/tail.h>
int main(void) { return 0; }
EOF
if "$EMBCC" -c "$out_dir/lonely.c" -o "$out_dir/lonely.o" \
       -I "$out_dir/second" 2>/dev/null; then
    echo "an unsatisfiable #include_next was accepted"
    exit 1
fi
"$EMBCC" -c "$out_dir/lonely.c" -o "$out_dir/lonely.o" \
    -I "$out_dir/second" 2>&1 | grep -q "NEXT include file" || {
    echo "unsatisfiable #include_next: wrong diagnostic"; exit 1; }
echo "unsatisfiable #include_next: refused"
