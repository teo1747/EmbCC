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
  embcc→gcc link). `&bitfield` is refused. *Known gap (seam left):* a braced
  initializer that targets a bitfield is refused loudly (not miscompiled) —
  needs bit-masked merge on `initelem` in both the static-byte and local
  lowerings; assign in a statement meanwhile.
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
  match gcc. *Seam:* a file-scope (static-storage) compound literal is refused
  loudly, not lowered — it needs a synthesized static global.
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

- [ ] **8. Accept `-isystem`** (today only `-I`). Trivial; helps drive real
  builds whose scripts pass `-isystem`.
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
