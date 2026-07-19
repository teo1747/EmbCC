#!/bin/sh
# THE RULE, as a test: everything outside the subset must FAIL with a
# diagnostic naming the construct — never compile to something else.
# Each case asserts nonzero exit AND a message, because a bare nonzero
# could be any failure. Cases graduate OUT of this file as milestones
# implement them (if/while/for left when control flow landed).
set -u
echo "TEST-MARKER reject-unimplemented"

out_dir="tests/compile/out"
mkdir -p "$out_dir"

check() { # name source expected-message-grep
    src="$out_dir/$1.c"
    printf '%s\n' "$2" > "$src"
    if err=$("$EMBCC" -c "$src" -o "$out_dir/$1.o" 2>&1); then
        echo "case $1: compiled instead of failing"
        exit 1
    fi
    echo "$err" | grep -q "$3" || {
        echo "case $1: wrong diagnostic:"
        echo "$err"
        exit 1
    }
    echo "$err" | grep -q "$src:" || {
        echo "case $1: diagnostic has no file:line position:"
        echo "$err"
        exit 1
    }
    echo "case $1: refused with a diagnostic"
}

check switch-stmt \
    'int main(void) { switch (1) { } return 0; }' \
    "not supported"
check break-stmt \
    'int main(void) { while (1) { break; } return 0; }' \
    "not supported"
check division \
    'int main(void) { return 84 / 2; }' \
    "not supported"
check preprocessor \
    '#include <stdio.h>
int main(void) { return 0; }' \
    "preprocessor"
check float-type \
    'float main(void) { return 0; }' \
    "not supported"
check pointer \
    'int main(void) { int *p; return 0; }' \
    "pointers are not supported"
check undefined-call \
    'int main(void) { return foo(); }' \
    "not defined in this file"
check forward-call \
    'static int a(void) { return b(); }
static int b(void) { return 1; }
int main(void) { return a(); }' \
    "before its definition"
check fallthrough \
    'int main(void) { int x = 1; }' \
    "must end in a return"
check fallthrough-if \
    'int main(void) { if (1) return 1; }' \
    "must end in a return"
check arity \
    'static int f(int a, int b) { return a + b; }
int main(void) { return f(1); }' \
    "takes 2 arguments, called with 1"
check string-literal \
    'int main(void) { return "x"; }' \
    "not supported"
check decl-as-if-body \
    'int main(void) { if (1) int x = 1; return 0; }' \
    "wrap it in braces"
check shadowing \
    'int main(void) { int x = 1; { int x = 2; } return x; }' \
    "already declared"
check assign-to-literal \
    'int main(void) { 5 = 6; return 0; }' \
    "assignment target must be a variable"

# And without -c: linking does not exist until M3.
printf 'int main(void) { return 0; }\n' > "$out_dir/nolink.c"
if err=$("$EMBCC" "$out_dir/nolink.c" 2>&1); then
    echo "case nolink: exited 0 but embcc cannot link"
    exit 1
fi
echo "$err" | grep -q "linker is M3" || {
    echo "case nolink: wrong diagnostic:"; echo "$err"; exit 1; }
echo "case nolink: refused with a diagnostic"
