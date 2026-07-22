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
check do-while \
    'int main(void) { int i = 0; do { i++; } while (i < 3); return 0; }' \
    "not supported"
check ternary \
    'int main(void) { return 1 ? 42 : 0; }' \
    "not supported"
check break-outside-loop \
    'int main(void) { break; return 0; }' \
    "outside of a loop"
check deref-non-pointer \
    'int main(void) { int x = 1; return *x; }' \
    "cannot dereference int"
check deref-void-ptr \
    'int main(void) { void *p = 0; return *p; }' \
    "cannot dereference void"
check ptr-plus-ptr \
    'int main(void) { int x; int *a = &x; int *b = &x; return !(a + b); }' \
    "cannot add two pointers"
check int-to-ptr-implicit \
    'int main(void) { int *p = 42; return !p; }' \
    "without a cast"
check ptr-int-compare \
    'int main(void) { int x; int *p = &x; return p == 42; }' \
    "needs a cast"
check incompatible-ptr-assign \
    'int main(void) { int x; char *p = &x; return !p; }' \
    "without a cast"
check compound-through-pointer \
    'int main(void) { int x = 1; int *p = &x; *p += 1; return x; }' \
    "compound assignment through a pointer"
check addr-of-rvalue \
    'int main(void) { int x = 1; return !&(x + 1); }' \
    "needs a variable"
check function-pointer \
    'static int f(void) { return 1; }
int main(void) { return !&f; }' \
    "function pointers are not supported"
check void-variable \
    'int main(void) { void v; return 0; }' \
    "cannot have type void"
check preprocessor \
    '#include <stdio.h>
int main(void) { return 0; }' \
    "preprocessor"
check float-type \
    'float main(void) { return 0; }' \
    "not supported"
check undefined-call \
    'int main(void) { return foo(); }' \
    "not declared"
check forward-call \
    'static int a(void) { return b(); }
static int b(void) { return 1; }
int main(void) { return a(); }' \
    "before its declaration"
check proto-arity-mismatch \
    'int f(int, int);
int f(int a) { return a; }
int main(void) { return f(1); }' \
    "conflicting declaration"
check proto-type-mismatch \
    'int f(int x);
char *f(int x) { return 0; }
int main(void) { return !f(1); }' \
    "conflicting declaration"
check static-never-defined \
    'static int ghost(void);
int main(void) { return ghost(); }' \
    "called but never defined"
check nonstatic-then-static \
    'int f(void);
static int f(void) { return 1; }
int main(void) { return f(); }' \
    "static declaration of 'f' follows non-static"
check unnamed-param-in-definition \
    'int f(int) { return 1; }
int main(void) { return f(1); }' \
    "needs a name in a definition"
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
