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

# File-scope asm: crt0's _start stub — a .global label, and/call/jmp, and a
# PLT32 relocation to the C entry it calls.
cat > "$out/start.c" <<'CEOF'
extern void start_c(void);
__asm__(
    ".global _start\n"
    "_start:\n"
    "    and $-16, %rsp\n"
    "    call start_c\n"
    "1:  jmp 1b\n"
);
void start_c(void) {}
CEOF
"$EMBCC" -c "$out/start.c" -o "$out/start.o" || { echo "embcc failed on _start"; exit 1; }
sd=$(objdump -d "$out/start.o" 2>/dev/null); st=$(objdump -t "$out/start.o" 2>/dev/null)
sr=$(objdump -r "$out/start.o" 2>/dev/null)
f2=0
echo "$st" | grep -qE 'g +F .text.*_start'  || { echo "MISSING: _start global func symbol"; f2=1; }
echo "$sd" | grep -q '48 83 e4 f0'          || { echo "MISSING: and \$-16,%rsp (48 83 e4 f0)"; f2=1; }
echo "$sd" | grep -q 'e8 00 00 00 00'       || { echo "MISSING: call rel32 (e8 00000000)"; f2=1; }
echo "$sr" | grep -qE 'R_X86_64_PLT32 +start_c' || { echo "MISSING: PLT32 to start_c"; f2=1; }
echo "$sd" | grep -q 'e9 fb ff ff ff'       || { echo "MISSING: jmp 1b (e9 fb ff ff ff)"; f2=1; }
[ "$f2" -eq 0 ] && echo "file-scope asm: _start stub correct" || exit 1

# REGRESSION: a STATIC function reachable ONLY through file-scope asm must NOT
# be dead-code-eliminated. crt0's start_c is static and called solely from
# _start's `call start_c`; irgen prunes unused statics, and the asm reference
# wasn't counted as a use until AFTER irgen -- so start_c's body was never
# generated and the reloc bound to a value-0 placeholder that aliased the
# function at .text offset 0 (a syscall stub). On the OS the EmbCC rim then
# called the wrong function and hung on a bogus SYS_write. The fix marks
# asm-call targets used BEFORE irgen. Discriminator: the target's symbol must
# have a NON-ZERO size (a real emitted body), not the 0/0 placeholder.
cat > "$out/dce.c" <<'CEOF'
static long sys1(long n){ long r;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n):"memory"); return r; }
static void real_entry(void){ (void)sys1(60); }
__asm__(
    ".global _start\n"
    "_start:\n"
    "    call real_entry\n"
    "1:  jmp 1b\n"
);
CEOF
"$EMBCC" -c "$out/dce.c" -o "$out/dce.o" || { echo "embcc failed on dce"; exit 1; }
dt=$(objdump -t "$out/dce.o" 2>/dev/null)
f4=0
echo "$dt" | grep -qE 'F \.text.* real_entry$' || { echo "MISSING: real_entry emitted as a .text FUNC (DCE'd?)"; f4=1; }
rsz=$(echo "$dt" | awk '/[[:space:]]real_entry$/{print $(NF-1)}')
[ "$rsz" != "0000000000000000" ] && [ -n "$rsz" ] || { echo "BUG: real_entry has zero size ($rsz) -- body not generated, aliases .text offset 0"; f4=1; }
[ "$f4" -eq 0 ] && echo "file-scope asm: static asm-only callee survives DCE (size=$rsz)" || exit 1

# Advanced inline asm: cpuid (fixed a/b/c/d), and rdrand/setc with %-operand
# substitution + general 'r'/'qm' register allocation (syscalls.c's RNG path).
cat > "$out/adv.c" <<'CEOF'
typedef unsigned int u32;
typedef unsigned long u64;
void cid(u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a),"=b"(*b),"=c"(*c),"=d"(*d)
                             : "a"(1u),"c"(0u));
}
int rd(u64 *o) { unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*o),"=qm"(ok) :: "cc");
    return ok;
}
CEOF
"$EMBCC" -c "$out/adv.c" -o "$out/adv.o" || { echo "embcc failed on adv asm"; exit 1; }
ad=$(objdump -d "$out/adv.o" 2>/dev/null)
f3=0
echo "$ad" | grep -q '0f a2'          || { echo "MISSING: cpuid (0f a2)"; f3=1; }
echo "$ad" | grep -q '48 0f c7 f0'    || { echo "MISSING: rdrand %rax (48 0f c7 f0)"; f3=1; }
echo "$ad" | grep -q '0f 92 c1'       || { echo "MISSING: setc %cl (0f 92 c1)"; f3=1; }
[ "$f3" -eq 0 ] && echo "advanced asm: cpuid/rdrand/setc + %-subst correct" || exit 1
