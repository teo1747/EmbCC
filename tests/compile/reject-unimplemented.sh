#!/bin/sh
# THE RULE, as a test: with no compiler present, feeding embcc a C file
# must FAIL loudly — nonzero exit and a diagnostic that says why. A quiet
# success here would be the exact "valid-looking artifact that is wrong"
# failure CONTRIBUTING warns about.
set -u
echo "TEST-MARKER reject-unimplemented"

out_dir="tests/compile/out"
mkdir -p "$out_dir"
src="$out_dir/hello.c"
echo 'int main(void) { return 42; }' > "$src"

if err=$("$EMBCC" "$src" 2>&1); then
    echo "embcc exited 0 on a .c file it cannot compile"
    exit 1
fi
echo "$err"
echo "$err" | grep -q "cannot compile" || {
    echo "failure was not diagnosed (no 'cannot compile' in stderr)"
    exit 1
}
