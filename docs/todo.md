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

***79 / 89 kernel TUs now compile clean (was 22).*** **K1–K9 are all closed.**
The remaining 10 are distinct one-off gaps (below, "Remaining"), not the
lettered blockers. No kernel C was changed to work around any of these.

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

### Remaining one-offs (each a distinct feature, not a lettered gap)
Ten TUs still fail, each on something different: a GCC **statement expression**
`({ ... })` (selftests.c); an **array-range designator** `[a ... b] =`
(font_8x16.c); a **`static`-init that isn't constant** (keyboard.c); a
function that ends in a **non-returning asm loop** so the return-path check
fires (syscall.c `sys_exit`); a **`typedef` form** the parser trips on
(kprintf.c); an **asm constraint** not yet parsed (usermode.c); the gdt.c
**segment-reload trampoline** (inline-asm local labels + RIP-relative `leaq` +
`lretq`); a **`_Static_assert` ICE** (embkfs.c); plus integration
(a freestanding `<string.h>` for fd.c). The char*/unsigned char* signedness
mismatch (fat32.c) is now allowed.

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

### Not language gaps, but required for a *bootable* kernel (codegen/ABI)
The kernel is built with `-mcmodel=kernel -mno-red-zone -mno-sse -mno-mmx`
(higher-half at `0xFFFFFFFF80000000`; interrupt-safe; no vector regs). EmbCC's
CLI takes none of these today. The **kernel code model** (RIP-relative into the
top 2 GB) and **no-red-zone** codegen are hard requirements for a kernel that
boots — confirm EmbCC's default output already satisfies them, or add the flags.
(Separate from the language items above.)

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
