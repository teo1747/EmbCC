#!/bin/sh
# Extended inline asm with fixed-register constraints (VISION_LONGTERM: so
# EmbLinkOS's int-$0x80 syscall stubs compile and the __TINYC__ workaround
# can die). int $0x80 makes a REAL syscall, so it can't run on this Linux
# host; the object is checked instead — the assembled trap byte and the
# per-constraint register loads. The running proof is on the OS.
set -u
echo "TEST-MARKER inline-asm"
out=tests/golden/out/inline-asm
rm -rf "$out"; mkdir -p "$out"

cat > "$out/sys.c" <<'CEOF'
typedef long i64;
i64 sys6(i64 n, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    i64 ret;
    register i64 r10 __asm__("r10") = a4;
    register i64 r8  __asm__("r8")  = a5;
    register i64 r9  __asm__("r9")  = a6;
    __asm__ volatile ("int $0x80" : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}
CEOF
"$EMBCC" -c "$out/sys.c" -o "$out/sys.o" || { echo "embcc failed"; exit 1; }

d=$(objdump -d "$out/sys.o" 2>/dev/null)
fail=0
check() { echo "$d" | grep -qE "$1" || { echo "MISSING: $2 (/$1/)"; fail=1; }; }
check 'cd 80[[:space:]]+int' "the int \$0x80 trap byte"
check 'mov +-0x[0-9a-f]+\(%rbp\),%r10' "r10 loaded from its slot (REX)"
check 'mov +-0x[0-9a-f]+\(%rbp\),%r8'  "r8 loaded"
check 'mov +-0x[0-9a-f]+\(%rbp\),%r9'  "r9 loaded"
check 'mov +-0x[0-9a-f]+\(%rbp\),%rdi' "a1 -> rdi (D)"
check 'mov +-0x[0-9a-f]+\(%rbp\),%rsi' "a2 -> rsi (S)"
check 'mov +%rax,0x0\(%rcx\)'          "the =a result stored through the lvalue"
[ "$fail" -eq 0 ] && echo "inline-asm: syscall codegen correct" || exit 1
echo "inline-asm golden passed (running proof: on the OS)"
