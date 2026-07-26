# TODO — completeness gaps (evidence-backed corpus audit)

*How complete is EmbCC? It self-hosts and compiles all of fdlibm + emlibc, so the
architecture is done — what's left is a short, concrete feature list. This list
is ranked by a corpus run of `embcc -c` over TinyCC 0.9.27 (24 files) and newlib
libc string/stdlib/stdio (~500 files), with header-integration noise separated
from real language gaps.*

*Repro (host), from the EmbCC dir:*

```
NL="/home/motsou/cross/newlib-c99/x86_64-elf/include"
./embcc -c FILE.c -I include -I "$NL" -o /tmp/x.o
```

---

## Kernel self-host — the next corpus (the hardest yet)

*New goal: EmbCC compiles the EmbLinkOS **kernel** (so the OS can rebuild itself
entirely, not just its userland). The kernel is freestanding, higher-half,
hardware C — a materially harder corpus than TinyCC/newlib. Repro from `myos/`:*

```
for f in $(find kernel -name '*.c'); do /home/motsou/EmbCC/embcc -c "$f" -Ikernel -o /tmp/x.o; done
```

***89 / 89 kernel TUs compile clean (was 22) — the whole kernel builds under
EmbCC.*** K1–K9 and every one-off below are closed. No kernel C was changed.

### K1 — inline-asm assembler — DONE
EmbCC's extended-asm assembler (irgen.c `asm_assemble`) grew from
`int`/`cpuid`/`rdrand`/`setc` to the kernel's hardware vocabulary, every
encoding byte-verified against objdump: fixed-form (`cli`/`sti`/`hlt`/`nop`/
`pause`/`mfence`/`lfence`/`sfence`/`wbinvd`/`rdtsc`/`rdmsr`/`wrmsr`/`fninit`/
`pushfq`/`popfq`); port I/O (`outb`/`w`/`l`, `inb`/`w`/`l`); reg operands
(`pop`/`push`/`popq`/`pushq` `%N`, `str`/`ltr` `%N`); control registers
(`mov %reg,%%crN` / `mov %%crN,%reg`); memory operands (`lgdt`/`lidt %N`,
`invlpg (%N)`, `movdqa` ↔ `%%xmm0`). Constraint grammar already covered
`"=r"`/`"r"`/`"=a"`/`"a"`/`"=m"`/`"m"`/`"N"`/`"x"`, `"memory"`/`"cc"` clobbers,
`%N` refs, and `volatile`. Small parse helpers read the operand forms; unknown
mnemonics still refuse loudly. Golden test: tests/golden/inline-asm-kernel.sh.

### One-offs — ALL DONE
The ten distinct remaining gaps, each closed and gcc-verified:
- GCC **statement expressions** `({ ... })` — EXPR_STMTEXPR (selftests.c).
- GNU **array-range designators** `[lo ... hi] = v` (font_8x16.c).
- **`__attribute__((noreturn))`** honored in the return-path check (syscall.c).
- **`__builtin_va_list`** accepted as `char *` (kprintf.c).
- **`&global` at a constant offset** in a static initializer — `&arr[i]`,
  `&g.field`, `p+n` — via a recursive resolve_addr filling greloc's addend
  (keyboard.c).
- **`sizeof(EXPR)`** folded in an ICE when the type is resolvable
  (`sizeof(((T*)0)->f)`, embkfs.c) and **`sizeof(local/global var)`** (fd.c).
- freestanding **`<string.h>`** (fd.c); **char\*/unsigned char\*** signedness.
- inline asm the three low-level TUs need: `mov` reg/imm↔GPR and segment
  registers, `pushq $imm`, `iretq`/`lretq`, a local-label RIP-relative `leaq`
  (the gdt trampoline), and named `%[operand]`s with the "i" constraint
  (process.c, gdt.c, usermode.c). **CRITICAL fix along the way:** the
  per-instruction operand-skip stopped only at `;`, silently dropping every
  instruction after the first in a `\n`-separated template — now stops at `\n`.

