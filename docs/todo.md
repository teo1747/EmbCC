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

***22 / 89 kernel TUs already compile clean.*** The gaps below are the
first-error-per-file from that sweep (so each category has more behind it),
ranked by how much of the kernel they block. No kernel C was changed to work
around any of these — that's the point; they're EmbCC's to close.

### K1 — inline-asm assembler (THE blocker; touches most of `arch/`, `drivers/`, `mm/`)
EmbCC currently assembles only `int`/`cpuid`/`rdrand`/`setc`. The kernel is
pervasive hardware C and needs a real x86-64 inline-asm path with operand
constraints. Instructions seen already (first-errors only): `pushfq`/`popfq`,
`outb`/`outl` (plus `inb`/`inw`/`inl`/`outw`), `mov` to/from **CR0/2/3/4** and
MSR/segment paths, `rdtsc`, `pause`, `mfence` (and `lfence`/`sfence`), plus the
rest the kernel uses behind these: `cli`/`sti`, `hlt`, `rdmsr`/`wrmsr`,
`invlpg`, `lgdt`/`lidt`/`ltr`, `wbinvd`. Also the **constraint grammar**:
`"=r"`,`"r"`,`"=a"`,`"a"`,`"=m"`,`"m"`,`"N"`(imm), `"memory"`/`"cc"` clobbers,
`%0`-style operand refs, and `volatile`. Dominant item — without it the
low-level kernel can't be built at all.

### K2 — GCC builtins
- `__builtin_bswap16/32/64` (9 files — network byte order in `kernel/net/`).
- `__atomic_load_n`, `__atomic_exchange_n`, `__atomic_store_n` (and the rest of
  the `__atomic_*` compare/fetch family; check `__sync_*` too) — the kernel's
  spinlock / ksync / block-stat primitives are built on these.
- Worth checking while here: `__builtin_memcpy/memset`, `__builtin_expect`,
  `__builtin_unreachable`.

### K3 — `sizeof` / `offsetof` as an integer-constant-expression
`_Static_assert(sizeof(struct T) == 0x50, ...)` and
`_Static_assert(offsetof(struct T, f) == N, ...)` fail with *"`_Static_assert`
needs a constant integer expression"* (e.g. `boot_protocol.h:83`, 5 files). The
constant folder must evaluate `sizeof(type-name)` and `offsetof(...)` in ICE
context (the kernel uses these to pin ABI struct layouts).

### K4 — raise the parameter / argument cap (currently 12)
*"more than 12 parameters"* / *"more than 12 call arguments"*
(`framebuffer.h:78`, `net.c:216`). Several kernel functions exceed 12; raise the
fixed limit (or make it dynamic).

### K5 — char/string escape sequences
*"unknown escape '\v' in character constant"* (`ctype.h:44`). The lexer's escape
table is incomplete — at least `\v`, and almost certainly `\f \a \b \?`, `\xHH`,
and octal `\NNN` as well.

### K6 — `&array` (address-of an array object)
`(uint64_t)&gdt` where `gdt` is an array type is refused with *"'&' on an array
is not supported yet"* (`gdt.c:140`, `idt.c:65`). `&arr` is valid C (yields
`T(*)[N]`); the descriptor-table code relies on it.

### K7 — use-before-declaration of a `static` function
`epfs_lookup` is called before its definition and rejected (*"used before its
declaration"*, `epfs.c:156`). GCC resolves this within a TU; EmbCC needs a
forward pass (or to accept a later same-TU `static` definition).

### K8 — `__attribute__` after a declarator
`uint8_t observed[16] __attribute__((aligned(16)));` → *"expected ';' before
'__attribute__'"* (`process.c:2891`). Attributes are accepted in some positions
but not trailing a local array declarator.

### K9 — minor / integration
- An **empty translation unit** errors (*"no functions or globals in file"*,
  `spawn.c` is 0 lines). An empty TU should yield a valid (empty) object.
- One TU wants a freestanding `<string.h>` on the include path.

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
