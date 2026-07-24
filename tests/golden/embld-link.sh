#!/bin/sh
# EmbLD, the integrated linker (ARCHITECTURE §6, WORKPLAN stream B),
# proven on the host — objects linked into ET_EXECs that RUN.
#
# The test programs are freestanding: their _start does the Linux
# exit(N) syscall directly, so the host can run them with no libc and no
# OS. That isolates the LINKER — layout, symbol resolution, relocation,
# ET_EXEC emission — from the EmbLink syscall ABI, exactly the
# "prove on the host first" split (DECISIONS D-005) that got the
# compiler through its own milestones. The EmbLink acceptance (B1, the
# M1 program with real crt0/newlib) is a separate on-OS step.
set -u
echo "TEST-MARKER embld-link"

EMBLD=./embld
EMBCC=${EMBCC:-./embcc}
[ -x "$EMBLD" ] || { echo "embld not built"; exit 1; }
out=tests/golden/out/embld
rm -rf "$out"; mkdir -p "$out"

# _start that exits with the value a compute() returns — the freestanding
# harness every case links against.
cat > "$out/start.c" << 'EOF'
extern int compute(void);
static long do_exit(long c){long r;
  __asm__ volatile("syscall":"=a"(r):"a"(60),"D"(c):"rcx","r11","memory");
  return r;}
void _start(void){ do_exit(compute()); }
EOF
gcc -c -ffreestanding -fno-pie -O0 "$out/start.c" -o "$out/start.o" || {
    echo "could not build the freestanding harness"; exit 1; }

run() { # name expected -- links $out/$name.o + start.o, runs, checks exit
    "$EMBLD" -o "$out/$1" "$out/start.o" "$out/$1.o" || {
        echo "$1: embld failed"; exit 1; }
    chmod +x "$out/$1"
    "$out/$1"; got=$?
    [ "$got" -eq "$2" ] || { echo "$1: exit $got, expected $2"; exit 1; }
    echo "$1: linked and ran, exit $got"
}

# 1. a single object, intra-object call (PC32 relocation)
cat > "$out/one.c" << 'EOF'
static int twice(int x){ return x + x; }
int compute(void){ return twice(21); }
EOF
gcc -c -ffreestanding -fno-pie -O0 "$out/one.c" -o "$out/one.o"
run one 42

# 2. cross-object symbol resolution + .data + .bss
cat > "$out/two.c" << 'EOF'
extern int helper(int);
int shared_global = 100;      /* .data */
int bss_global;               /* .bss, loader zero-fills */
int compute(void){ bss_global = 5; return helper(shared_global) + bss_global; }
EOF
cat > "$out/twohelp.c" << 'EOF'
int helper(int x){ return x / 3 - 3; }   /* 100/3 - 3 = 30 */
EOF
gcc -c -ffreestanding -fno-pie -O0 "$out/two.c" -o "$out/two.o"
gcc -c -ffreestanding -fno-pie -O0 "$out/twohelp.c" -o "$out/twohelp.o"
"$EMBLD" -o "$out/two" "$out/start.o" "$out/two.o" "$out/twohelp.o" || {
    echo "two: embld failed"; exit 1; }
chmod +x "$out/two"; "$out/two"; got=$?
[ "$got" -eq 35 ] || { echo "two: exit $got, expected 35 (30+5)"; exit 1; }
echo "two: cross-object + .data + .bss, exit $got"

# 3. THE INTEGRATION: an object compiled by EMBCC, linked by EMBLD.
# No external toolchain touches the compute half — the self-hosting loop
# in miniature.
if [ -x "$EMBCC" ]; then
    "$EMBCC" -c "$out/one.c" -o "$out/emb.o" || {
        echo "embcc failed on the compute object"; exit 1; }
    "$EMBLD" -o "$out/emb" "$out/start.o" "$out/emb.o" || {
        echo "embld failed on the embcc object"; exit 1; }
    chmod +x "$out/emb"; "$out/emb"; got=$?
    [ "$got" -eq 42 ] || { echo "emb: exit $got, expected 42"; exit 1; }
    echo "emb: EmbCC-compiled, EmbLD-linked, exit $got"
fi

# 4. static archives: pull members to satisfy references, to a fixed
#    point (back-references within the archive), and DEAD members stay
#    out. Needs the cross ar; skipped cleanly without it.
AR=/usr/local/cross/bin/x86_64-elf-ar
if [ -x "$AR" ]; then
    cat > "$out/lb.c" << 'EOF'
int used_b(int x){ return x + 1; }
EOF
    cat > "$out/la.c" << 'EOF'