### K2 — GCC builtins — DONE
`__builtin_bswap16/32/64` (IR_BSWAP), `__sync_synchronize` (mfence),
`__builtin_unreachable` (ud2 + noreturn in the return-path check),
`__builtin_expect` (becomes its first arg), `__atomic_load_n`/`store_n`
(load / store+mfence) and `__atomic_exchange_n` (locked `xchg`, IR_XCHG).
`__builtin_memcpy/memset` already resolved to the libc names. Verified vs gcc
at -O0 and -O1.

### K3 — `sizeof` / `offsetof` as an integer-constant-expression — DONE
`sizeof(type)` already folded; the gap was `offsetof`. Added
`__builtin_offsetof(type, designator)` folded to a size_t constant at parse
time (descending `.field`/`[index]`), and pointed `<stddef.h>`'s offsetof at
it — so `_Static_assert(offsetof(...) == N)` works.

### K4 — raise the parameter / argument cap — DONE
12 → 32. MAX_PARAMS moved to type.h so `struct type`'s `ptypes[]` grows with
it (the cap in ast.h alone left ptypes[12] to overflow — a heap smash on a
>12-param function type, found by ASAN).

### K5 — char/string escape sequences — DONE
A shared `scan_escape` now handles the full C set: `\a \b \f \v \?`, GNU `\e`,
hex `\xH...`, and octal `\NNN`, for both char and string literals.

### K6 — `&array` — DONE
`&arr` yields `T(*)[N]` whose value is the array's address (gen_addr already
produces it). *(The pointer-to-array `int (*p)[N]` declarator is a separate,
rarer gap, still open.)*

### K7 — use-before-declaration of a `static` function — DONE
The call and value paths test `seq <= cur_body_seq` (what the ordered-walk
`declared` flag encoded) so it also holds during static-initializer lowering —
a function pointer in a vtable resolves. This closed the "&func in a static
initializer" seam too: `greloc` grew an `ftarget` the driver relocates.

### K8 — `__attribute__` after a declarator — DONE
A trailing `__attribute__` on a local declarator is accepted (alignment not
yet honored on a stack slot).

### K9 — DONE
An empty translation unit yields a valid empty object.
*(A freestanding `<string.h>` on the path is an integration matter.)*

### K10 — `-mno-sse` codegen — **DONE** (kernel now compiles + links + boots + runs)

**RESOLVED (branch Teo).** EmbCC gained a `-mno-sse` mode (also spelled
`-mno-sse2` / `-mgeneral-regs-only`; `-mno-mmx` / `-mno-red-zone` / `-mno-80387`
/ `-mcmodel=` accepted as no-ops). Under it the varargs prologue skips the
`xmm0..7` register-save spill entirely, and any float op / `IR_I2F` / `IR_F2I`
/ `IR_F2F` is **refused loudly** (THE RULE — no silent SSE). Verified:
the `#UD` site `kprintf` disassembles to **0 SSE**; whole-kernel codegen under
`-mno-sse` emits **0 SSE** (the only 2 `movdqa` kernel-wide are the kernel's own
deliberate SSE-context-switch selftest inline asm in `process.c`, which runs
*after* `fpu_init` sets `CR4.OSFXSR`); integer varargs still run correctly;
default (no-flag) path is byte-identical (self-host holds, suite 87/87). The
2 `movdqa` in the original triage were miscounted as a struct-copy lowering —
they were always the kernel's own inline asm, not codegen.

<details><summary>original triage (kept for context)</summary>

*Empirically confirmed: all 88 kernel TUs compile, LINK with the kernel linker
script into a valid higher-half `EXEC` (entry `0xffffffff8037bde0`), and it BOOTS
and runs ring-0 higher-half code — so `-mcmodel=kernel` already works, no
relocation or code-model problems. It dies with a `#UD` → triple fault at the
first varargs call:*

```
v=06 (#UD) at kprintf+0x20:  f2 0f 11 85 ... movsd %xmm0,-0x130(%rbp)
```

