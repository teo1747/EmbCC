# TODO — front-end gaps to compile real vendored C (fdlibm) + emlibc

> **RESOLVED (commit pending).** All seven gaps below (A1–A4, B5–B7) are
> implemented, each proven byte-identical to gcc; `tests/exec/frontend-gaps.c`
> is the golden. 72/72 tests pass and the self-host fixed point still closes.
> Against the real sources, **28/32 fdlibm files now compile**; the remaining 4
> are exactly the section-C *implicit-declaration* cases (`expm1`, `finite`),
> which are correct C99 strictness — fix them with prototypes emlibc-side.

*Goal that drives this list: fold **math** into the on-OS self-host set. emlibc's
`<math.h>` is now backed by real **fdlibm** (Sun's freely-licensed ~1-ulp
library, lifted verbatim into `myos/user/emlibc/math/fdlibm/`). To have EmbCC
compile that math on the OS — closing the self-host loop **including floating
point** — EmbCC must accept the standard C those files use.*

*Found by running `embcc -c` over `myos/user/emlibc/math/math.c` and every
`myos/user/emlibc/math/fdlibm/*.c`. FP codegen itself works (commit `c8b8ef5`);
these are the remaining **front-end** gaps. Each row is real C that gcc accepts;
the error text is EmbCC's own. Repro (host):*

```
INC="-I ../myos/user/emlibc/include -I include -I ../myos/user/lib -I ../myos/user/emlibc/math/fdlibm"
./embcc -c ../myos/user/emlibc/math/fdlibm/k_rem_pio2.c $INC -o /tmp/x.o
```

---

## A. Must add — standard C in vendored fdlibm, cannot be worked around

Without these, real third-party C (not just fdlibm) won't compile.

- [x] **1. `goto` + labels.**
  `recompute: … goto recompute;` (`k_rem_pio2.c`) — a retry loop.
  Error: `expected ';' before ':'` (at the label).

- [x] **2. Null statement as a body.**
  `for(k=1;iq[jk-k]==0;k++);` (`k_rem_pio2.c`), `while((*d++=*s++));` (`string.c`).
  Error: `expected a statement, got ';'`. (The empty `;` statement.)

- [x] **3. Block-scope struct/union definitions.**
  `union { double d; uint64_t u; } v;` inside a function
  (`math_config.h` shim, `syscalls.c` `chdir`).
  Error: `define structs/unions at file scope (block-scope type definitions are not supported)`.

- [x] **4. Multiple declarators in one declaration.**
  `extern double f(double), g(double);` (`math.c`).
  Error: `expected '{' or ';' before ','`.

## B. Your call — needed for natural gcc-style code, but emlibc can sidestep each

Adding the mechanism to EmbCC keeps the code natural; otherwise emlibc works
around it (noted per row).

- [x] **5. SSE/XMM inline-asm constraints `"x"` / `"=x"`** (and `Yz`, etc.).
  `__asm__("sqrtsd %1,%0":"=x"(r):"x"(x))` (`math.c` hardware sqrt).
  Error: `asm constraint '=x' is not supported (EmbCC handles a/b/c/d/S/D, r/q/g/m, register-asm)`.
  *Sidestep:* use fdlibm's software `e_sqrt.c` (no asm).

- [x] **6. `__builtin_*`** — `__builtin_huge_val()`, `__builtin_inff()`,
  `__builtin_nanf()`, `__builtin_memcpy()`.
  Used for `HUGE_VAL`/`INFINITY`/`NAN` and struct copies.
  Error: `'__builtin_huge_val' is not declared`.
  *Sidestep:* bit-construct the specials; hand-roll memcpy.

- [x] **7. Ship a freestanding `<stdint.h>`.** EmbCC's `include/` has
  `stddef/stdarg/stdbool/float` only; real code wants `int64_t` etc.
  *Sidestep (current):* emlibc ships its own `stdint.h`.

## C. Not a gap — EmbCC's strictness is correct, don't "fix" it

- **Implicit function declaration.** fdlibm calls `expm1()`/`finite()` with no
  visible prototype (C89 style); EmbCC errors `… is not declared, add a
  prototype`, gcc only warns. This is correct C99 — the fix is prototypes (added
  emlibc-side), not a lax mode.


