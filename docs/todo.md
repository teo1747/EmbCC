# TODO — two EmbCC gaps found while self-hosting emlibc's FP math

*The first batch (goto/labels, null-statement body, block-scope struct/union,
multiple declarators, "x" SSE asm, `__builtin_*`) is done — EmbCC now compiles
all 31 lifted fdlibm files + the glue, and `test emlibc math selfhost` closes
the loop INCLUDING floating point (the OS compiles 39 FP units with embcc, links
with embld, runs at 1e-12). These two remaining gaps surfaced only when
compiling `stdio.c` + a weak decoupling; both are sidestepped emlibc-side for
now, so nothing is blocked — implement them and the sidesteps come out.*

*Repro (host), from the EmbCC dir:*

```
INC="-I ../myos/user/emlibc/include -I include -I ../myos/user/lib"
./embcc -c ../myos/user/emlibc/stdio/stdio.c $INC -o /tmp/x.o        # gap 2
./embcc -c ../myos/user/emlibc/stdlib/stdlib.c $INC -o /tmp/y.o      # then: nm /tmp/y.o | grep flush  (gap 1)
```

---

- [ ] **1. Weak UNDEFINED references** (`__attribute__((weak))` on an `extern`).
  `stdlib.c`: `extern void emlibc_stdio_flush_all(void) __attribute__((weak));`
  then `if (emlibc_stdio_flush_all) emlibc_stdio_flush_all();`.
  gcc emits the symbol as `w` (weak undef) → EmbLD resolves an unresolved weak to
  0, so a program that never links stdio just skips the flush.
  **EmbCC emits it as `U` (global undef)** → EmbLD errors
  `undefined symbol 'emlibc_stdio_flush_all'`.
  Fix: when a declaration is `weak` and the symbol is referenced-but-undefined,
  give its symtab entry `STB_WEAK`, not `STB_GLOBAL`.
  *Sidestep now:* the flush hook stays weak (helps the gcc build) but the on-OS
  self-host just includes `stdio.o` in the link so the symbol is defined.

- [ ] **2. Unsigned 64-bit ↔ `double` conversion.**
  `stdio.c` `%f` formatter did `unsigned long long ip = (unsigned long long)v;`
  and `(double)ip`.
  Error: `converting between unsigned long and double is not supported yet
  (SSE2 has no unsigned 64-bit conversion; cast through a signed long if the
  value fits)`. (Correct diagnosis — `cvtsi2sd`/`cvttsd2si` are signed-only.)
  Fix: emit the standard unsigned fixups — for `u64 -> double`, the
  split-and-add sequence (or the sign-bit test + scale); for `double -> u64`, the
  `> 2^63` bias correction.
  *Sidestep now:* the formatter uses signed `long long` (every `%f`
  integer/fraction part fits in 63 bits).

---

## Acceptance

When both land: revert the two sidesteps (real weak-flush decoupling so
stdio-free programs don't link stdio; unsigned `%f`), and `embcc -c` +
`test emlibc math selfhost` stay green. Nothing else depends on these today.