The kernel is `-mno-sse -mno-mmx`, and SSE is not enabled in CR4 until `fpu_init`
runs — so any SSE instruction before that faults. EmbCC emits SSE where GCC (with
`-mno-sse`) does not. Whole-kernel disassembly shows this is **tiny and targeted
— 18 SSE instructions total, zero float math**:
- **16 `movsd`**: the System V varargs prologue spilling `xmm0..7` into the
  register-save area (in `kprintf` and one other varargs fn). Under `-mno-sse`
  this whole XMM save area is skipped (and callers must not set `AL`=xmm-count).
- **2 `movdqa`**: a 16-byte aligned move (a struct/`memcpy` lowering) — should use
  general-purpose `mov`s under `-mno-sse`.

**Fix: a `-mno-sse` mode** (a) no XMM spill in the varargs prologue, (b) never
lower struct copies / anything to SSE. That's the last thing between EmbCC and a
booting self-compiled kernel. (`-mno-red-zone` not yet exercised — the `#UD`
comes first; worth confirming once SSE is off, since the kernel takes interrupts.)

*Repro (from `myos/`): compile every `KERNEL_SRC` with `embcc -c … -Ikernel`,
link `x86_64-elf-ld -T kernel/linker.ld` with the nasm objects, boot.*

</details>

### K11 — honor `__attribute__((aligned(N)))` in LAYOUT — **DONE**

**RESOLVED (branch Teo).** `aligned(N)` is now applied to layout, not just
parsed: (a) a struct **member**'s offset rounds up to `N` and (b) the struct's
own align/size rise to a multiple of `N` (per-member `user_align` threads
through `ty_struct_layout`; it overrides `packed`, which only lowers the
default); (c) a **stack local** carrying the attribute gets its frame slot
rounded so its rbp-relative base is `N`-aligned (rbp is 16-aligned on entry, so
`N<=16` is honored; `N>16` would need dynamic stack realignment and is **refused
loudly** — THE RULE). Verified against the REAL kernel header: `struct thread`
(the `fxsave` target) lays out byte-identical to gcc — `fpu_state` at offset 80
(16-aligned) and `sizeof == 848` (16-multiple); the minimal repro static-asserts
pass; a local `observed[16] aligned(16)` lands on a 16-aligned slot; suite
87/87, self-host holds, kernel 89/89. This was the last thing between the
self-compiled kernel and the desktop.

<details><summary>original triage (kept for context)</summary>

*With `-mno-sse` in, the self-compiled kernel now boots much further — PMM (512 MB),
VMM direct map, ACPI, EMBKFS mounted, VFS at `/`, ksym loaded — then takes a
`#GP` at the **first context switch**:*

```
Vector 0x0D (#GP)  RIP 0xFFFFFFFF8037BC90 = kernel_ctx_switch:  fxsave (%rdx)
```

`fxsave` **#GPs unless its memory operand is 16-byte aligned**. The buffer is the
process struct's FPU save area:

```c
/* kernel/process/process.h:153 — the comment says "aligned(16) is load-bearing" */
unsigned char fpu_state[512] __attribute__((aligned(16)));
```

EmbCC **parses** `aligned(N)` (K8) but does not **apply** it to layout, so
`fpu_state` lands at a non-16 offset and `fxsave` faults. Minimal host repro —
this `_Static_assert` **fails** under embcc, passes under gcc:

```c
struct s { char c; char buf[512] __attribute__((aligned(16))); };
_Static_assert(__builtin_offsetof(struct s, buf) % 16 == 0, "buf 16-aligned");
```

**Fix: apply `aligned(N)` to layout** — (a) round a struct **field**'s offset up
to `N`, (b) raise the **struct's** own alignment/size to a multiple of `N`, and
(c) align **stack slots** for locals carrying the attribute (e.g.
`uint8_t observed[16] __attribute__((aligned(16)))`). This is the one thing
between the self-compiled kernel and reaching the desktop.

</details>