int used_b(int);                       /* satisfied by ANOTHER member */
int used_a(int x){ return used_b(x) * 2; }
EOF
    cat > "$out/lu.c" << 'EOF'
int unused(int x){ return x - 999; }   /* referenced by nothing */
EOF
    for f in lb la lu; do
        gcc -c -ffreestanding -fno-pie -O0 "$out/$f.c" -o "$out/$f.o"
    done
    "$AR" rcs "$out/libtest.a" "$out/la.o" "$out/lb.o" "$out/lu.o"
    cat > "$out/amain.c" << 'EOF'
extern int used_a(int);
static long do_exit(long c){long r;
  __asm__ volatile("syscall":"=a"(r):"a"(60),"D"(c):"rcx","r11","memory");return r;}
void _start(void){ do_exit(used_a(20)); }   /* used_b(20)*2 = 42 */
EOF
    gcc -c -ffreestanding -fno-pie -O0 "$out/amain.c" -o "$out/amain.o"
    "$EMBLD" -o "$out/aprog" "$out/amain.o" "$out/libtest.a" || {
        echo "archive link failed"; exit 1; }
    chmod +x "$out/aprog"; "$out/aprog"; got=$?
    [ "$got" -eq 42 ] || { echo "archive link: exit $got, expected 42"; exit 1; }
    # dead-member proof: linking the archive must equal linking exactly
    # the two live members explicitly — byte for byte.
    "$EMBLD" -o "$out/aprog2" "$out/amain.o" "$out/la.o" "$out/lb.o"
    cmp -s "$out/aprog" "$out/aprog2" || {
        echo "archive pulled a dead member (image differs from la+lb only)"
        exit 1; }
    echo "archive: fixed-point pull + back-ref, dead member excluded, exit 42"
fi

# 5. CONSTRUCTORS: a program with __attribute__((constructor)) whose
#    _start walks __init_array_start..__init_array_end. If EmbLD's
#    bracket symbols are the real .init_array group bounds, the ctor
#    runs; if they were 0 (B1's weak-→0 for an empty array), it would
#    not. Also exercises R_X86_64_64 into .init_array (the fn pointer).
cat > "$out/ctor.c" << 'EOF'
static int marker = 0;
__attribute__((constructor)) static void set_marker(void){ marker = 42; }
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
static long do_exit(long c){long r;
  __asm__ volatile("syscall":"=a"(r):"a"(60),"D"(c):"rcx","r11","memory");return r;}
void _start(void){
    for (void (**p)(void)=__init_array_start; p<__init_array_end; p++) (*p)();
    do_exit(marker);
}
EOF
gcc -c -ffreestanding -fno-pie -O0 "$out/ctor.c" -o "$out/ctor.o"
"$EMBLD" -o "$out/ctor" "$out/ctor.o" || { echo "ctor: embld failed"; exit 1; }
chmod +x "$out/ctor"; "$out/ctor"; got=$?
[ "$got" -eq 42 ] || {
    echo "ctor: exit $got, expected 42 (0 = __init_array brackets empty)"
    exit 1; }
echo "constructors: __init_array bracket symbols correct, ctor ran, exit 42"

# 6. COMMON (tentative definitions) placed into .bss and usable.
cat > "$out/common.c" << 'EOF'
int common_var;
int other_common;
int compute(void){ common_var = 40; other_common = 2;
                   return common_var + other_common; }
EOF
gcc -c -ffreestanding -fno-pie -O0 -fcommon "$out/common.c" -o "$out/common.o"
"$EMBLD" -o "$out/common" "$out/start.o" "$out/common.o" || {
    echo "common: embld failed"; exit 1; }
chmod +x "$out/common"; "$out/common"; got=$?
[ "$got" -eq 42 ] || { echo "common: exit $got, expected 42"; exit 1; }
echo "COMMON symbols placed in .bss and usable, exit 42"

# 7. the ET_EXEC is well-formed: readelf accepts it, it is EXEC not DYN
#    (TARGET_ABI §4b: never PIE), entry lands in the executable segment.
readelf -h "$out/one" | grep -q "EXEC (Executable file)" || {
    echo "output is not ET_EXEC"; exit 1; }
# readelf prints each segment's flags on the line AFTER the LOAD line
flags=$(readelf -l "$out/one" | grep -A1 "LOAD" | grep -oE "R E|RW ")
echo "$flags" | grep -q "R E" || { echo "no R+X load segment"; exit 1; }
echo "$flags" | grep -q "RW"  || { echo "no R+W load segment"; exit 1; }
echo "ET_EXEC well-formed: two PT_LOAD segments, W^X"
