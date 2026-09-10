#!/bin/sh
# Run a QEMU command with a hard timeout, portably.
#
# macOS ships no coreutils `timeout`, and `perl -e alarm; exec` does not
# work here: QEMU installs its own SIGALRM handler, so the alarm is
# swallowed and a hung guest hangs the whole suite. Backgrounding and
# killing is the only reliable form.
#
# usage: qrun.sh <seconds> <command> [args...]   -> the guest's exit status
timeout=$1; shift
"$@" & qpid=$!
( sleep "$timeout"; kill -9 "$qpid" 2>/dev/null ) & wpid=$!
wait "$qpid" 2>/dev/null; status=$?
kill -9 "$wpid" 2>/dev/null
wait "$wpid" 2>/dev/null
exit $status
