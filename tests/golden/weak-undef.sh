#!/bin/sh
# A weak reference to an EXTERNAL symbol (declared weak, defined elsewhere or
# nowhere) must be emitted as a WEAK undefined symbol (nm 'w'), not a global
# undefined one ('U'). The linker resolves an unresolved weak to 0, so code
# guarded by `if (sym) sym();` links and runs even when the definition is
# absent -- exactly how emlibc decouples an optional stdio flush from stdlib.
set -u
echo "TEST-MARKER weak-undef"
out=tests/golden/out/weak-undef
rm -rf "$out"; mkdir -p "$out"

cat > "$out/w.c" << 'EOF'
extern void flush_all(void) __attribute__((weak));
void go(void) { if (flush_all) flush_all(); }
EOF
"$EMBCC" -c "$out/w.c" -o "$out/w.o" || { echo "embcc failed"; exit 1; }

fail=0
# the weak-undef function symbol is 'w' (lowercase), never 'U'
nm "$out/w.o" | grep -qE '^ *w +flush_all$' || { echo "flush_all is not a weak undef (nm):"; nm "$out/w.o" | grep flush_all; fail=1; }
nm "$out/w.o" | grep -qE '^ *U +flush_all$' && { echo "flush_all wrongly emitted as GLOBAL undef 'U'"; fail=1; }

# a NON-weak external stays a global undef 'U', so we did not weaken everything
cat > "$out/n.c" << 'EOF'
extern void other(void);
void call(void) { other(); }
EOF
"$EMBCC" -c "$out/n.c" -o "$out/n.o" || { echo "embcc failed (n)"; exit 1; }
nm "$out/n.o" | grep -qE '^ *U +other$' || { echo "non-weak extern should be 'U':"; nm "$out/n.o" | grep other; fail=1; }

[ "$fail" -eq 0 ] && echo "weak-undef: __attribute__((weak)) extern -> weak undef 'w'; plain extern stays 'U'" || exit 1
echo "weak-undef golden passed"
