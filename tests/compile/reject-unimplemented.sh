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

check case-outside-switch \
    'int main(void) { case 1: return 0; }' \
    "directly in its switch body"
check case-nested-in-block \
    'int main(void) { int x = 1; switch (x) { { case 1: return 1; } } return 0; }' \
    "directly in its switch body"
check duplicate-case \
    'int main(void) { int x = 1; switch (x) { case 2: break; case 2: break; } return 0; }' \
    "duplicate case label 2"
check two-defaults \
    'int main(void) { int x = 1; switch (x) { default: break; default: break; } return 0; }' \
    "only one .default."
check non-constant-case \
    'int main(void) { int x = 1; int y = 2; switch (x) { case y: break; } return 0; }' \
    "integer constant"
check switch-on-pointer \
    'int main(void) { int x; int *p = &x; switch (p) { case 1: break; } return 0; }' \
    "needs an integer"
check continue-in-switch-no-loop \
    'int main(void) { int x = 1; switch (x) { case 1: continue; } return 0; }' \
    "outside of a loop"
check cond-incompatible \
    'int main(void) { int x; int *p = &x; return 1 ? p : 5; }' \
    "incompatible"
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
check compound-on-rvalue \
    'int main(void) { int x = 1; (x + 1) += 2; return x; }' \
    "must be a variable, \*pointer, or member"
check compound-ptr-mul \
    'int main(void) { int a[4]; int *p = a; p *= 2; return !p; }' \
    "only += and -= apply to a pointer"
check addr-of-rvalue \
    'int main(void) { int x = 1; return !&(x + 1); }' \
    "needs a variable"
check assign-to-function \
    'static int f(void) { return 1; }
int main(void) { f = 0; return f(); }' \
    "cannot assign to a function"
check call-non-function \
    'int main(void) { int x = 1; return x(); }' \
    "called object is not a function"
check fp-type-mismatch \
    'static int f(int x) { return x; }
int main(void) { long (*fp)(int) = f; return 0; }' \
    "without a cast"
check void-variable \
    'int main(void) { void v; return 0; }' \
    "cannot have type void"
check array-assign \
    'int main(void) { int a[3]; int b[3]; a = b; return 0; }' \
    "cannot assign to an array"
check addr-of-array \
    'int main(void) { int a[3]; return !&a; }' \
    "already the address"
check array-scalar-init \
    'int main(void) { int a[3] = 0; return 0; }' \
    "brace initializer or a string"
check too-many-initializers \
    'int main(void) { int a[2] = {1,2,3}; return a[0]; }' \
    "3 initializers for an array of 2"
check designated-init \
    'struct P { int x; int y; };
int main(void) { struct P p = { .x = 1 }; return p.x; }' \
    "designated initializers"
check non-char-array-from-string \
    'int main(void) { int a[4] = "abc"; return a[0]; }' \
    "only a char array"
check adjacent-strings \
    'int puts(char *);
int main(void) { puts("a" "b"); return 0; }' \
    "concatenation is not supported"
check varargs-definition \
    'int f(int a, ...) { return a; }
int main(void) { return f(1); }' \
    "variadic function is not supported"
check member-dot-on-int \
    'int main(void) { int x = 1; return x.y; }' \
    "needs a struct/union, got int"
check varargs-too-few \
    'int printf(char *fmt, ...);
int main(void) { printf(); return 0; }' \
    "needs at least 1 argument"
check global-conflicting-types \
    'int g;
long g;
int main(void) { return g; }' \
    "conflicting types"
check global-two-inits \
    'int g = 1;
int g = 2;
int main(void) { return g; }' \
    "redefinition"
check global-nonconst-init \
    'int a = 1;
int b = a;
int main(void) { return b; }' \
    "must be an integer literal"
check extern-with-init \
    'extern int g = 5;
int main(void) { return g; }' \
    "'extern' with an initializer"
check ptr-global-bad-init \
    'int *p = 42;
int main(void) { return !p; }' \
    "initialized to 0"
check global-use-before-decl \
    'int main(void) { return g; }
int g = 42;' \
    "used before its declaration"
check global-vs-function \
    'static int f(void) { return 1; }
int f;
int main(void) { return f(); }' \
    "both a function and a variable"
check unterminated-cond \
    '#ifdef NEVER
int main(void) { return 0; }' \
    "unterminated conditional"
check error-directive \
    '#error deliberately broken
int main(void) { return 0; }' \
    "deliberately broken"
check missing-include \
    '#include "no/such/file.h"
int main(void) { return 0; }' \
    "cannot find include"
check struct-scalar-init \
    'struct P { int x; };
int main(void) { struct P p = 1; return p.x; }' \
    "cannot convert"
check struct-assign-mismatch \
    'struct P { int x; };
struct Q { int x; };
int main(void) { struct P a; struct Q b; b.x = 1; a = b; return a.x; }' \
    "cannot assign"
check struct-arg-mismatch \
    'struct P { int x; };
struct Q { int x; };
static int f(struct P p) { return p.x; }
int main(void) { struct Q q; q.x = 1; return f(q); }' \
    "cannot convert"
check incomplete-var \
    'struct Later;
int main(void) { struct Later v; return 0; }' \
    "incomplete type"
check unknown-member \
    'struct P { int x; };
int main(void) { struct P p; p.x = 1; return p.z; }' \
    "no member 'z'"
check dot-on-pointer \
    'struct P { int x; };
int main(void) { struct P p; struct P *q = &p; return q.x; }' \
    "use '->' through a pointer"
check arrow-on-struct \
    'struct P { int x; };
int main(void) { struct P p; return p->x; }' \
    "needs a pointer"
check tag-redefinition \
    'struct P { int x; };
struct P { int y; };
int main(void) { return 0; }' \
    "redefinition of 'P'"
check block-scope-struct \
    'int main(void) { struct L { int x; }; return 0; }' \
    "file scope"
check bitfield \
    'struct B { int f : 3; };
int main(void) { return 0; }' \
    "before ':'"
check empty-struct \
    'struct E { };
int main(void) { return 0; }' \
    "at least one member"
check typedef-redef \
    'typedef int T;
typedef long T;
int main(void) { T v = 1; return (int)v; }' \
    "redefinition of typedef"
check angle-include-no-path \
    '#include <stdio.h>
int main(void) { return 0; }' \
    "cannot find include file"
check u64-to-double \
    'int main(void) { unsigned long u = 1; double d = u; return (int)d; }' \
    "unsigned 64-bit conversion"
check float-modulo \
    'int main(void) { double a = 5.0; double b = 2.0; return (int)(a % b); }' \
    "needs an integer"
check float-bitand \
    'int main(void) { double a = 5.0; return (int)(a & 1); }' \
    "needs an integer"
check float-to-pointer \
    'int main(void) { double d = 1.0; int *p = (int *)d; return !p; }' \
    "cannot convert between"
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
    "needs 2 arguments, got 1"
check string-as-int \
    'int main(void) { return "x"; }' \
    "converting char \* to int"
check decl-as-if-body \
    'int main(void) { if (1) int x = 1; return 0; }' \
    "wrap it in braces"
check redeclare-same-block \
    'int main(void) { int x = 1; int x = 2; return x; }' \
    "already declared in this block"
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
