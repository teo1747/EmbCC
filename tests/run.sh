#!/bin/sh
# Test runner (ROADMAP M0, ARCHITECTURE.md §7).
#
# A test is an executable *.sh under tests/{exec,compile,golden}/, run with
# $EMBCC pointing at the freshly built binary. It passes when it exits 0
# AND its output contains its own marker line "TEST-MARKER <name>" —
# CONTRIBUTING's lie #2 (a test that silently never ran) is exactly what
# the marker check catches.
#
# exec/ additionally holds *.c programs compiled-and-run once a compiler
# exists (M1); the runner will grow that mode when there is something to
# compile with.
set -u

cd "$(dirname "$0")/.."
EMBCC="$PWD/embcc"
export EMBCC

if [ ! -x "$EMBCC" ]; then
    echo "run.sh: $EMBCC not built (run make first)" >&2
    exit 1
fi

pass=0
fail=0
for t in tests/exec/*.sh tests/compile/*.sh tests/golden/*.sh; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .sh)
    out=$(sh "$t" 2>&1)
    status=$?
    if [ $status -eq 0 ] && printf '%s\n' "$out" | grep -q "TEST-MARKER $name"; then
        echo "PASS $t"
        pass=$((pass + 1))
    else
        if [ $status -eq 0 ]; then
            echo "FAIL $t (exit 0 but marker 'TEST-MARKER $name' missing — did it run?)"
        else
            echo "FAIL $t (exit $status)"
        fi
        printf '%s\n' "$out" | sed 's/^/     | /'
        fail=$((fail + 1))
    fi
done

total=$((pass + fail))
if [ $total -eq 0 ]; then
    # Zero tests is a failure, not a green run: an empty suite proves nothing.
    echo "run.sh: no tests found" >&2
    exit 1
fi
echo "-----"
echo "$pass/$total passed"
[ $fail -eq 0 ]
