#!/bin/sh
# K1: the inline-asm instruction set the EmbLinkOS kernel needs (docs/todo.md).
# Most are privileged (cli/hlt/out/mov-cr/wrmsr/lidt/...) so they cannot RUN in
# userland; the check is that EmbCC emits the CORRECT machine bytes — objdump,
# the reference decoder, must reproduce each mnemonic and flag nothing (bad).
set -u
echo "TEST-MARKER inline-asm-kernel"

EMBCC=${EMBCC:-./embcc}
out=tests/golden/out/inline-asm-kernel
rm -rf "$out"; mkdir -p "$out"
src="$out/asm.c"

cat > "$src" <<'EOF'
typedef unsigned long u64; typedef unsigned int u32;
typedef unsigned short u16; typedef unsigned char u8;
void f_cli(void){ __asm__ volatile("cli"); }
void f_sti(void){ __asm__ volatile("sti; hlt"); }
void f_pause(void){ __asm__ volatile("pause"); }
void f_fence(void){ __asm__ volatile("mfence"); __asm__ volatile("lfence");
                    __asm__ volatile("sfence"); __asm__ volatile("wbinvd"); }
u64  f_rdtsc(void){ u32 lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi));
                    return ((u64)hi<<32)|lo; }
u64  f_rdmsr(u32 m){ u32 lo,hi; __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(m));
                     return ((u64)hi<<32)|lo; }
void f_wrmsr(u32 m,u64 v){ __asm__ volatile("wrmsr"::"c"(m),"a"((u32)v),
                                            "d"((u32)(v>>32))); }
u64  f_flags(void){ u64 f; __asm__ volatile("pushfq; pop %0":"=r"(f)); return f; }
void f_cr0w(u64 v){ __asm__ volatile("mov %0, %%cr0"::"r"(v)); }
u64  f_cr2r(void){ u64 v; __asm__ volatile("mov %%cr2, %0":"=r"(v)); return v; }
void f_cr3w(u64 v){ __asm__ volatile("mov %0, %%cr3"::"r"(v):"memory"); }
void f_outb(u16 p,u8 v){ __asm__ volatile("outb %0, %1"::"a"(v),"Nd"(p)); }
u8   f_inb(u16 p){ u8 r; __asm__ volatile("inb %1, %0":"=a"(r):"Nd"(p)); return r; }
void f_outl(u16 p,u32 v){ __asm__ volatile("outl %0, %1"::"a"(v),"Nd"(p)); }
void f_invlpg(void*a){ __asm__ volatile("invlpg (%0)"::"r"(a):"memory"); }
void f_lidt(void*p){ __asm__ volatile("lidt %0"::"m"(*(char*)p)); }
void f_lgdt(void*p){ __asm__ volatile("lgdt %0"::"m"(*(char*)p)); }
u16  f_str(void){ u16 t; __asm__ volatile("str %0":"=r"(t)); return t; }
void f_ltr(u16 s){ __asm__ volatile("ltr %0"::"r"(s):"memory"); }
void f_store(void*d){ __asm__ volatile("movdqa %%xmm0, (%0)"::"r"(d):"memory"); }
void f_load(const void*s){ __asm__ volatile("movdqa (%0), %%xmm0"::"r"(s):"memory"); }
/* a multi-instruction, newline-separated block: every instruction must be
 * assembled (not truncated after the first), with reg->named, imm->named,
 * push imm, and iretq. */
void f_enter(u64 e,u64 sp,u64 a){
    __asm__ volatile("movq %2, %%rdi\n" "mov $1, %%rax\n"
                     "pushq %0\n" "pushq $0x202\n" "pushq %1\n" "iretq\n"
                     :: "r"(sp),"r"(e),"r"(a) : "rdi","rax","memory"); }
/* the gdt segment-reload trampoline: a local-label RIP-relative leaq,
 * lretq, a 16-bit immediate load and segment-register moves. */
void f_reload(void){
    __asm__ volatile("pushq $0x08\n" "leaq 1f(%%rip), %%rax\n" "pushq %%rax\n"
                     "lretq\n" "1:\n" "mov $0x10, %%ax\n" "mov %%ax, %%ds\n"
                     "mov %%ax, %%es\n" "mov %%ax, %%ss\n" ::: "rax","memory"); }
/* named `%[operand]`s with the "i" (immediate) constraint: EmbCC computes
 * the value/address into a register, so `movabs %[m]` becomes a reg move —
 * correct, if not gcc's exact encoding — and the syscall issues. */
long f_syscall(void){
    static const char m[] = "hi\n"; long r;
    __asm__ volatile("mov $1, %%rax\n" "movabs %[b], %%rsi\n"
                     "mov %[n], %%rdx\n" "int $0x80\n"
                     : "=a"(r) : [b]"i"(&m[0]), [n]"i"(sizeof m - 1)
                     : "rsi","rdx","rcx","r11","memory");
    return r; }
int main(void){ return 42; }
EOF

"$EMBCC" -c "$src" -o "$out/asm.o" || { echo "embcc failed to assemble"; exit 1; }

dis=$(objdump -d "$out/asm.o" 2>/dev/null)

# nothing must decode to (bad)
if printf '%s\n' "$dis" | grep -q '(bad)'; then
    echo "an instruction decoded to (bad):"
    printf '%s\n' "$dis" | grep -B1 '(bad)'
    exit 1
fi

# every expected mnemonic must be present, exactly as the reference decodes it
for want in cli sti hlt pause mfence lfence sfence wbinvd rdtsc rdmsr wrmsr \
            pushf 'mov +%rax,%cr0' 'mov +%cr2,%rax' 'out +%al' 'in +.*%al' \
            invlpg lidt lgdt str ltr movdqa iretq 'mov +\$0x1,%rax' \
            lretq 'lea +0x3\(%rip\)' 'mov +\$0x10,%ax' 'mov +%eax,%ds' \
            'int +\$0x80'; do
    printf '%s\n' "$dis" | grep -Eq "$want" || {
        echo "missing expected instruction: $want"; exit 1; }
done

echo "all kernel inline-asm instructions assembled and decode cleanly"
echo "inline-asm-kernel acceptance passed"
