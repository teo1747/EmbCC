#!/bin/sh
# Test runner (ARCHITECTURE.md §7).
#
# Two kinds of test:
#   *.sh under tests/{exec,compile,golden}/ — run with $EMBCC set; passes
#     when it exits 0 AND its output contains "TEST-MARKER <name>". The
#     marker catches CONTRIBUTING's lie #2 (a test that silently never
#     ran).
#   *.c under tests/exec/ — compiled by embcc, linked with the host cc,
#     and RUN; the exit code must equal the '// expect-exit: N' line in
#     the file. These are the tests that count: they assert the machine
#     ran what we emitted. Artifacts are rebuilt from scratch every run
#     (lie #1: the stale binary).
set -u

cd "$(dirname "$0")/.."
EMBCC="$PWD/embcc"
export EMBCC

# Turn on the optimizer's IR verifier for the whole suite: every -O1/-O2 compile
# (optimizer.sh, regalloc-O2.sh, the on-metal kernel build) then checks that no
# pass dropped a live value or left var_scope indices stale, and aborts loudly if
# so. -O0 skips the optimizer entirely, so this is free there. It exists because
# the var_scope-after-DCE miscompile passed the whole suite once (see
# tests/exec/scope-dce-shift.c) — this makes that class a hard failure, not a
# boot-the-kernel-to-find-out.
export EMBCC_VERIFY=1

if [ ! -x "$EMBCC" ]; then
    echo "run.sh: $EMBCC not built (run make first)" >&2
    exit 1
fi

pass=0
fail=0

ok()   { echo "PASS $1"; pass=$((pass + 1)); }
bad()  { echo "FAIL $1 ($2)"; [ -n "$3" ] && printf '%s\n' "$3" | sed 's/^/     | /'; fail=$((fail + 1)); }

for t in tests/exec/*.sh tests/compile/*.sh tests/golden/*.sh; do
    [ -e "$t" ] || continue
    name=$(basename "$t" .sh)
    out=$(sh "$t" 2>&1)
    status=$?
    if [ $status -eq 0 ] && printf '%s\n' "$out" | grep -q "TEST-MARKER $name"; then
        ok "$t"
    elif [ $status -eq 0 ]; then
        bad "$t" "exit 0 but marker 'TEST-MARKER $name' missing — did it run?" "$out"
    else
        bad "$t" "exit $status" "$out"
    fi
done

for c in tests/exec/*.c; do
    [ -e "$c" ] || continue
    name=$(basename "$c" .c)
    expect=$(sed -n 's|.*// expect-exit: *\([0-9][0-9]*\).*|\1|p' "$c" | head -1)
    if [ -z "$expect" ]; then
        bad "$c" "no '// expect-exit: N' line — cannot assert anything" ""
        continue
    fi
    out_dir="tests/exec/out"
    mkdir -p "$out_dir"
    obj="$out_dir/$name.o"
    exe="$out_dir/$name"
    rm -f "$obj" "$exe"
    if ! msg=$("$EMBCC" -c "$c" -o "$obj" 2>&1); then
        bad "$c" "embcc failed" "$msg"
        continue
    fi
    if ! msg=$(cc -no-pie -o "$exe" "$obj" 2>&1); then
        bad "$c" "host link failed" "$msg"
        continue
    fi
    "$exe" >/dev/null 2>&1 # golden/agrees-with-gcc.sh diffs the output
    got=$?
    if [ "$got" -eq "$expect" ]; then
        ok "$c (exit $got)"
    else
        bad "$c" "exit $got, expected $expect" ""
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