### K12 — inline-asm operand allocator must EXCLUDE clobbered registers — **DONE**

**RESOLVED (branch Teo).** The `"r"` operand allocator now removes from its free
set (a) every register in the **clobber list** (kept in the AST now, no longer
discarded) and (b) every hard register the **template writes/reads as `%%reg`**
(any width — `rdi`/`edi`/`di`/`dil`…, mapped by `asm_phys_reg`; conservative,
a read-only mention is excluded too — always sound). So no allocatable operand
can land in a register the asm destroys. Verified on the REAL kernel: both
`iretq` trampolines in process.c now load argc/argv/envp into rdi/rsi/rdx via
r9/r10/r11 and push operands from rax/rcx/rbx/r8 — none template-written;
gcc-refereed exec test `tests/exec/asm-clobber.c` (clobber-list AND
template-written paths) passes. Suite 88/88, self-host holds, kernel 89/89.
Alongside, K11 got a refinement: a local whose *type* is over-aligned (a struct
with an aligned member) now also gets its stack slot rounded to the type's
natural alignment (`ty_align`), not just the declarator attribute.

<details><summary>original triage (kept for context)</summary>

*With aligned(N) honored (K11), the self-compiled kernel boots even further —
past the first context switch — then `#GP`s on the `iretq` that launches the
first ring-3 process (`process_trampoline`), i.e. right as it would start
`init`/`home`.*

Root cause: an inline-asm `"r"` operand is allocated to a register named in the
**clobber list**, and the asm's own instructions destroy it before it's used.
`process_trampoline` builds the iret frame with 7 `"r"` operands and clobbers
`rdi`,`rdx`:

```c
"movq %6, %%rdx\n"     /* envp -> rdx (rdx is clobbered) */
"pushq %2\n"           /* cs=0x23 ... but %2 was allocated to rdx! */
"iretq\n"
: : ... "r"((uint64_t)(0x20|3)) /*=%2 cs*/ ... "r"(envp) /*=%6*/
: "rdi", "rdx", "memory"
```

EmbCC put `%2` (cs) in `rdx`; the `movq %6,%%rdx` overwrites it with `envp`, so
`push %2` pushes `envp` as **CS** → `iretq` faults. Minimal host repro (7 ops,
clobber `rdi`/`rdx`) — EmbCC emits `push %rdx` for the operand despite the
clobber; gcc never does:

```c
void f(unsigned long o0,unsigned long o1,unsigned long o2,unsigned long o3,
       unsigned long o4,unsigned long o5,unsigned long o6){
  __asm__ volatile("movq %4,%%rdi\n movq %5,%%rsi\n movq %6,%%rdx\n"
                   "pushq %0\n pushq %1\n pushq %2\n pushq %3\n"
   : : "r"(o0),"r"(o1),"r"(o2),"r"(o3),"r"(o4),"r"(o5),"r"(o6)
   : "rdi","rdx","memory"); }        /* embcc: 'push %rdx' for %2 — the bug */
```

**Fix:** remove clobber-list registers (and any register the template writes
explicitly, e.g. `%%rdi`/`%%rsi`/`%%rdx` here) from the operand allocator's free
set, so no `"r"` operand is ever placed in one. This is the last thing between
the self-compiled kernel and userspace / the desktop.

*(K1 follow-ups spotted alongside — **now DONE**: inline-asm memory operands
`movq disp(%base), %dst` and the store reverse `movq %src, disp(%base)` (incl.
rbp/rsp/r12/r13 SIB / forced-disp bases), plus the ALU ops add/sub/and/or/xor/
cmp in `%src,%dst` and `$imm,%dst` (imm8/imm32, 64- and 32-bit) forms. Every
encoding byte-compared to gas; gcc-refereed exec test `tests/exec/asm-mem-alu.c`
+ extended `tests/golden/inline-asm-kernel.sh`. Remaining known gap, NOT on the
boot path: a `+r` read-write operand's read side isn't wired — sema accepts `+`
but treats it as output-only, so the initial value isn't loaded. The kernel
uses no `+` constraints; deferred.)*

