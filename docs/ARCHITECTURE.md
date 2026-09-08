# Architecture

*Originally a design record written before implementation, chosen to serve
ROADMAP M1 (emit something the OS runs) and the constraints in TARGET_ABI.md.
It has since been revised by contact with reality, which was always the
intent — §3, §6 and §8 carry the revisions and say what changed. Keep doing
that: when reality disagrees with this file, update it and note why in
DECISIONS.md.*

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
| **parse** | C → AST | The subset grew by need, not by standard-completeness; `todo.md` tracks what is left |
| **sema** | types, declarations, conversions, diagnostics | Where most "real compiler" work lives |
| **IR** | a small typed intermediate form | See §3 |
| **codegen** | IR → x86-64, System V AMD64 | See §4 |
| **asm** | encode instructions to bytes | Integrated; no external assembler exists on-OS |
| **as** | standalone NASM/Intel `.asm` → ELF object | `embas` / `embcc -c foo.asm`; byte-identical to nasm on the kernel corpus (A1) |
| **opt** | IR→IR optimization at `-O1`/`-O2` | See §3; SSA is built on demand here, not carried in the IR |
| **debug** | DWARF-4 line/frame/local emission for `-g` | Read back by EmbDBG (`tools/embdbg`) |
| **link** | objects + archives → ET_EXEC ELF and EMBX | See §6 — the most target-specific part |

## 3. IR: something honest and small

**The decision, and it held:** a linear three-address IR over virtual
registers, with explicit types (integers by width and signedness, pointers,
aggregates by size/align). Correct-and-slow first — for a compiler that had
never run a program, that was the right call, and the IR did not need replacing
when the optimizer arrived.

SSA, a pass manager and an optimizer were explicitly **not** planned for the
early milestones. All three have since landed, deliberately and in that order of
difficulty:

- **`src/opt`** runs a local pass set (folding, strength reduction, value
  numbering/CSE, copy propagation, DCE, immediate folding, store forwarding) and
  a global one at `-O2` — function inlining, SCCP, dominator-scoped global CSE,
  and redundant-load elimination.
- **SSA is built on demand**, not carried in the IR: `pass_mem` constructs the
  CFG, the dominator tree (Cooper-Harvey-Kennedy) and dominance frontiers,
  inserts phis, renames, and destructs SSA back to copies. The IR stays the
  honest linear form it started as; SSA is a lens the optimizer puts on it.
- **`src/codegen`** carries register allocation (Chaitin-Briggs), stack-slot
  coalescing and a residency cache.

The single-assignment temporaries are still what make the *local* passes sound
with no analysis at all — that property is why the cheap passes came first.

## 4. Codegen: x86-64, System V AMD64

- Integer args: `rdi, rsi, rdx, rcx, r8, r9`; returns in `rax`/`rdx`.
- Floats in `xmm0–7`; return in `xmm0`.
- 16-byte stack alignment at `call`.
- **Explicit register constraints in inline asm are a first-class requirement**,
  not a nicety: the OS's syscall header binds `r10`/`r8`/`r9` by name, and TCC's
  inability to do so forced a `__TINYC__` workaround into the ABI header
  (TARGET_ABI.md §3). EmbCC supports this properly — fixed-register extended asm
  with the kernel's full vocabulary — so that workaround can die.
- **Intrinsics:** when codegen would emit a libcall gcc inlines (`__floatundisf`
  was the first), TARGET_ABI.md §7 governs — inline it in codegen (what we do)
  rather than shipping a `libembcc1` runtime.

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

The staging that was chosen, and worked: **M1–M2 emitted relocatable objects
only** and let the existing toolchain link them, validating codegen
independently of linking; the linker was built for M3, when self-hosting
demanded it.

**EmbLD exists and does all of the above** (`src/link`, the `embld` tool). It
links EmbCC itself and it links the EmbLinkOS kernel — including
linker-defined end symbols and higher-half LMA (`p_paddr`) — and it emits the
native **EMBX** container as well as ET_EXEC ELF. The PLT, GOT and weak-symbol
facts above are each covered by a golden test rather than a comment.

## 7. Source layout

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
  as/         EmbAS — standalone NASM/Intel assembler
  opt/        IR-level optimizer (local + global passes, SSA on demand)
  debug/      DWARF-4 emission for -g
  embx/       the EMBX container, byte-exact
  link/       EmbLD — ELF reading/writing, relocation, archives, EMBX output
  elf/        shared ELF structures used by asm, as + link
tests/
  exec/       programs compiled and RUN (the ones that count)
  compile/    programs that must compile (or must fail, with which diagnostic)
  golden/     output compared against gcc/TCC for agreed cases
```

**Self-hosting constrains the source itself.** EmbCC compiles EmbCC, so its own
code stays within the C subset it implements — no dependency on anything it
cannot parse. Practically: plain C99, no sprawling third-party headers. This is
no longer a periodic honest check but a hard gate: `tests/golden/self-host.sh`
and the on-OS `test embcc self` oracle fail the moment a source drifts outside
the subset.

## 8. Non-goals for the early milestones

Stated so they were not accidentally attempted before a program ran: optimization
passes, debug info (DWARF), C++, TLS/`__thread`, PIE/PIC output, cross-targets
other than x86-64, and the kernel's freestanding mode (DECISIONS D-007).

*Since the early milestones closed, three of these were done deliberately:*
**optimization passes** (`-O1`/`-O2` — §3, `src/opt`/`src/codegen`), **debug
info** (`-g` emits DWARF-4 line/frame/locals, and EmbDBG reads it back), and the
**kernel's freestanding mode** (`-mno-sse`, `-mcmodel=kernel` and friends — EmbCC
compiles the whole EmbLinkOS kernel, which boots to the desktop).

**Still out of scope, and refused loudly rather than faked:** C++ (the intended
second language, D-008, but a different project in size), TLS/`__thread`,
PIE/PIC output, and cross-targets other than x86-64. The remaining C-language
gaps — VLA, `_Complex`, 80-bit `long double` — are tracked in `todo.md` against
a real corpus.
