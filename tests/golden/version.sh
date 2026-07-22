#!/bin/sh
# ROADMAP M0: "a --version that prints something honest". At M1 honest
# means naming the subset AND what is still missing (preprocessor,
# linker) — the version must never claim more than the milestone holds.
set -u
echo "TEST-MARKER version"

out=$("$EMBCC" --version) || { echo "--version exited nonzero"; exit 1; }
echo "$out"

echo "$out" | grep -q "EmbCC"       || { echo "missing project name"; exit 1; }
echo "$out" | grep -q "x86_64-elf"  || { echo "missing target"; exit 1; }
echo "$out" | grep -q "C subset"    || { echo "does not name the subset"; exit 1; }
echo "$out" | grep -qi "preprocessor" || {
    echo "does not mention the preprocessor"; exit 1; }
echo "$out" | grep -qi "no linker" || {
    echo "does not admit the missing linker"; exit 1; }