</details>

### K13 — stack usage: `-O0` frames are ~18× GCC's, overflowing the kernel stack

*With K12 in, the self-compiled kernel boots ALL the way through init, the
dynamic linker runs, and **userspace launches** (`home: launched
/system/bin/home.elf as pid 4`) — then a **Double Fault** at a plain
`mov %rax,-0x70(%rbp)` in `ata_read_dma`, with a garbled backtrace: the classic
**kernel-stack-overflow** signature.*

EmbCC at `-O0` spills every local to the stack (no register allocation, no
slot reuse), so frames are far larger than GCC's, and a deep kernel call chain
(`syscall → vfs → embkfs → block → ata_read_dma → …`) overflows the **16 KiB**
per-thread kernel stack (`KSTACK_SIZE`, myos `process.h:37`):

| function | GCC frame | EmbCC frame |
|---|---|---|
| `ata_read_dma` | 96 B (`sub $0x60`) | **1760 B** (`sub $0x6e0`) |
| kernel-wide | — | **331 functions > 1 KB**, biggest ~4 KB |

The code is *correct* — it's just too stack-hungry. This is the first item where
"compiles + is correct" isn't enough; it's a **codegen-quality** gap.

**Fix (EmbCC side):** cut stack usage — real **register allocation** (the started
optimizer, `src/opt/opt.c`) so hot locals live in registers, and/or **reuse
stack slots** for locals whose live ranges don't overlap (today each gets its own
slot). Getting close to GCC's frame sizes lets the self-compiled kernel run in
the same 16 KiB the GCC kernel uses.

