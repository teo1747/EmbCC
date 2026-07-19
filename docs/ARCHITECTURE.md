# Architecture (intended)

*Design record, written before implementation. Everything here is a starting
position chosen to serve ROADMAP M1 (emit something the OS runs) and the
constraints in TARGET_ABI.md. It is meant to be revised by contact with reality
— when it is, update this file and note why in DECISIONS.md.*

## 1. Shape: one binary, all phases in-process

EmbCC is a **single self-contained program**: driver, preprocessor, parser,
semantic analysis, codegen, assembler, and linker in one process.

This is not a style preference — it is forced by the target. EmbLinkOS has
**no `fork`/`exec`**, so a driver that spawns `cc1`/`as`/`ld` as separate
programs (the gcc model) is structurally impossible to host. It is precisely why
TCC could be ported and gcc never can. **EmbCC must be one process for the same
reason, from the first commit.** A design that assumes sub-processes would have
to be undone later.

```
source ──► lex ──► parse ──► sema ──► IR ──► codegen ──► object ──► link ──► ELF
                                                     └── all in-process ──┘
```

## 2. Phases

| Phase | Responsibility | Notes |
|---|---|---|
| **driver** | argv, flags, deciding compile-vs-link, file discovery | Keep flags a *deliberate subset*; do not clone gcc's surface |
| **lex** | tokens, including the preprocessor's needs | |
| **cpp** | `#include`, `#define`, conditionals | Needed early — the OS's headers are real newlib headers (see §5) |
| **parse** | C subset → AST | Grow the subset by need, not by standard-completeness |
| **sema** | types, declarations, conversions, diagnostics | Where most "real compiler" work lives |
| **IR** | a small typed intermediate form | See §3 |
| **codegen** | IR → x86-64, System V AMD64 | See §4 |
| **asm** | encode instructions to bytes | Integrated; no external assembler exists on-OS |
| **link** | objects + archives + (later) shared objects → ELF | See §6 — the most target-specific part |

## 3. IR: start with something honest and small

**Decision for M1–M2:** the simplest thing that lets codegen be written without
lying — likely a linear three-address IR over virtual registers, with explicit
types (integers by width and signedness, pointers, aggregates by size/align).

Explicitly **not** planned for the early milestones: SSA, a pass manager, an
optimizer. They are the right shape for a mature compiler and the wrong shape
for one that has never run a program. Correct-and-slow first; the ROADMAP has no
performance milestone before M4 for exactly this reason.

## 4. Codegen: x86-64, System V AMD64

- Integer args: `rdi, rsi, rdx, rcx, r8, r9`; returns in `rax`/`rdx`.
- Floats in `xmm0–7`; return in `xmm0`.
- 16-byte stack alignment at `call`.
- **Explicit register constraints in inline asm are a first-class requirement**,
  not a nicety: the OS's syscall header binds `r10`/`r8`/`r9` by name, and TCC's
  inability to do so forced a `__TINYC__` workaround into the ABI header
  (TARGET_ABI.md §3). EmbCC should support this properly and let that workaround
  die.
- **Intrinsics:** the moment codegen emits a libcall gcc inlines (`__floatundisf`
  is the known first one), decide per TARGET_ABI.md §7 — inline it in codegen
  (preferred) or ship an `libembcc1` runtime.

## 5. The preprocessor is on the critical path

EmbCC must consume **real newlib headers** (`/system/abi/include` on the OS).
Those headers are not gentle: they select fixed-width types from the *full* GCC
predefined-macro family and hard-`#error` when it is absent — this is what broke
TCC until patch 0002 supplied 67 macros taken verbatim from
`x86_64-elf-gcc -dM -E`.

**Therefore:** EmbCC must predefine the complete x86-64 LP64 macro set
(`__INT64_TYPE__`, `__INTPTR_TYPE__`, `__SIZEOF_*`, `__*_MAX__`, `__CHAR_BIT__`,
…) from the beginning. Treat `x86_64-elf-gcc -dM -E </dev/null` as the reference
list; do not hand-derive it. Getting this wrong does not degrade gracefully — the
first real `#include <stdint.h>` fails outright.

## 6. Linker: the most EmbLink-specific component

The linker is where the target contract actually bites (TARGET_ABI.md §4). The
non-negotiables, each learned from a TCC failure:

- **Static links must not emit a PLT.** No resolver exists; PLT slots are
  unbindable and the program dies at a wild jump with a valid-looking ELF.
- **Static links must relocate their own GOT** if GOT-indirect access is emitted
  at all — newlib's `errno`/`stderr` go through `_impure_ptr`, a GOTPCREL access.
- **Weak undefined symbols bind to 0 at link time**, with no relocation emitted.
- **Archive semantics must satisfy a shared object's undefined symbols** —
  because there is no runtime libc, `libembk.so`'s libc imports must be pulled
  into the executable from `-lc`/`-lm` and re-exported.
- **Dynamic output:** `ET_EXEC` only (never PIE), classic `DT_HASH`, and
  relocations confined to `RELATIVE/COPY/64/GLOB_DAT/JUMP_SLOT`.

A defensible staging: **M1–M2 emit relocatable objects only** and let the
existing toolchain link them (validating codegen independently of linking), then
build the linker in M3 when self-hosting demands one binary that does everything.

## 7. Source layout (proposed)

```
src/
  driver/     argv, flags, orchestration
  lex/        tokens
  cpp/        preprocessor (+ the predefined macro table, §5)
  parse/      AST
  sema/       types + checking + diagnostics
  ir/         the intermediate form
  codegen/    x86-64 lowering + register allocation
  asm/        instruction encoding
  link/       ELF reading/writing, relocation, archives
  elf/        shared ELF structures used by asm + link
tests/
  exec/       programs compiled and RUN (the ones that count)
  compile/    programs that must compile (or must fail, with which diagnostic)
  golden/     output compared against gcc/TCC for agreed cases
```

**Self-hosting constrains the source itself.** EmbCC must eventually compile
EmbCC, so its own code should stay within the C subset it implements — no
dependency on anything it cannot yet parse. Practically: plain C99, no
sprawling third-party headers, and a periodic honest check of "could our own
compiler read this file yet?"

## 8. Non-goals for the early milestones

Stated so they are not accidentally attempted: optimization passes, debug info
(DWARF), C++, TLS/`__thread`, PIE/PIC output, cross-targets other than x86-64,
and the kernel's freestanding mode (DECISIONS D-007). Several become interesting
later; none belong before a program runs.
