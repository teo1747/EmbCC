#!/bin/sh
# ROADMAP M0: "a --version that prints something honest". Honest at this
# stage means it names the target AND says nothing compiles yet.
set -u
echo "TEST-MARKER version"

out=$("$EMBCC" --version) || { echo "--version exited nonzero"; exit 1; }
echo "$out"

echo "$out" | grep -q "EmbCC"       || { echo "missing project name"; exit 1; }
echo "$out" | grep -q "x86_64-elf"  || { echo "missing target"; exit 1; }
echo "$out" | grep -qi "nothing compiles" || {
    echo "version claims more than M0 delivers"; exit 1; }