*(Confirmed by a diagnostic-only KSTACK_SIZE bump on the myos side — NOT
committed, per "don't change the kernel for an EmbCC gap": with a larger stack
the self-compiled kernel runs past this. So this is the last codegen item; once
EmbCC's stack usage drops, no kernel change is needed.)*

---

## Tier 1 — blocks ordinary real C; do these first (small, high-leverage)

*Status: both landed. On the TinyCC 0.9.27 corpus (24 files) the block-scope
`extern`/`typedef` and `redefined-differently` first-errors dropped to **zero**;
every remaining blocker is now `cannot find include file "dlfcn.h"` (a missing
host header for dynamic linking — integration, not a language gap) or a file's
own `#error`. The front-end runs the whole preprocessor + language layer before
stopping. Full suite 74/74; self-host fixed point holds.*

- [x] **1. Block-scope `extern` / `typedef` declarations.**
  `int f(void){ extern int errno; ... }` and `... { typedef int T; ... }`.
  Error: `expected a statement, got 'extern'` / `'typedef'`. Confirmed on ~33
  newlib files. `parse_stmt` accepts a type or `static` (and the register-asm
  form) as the leading token of a local declaration, but not `extern` or a local
  `typedef`. Fix: recognize `extern`/`typedef` (and `extern`+type combos) at the
  start of a block-scope declaration. Very common in real C; contained fix.

- [x] **2. Macro redefinition: warn, don't fatal.**
  A macro redefined with a DIFFERENT body is a fatal `error: macro 'X' redefined
  differently` in EmbCC; gcc issues a WARNING and takes the new definition.
  Blocked ~190 corpus files (mostly `SEEK_SET`, `ARG_MAX` — largely a two-header-
  set artifact, but the fatal-vs-warn behavior is a real, gcc-incompatible
  strictness). Fix: downgrade a benign redefinition to a diagnostic that does not
  stop the compile (match gcc), keeping the LAST definition. (Identical
  redefinition already accepted.)

## Tier 2 — genuine ISO features still missing (confirmed by direct probe)

Lower corpus frequency only because Tier-1 fails hit first; each is real C.

- [x] **3. Bitfields.** `struct F { unsigned a:3, b:5; };` — DONE.
  Little-endian gcc-compatible layout (fields don't cross a storage-unit
  boundary; anonymous padding fields; `:0` separators; `packed`), signed and
  unsigned extraction via the two-shift trick, read/assign/`+=`/`++`, access
  through pointers, and `long` (>32-bit) fields. Verified against gcc for
  values, `sizeof` layout, and by-value SysV ABI (incl. a cross-ABI
  embcc→gcc link). `&bitfield` is refused. Braced initialization of bitfields
  also works — static, local, and designated — by carrying `(bit_off,
  bit_width)` on `initelem` and masking/merging in both the static-byte and
  local lowerings.
- [x] **4. Designated ARRAY initializers.** `int a[5] = { [2]=9, [4]=1 };` —
  DONE. File-scope and local, unsized arrays sized to the highest index
  reached, gaps zero-filled, a `[i]=` designator repositions the running
  index with positional elements continuing after it, later writes to a slot
  win. Verified against gcc; index-past-end is refused.
- [x] **5. Compound literals.** `&(struct P){ .x = 5 }` — DONE. `(type){init}`
  becomes an unnamed object with automatic storage (a synthesized local slot),
  initialized like a declared aggregate (zero-fill, designators, last-write-
  wins). It is an lvalue: address-of, member access, array decay + indexing,
  by-value passing, scalar literals, and initializing a local all work and
  match gcc. File-scope (static-storage) literals work too: a direct
  `T g = (T){...}` (or one nested in a static initializer) unwraps to its brace
  initializer, and `&(T){...}` becomes an anonymous global the pointer
  relocates to.
- [x] **6. A real `_Bool` type.** DONE. `_Bool` is a keyword and a distinct
  1-byte unsigned type (TY_BOOL); a store normalizes any nonzero scalar
  (integer, pointer, or float) to 1. `<stdbool.h>` now maps `bool` to it, so
  `sizeof(bool)==1`. Verified against gcc.
- [x] **7. C11 niceties.** All DONE.
  - [x] `_Static_assert(expr, "msg")` — evaluated at parse time; legal at file
    scope, in a struct/union body, and in a block; the message is optional
    (C23). A false assertion is a fatal error naming the message.
    (`size_fold` also learned `&&`/`||`.)
  - [x] `_Generic(ctrl, T: e, ..., default: e)` — the arm whose type matches
    the controlling expression (after its lvalue conversion) is selected at
    compile time; the controlling expression is not evaluated; no match and
    no default is an error. Verified against gcc.
  - [x] Anonymous struct/union members — a nameless `struct{...};`/`union{...};`
    member's fields are reached through the enclosing object (member lookup
    descends into them with cumulative offsets). Verified against gcc.

## Tier 3 — integration / ergonomics (NOT compiler gaps)

- [x] **8. Accept `-isystem`** — DONE. `-isystem DIR` and `-isystemDIR` are
  accepted as an include directory (EmbCC keeps one search path), so build
  scripts that pass it work.
- **"cannot find include file"** = a missing/inconsistent libc header set on the
  path — an integration matter, not a language gap.
- **Implicit-declaration errors** = CORRECT C99 strictness; the fix is prototypes,
  not a lax mode. Do not "fix".

## Confirmed NOT gaps (work today)

`register`, `const`/`volatile` (accepted), variadic macros, `#`/`##`, function
pointers + arrays of them, structs/unions/enums, `goto`, varargs incl. float,
static/hex float, u64<->double, weak undefined refs.

---

## Acceptance

Tier 1 both land + a corpus re-run shows real C getting materially further before
hitting a Tier-2 feature; full test suite + self-host fixed point stay green.

## Status

All of Tier 1, Tier 2, and the Tier-3 `-isystem` item are DONE — each landed
with a gcc-refereed exec test, the full suite green (80/80), and the self-host
fixed point holding. The two initializer seams that were once refused loudly
are now implemented too: braced initialization of **bitfields** (static/local/
designated) and **file-scope compound literals** (direct value, nested, and
`&(T){...}` via an anonymous global). The remaining Tier-3 entries are
integration notes, not compiler work.
