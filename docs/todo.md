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

- [ ] **1. Block-scope `extern` / `typedef` declarations.**
  `int f(void){ extern int errno; ... }` and `... { typedef int T; ... }`.
  Error: `expected a statement, got 'extern'` / `'typedef'`. Confirmed on ~33
  newlib files. `parse_stmt` accepts a type or `static` (and the register-asm
  form) as the leading token of a local declaration, but not `extern` or a local
  `typedef`. Fix: recognize `extern`/`typedef` (and `extern`+type combos) at the
  start of a block-scope declaration. Very common in real C; contained fix.

- [ ] **2. Macro redefinition: warn, don't fatal.**
  A macro redefined with a DIFFERENT body is a fatal `error: macro 'X' redefined
  differently` in EmbCC; gcc issues a WARNING and takes the new definition.
  Blocked ~190 corpus files (mostly `SEEK_SET`, `ARG_MAX` — largely a two-header-
  set artifact, but the fatal-vs-warn behavior is a real, gcc-incompatible
  strictness). Fix: downgrade a benign redefinition to a diagnostic that does not
  stop the compile (match gcc), keeping the LAST definition. (Identical
  redefinition already accepted.)

## Tier 2 — genuine ISO features still missing (confirmed by direct probe)

Lower corpus frequency only because Tier-1 fails hit first; each is real C.

- [ ] **3. Bitfields.** `struct F { unsigned a:3, b:5; };`
  Error: `expected ';' before ':'`. The biggest missing *language* feature —
  hardware registers and packed on-disk formats need it. Layout + load/store
  masking across the whole struct machinery.
- [ ] **4. Designated ARRAY initializers.** `int a[5] = { [2]=9, [4]=1 };`
  Error: `array [index] designators are not supported`. (The `.field` form
  already works.) Dispatch/lookup tables use these.
- [ ] **5. Compound literals.** `&(struct P){ .x = 5 }`.
  Error: `expected an expression, got '{'`.
- [ ] **6. A real `_Bool` type.** Today `bool` is a `#define` for `int`, so
  `sizeof(bool)==4` (should be 1) and `_Bool` isn't a keyword.
  Error: `expected a type before '_Bool'`.
- [ ] **7. C11 niceties.** `_Static_assert`, `_Generic`, anonymous struct/union
  members. Niche, but headers occasionally want them.

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
