# EmbCC — Vision Beyond a C Compiler

*Status: **long-horizon picture**, not a plan. [VISION.md](VISION.md) states why
EmbCC exists and its discipline of restraint; [ROADMAP.md](ROADMAP.md) states
what is actually committed (M0–M4). This document records where the project
could go **after** the total loop closes — and every item here is governed by
DECISIONS D-006: a capability is added for a stated reason, never because a
compiler "should" have it. Where this document and a decision record disagree,
the decision record wins.*

*Several items below have since landed off this list and are marked **Done** or
**Underway** in place — diagnostics, optimization, debug info and the inline-asm
work, each pulled forward because a concrete need arrived, which is exactly the
D-006 test. The rest is still horizon. Read the marks: this file deliberately
keeps finished items rather than deleting them, so the reasoning that justified
them stays visible.*

EmbCC is more than a compiler that translates C source into machine code. Its
long-term goal is to become the complete native compiler infrastructure for
EmbLinkOS — deeply integrated with the operating system, the development tools,
and the build system.

## Core goals

* Support the C language, grown by need (D-002): the subset expands when a real
  program demands it, with "full C" as the limit of that process rather than an
  up-front target.
* In the long term, **C++** — the second and final planned language (D-008),
  taken up only after the C compiler closes the M4 loop. **Still ahead.**
* Produce well-optimized native executables — *after* correct ones
  (ARCHITECTURE §3: correct-and-slow first; codegen quality is the first honest
  post-M4 reason to prefer EmbCC over TCC). **Underway** — `src/opt` runs SSA
  mem2reg, inlining, SCCP and global CSE; `src/codegen` does Chaitin-Briggs
  register allocation. `-O2` `.text` is at 1.63× gcc `-O0`, down from 2.81×.
* Become self-hosting by compiling EmbCC with EmbCC (ROADMAP M3). **Done** —
  the fixed point closed on the OS 2026-07-24 and holds over 16 sources.
* Fast compilation without sacrificing code quality.
* Modern, clear, actionable diagnostics (this one was *not* deferred — M2
  required diagnostics a human can act on). **Done** — clang-style carets with
  real columns for semantic as well as syntax errors, colour, span underlines,
  "did you mean?", and macro-expansion / previous-declaration notes.

## Native EmbLinkOS integration

EmbCC is designed specifically for EmbLinkOS and understands its architecture.

* Direct support for the EmbLinkOS ABI ([TARGET_ABI.md](TARGET_ABI.md)).
* **Executable format: ELF *and* EMBX.** ~~Per D-003 (firm), a native format is
  an **ELF superset**.~~ **D-003 was REVISED 2026-07-24** once the OS's
  capability model landed: EmbLinkOS has its own capability-carrying container,
  **EMBX**, and EmbLD emits it directly (`--embx --cap NAME`) alongside ET_EXEC
  ELF. ELF remains the porting lane — a dual *loader*, not a converter, mirroring
  EMBKFS's native-plus-FAT32 split. Read D-003 for why the superset plan was
  abandoned with eyes open. The `.note`-section idea below is superseded, kept
  because the reasoning is still instructive. The first candidate extension
  remains a declared capability manifest, gated on the capability model
  existing first.
* Tight integration with EmbBuild (M4 makes this real: EmbBuild builds EmbCC on
  the OS).
* Automatic discovery of the system SDK, runtime libraries, and headers
  (`/system/abi/include`, crt0/syscalls/libc as TARGET_ABI §4 lays them out).
* Native understanding of EmbLinkOS syscalls and services — starting with
  first-class fixed-register inline-asm constraints so the `__TINYC__`
  workaround in the syscall header can die (ARCHITECTURE §4). **Landed
  2026-07-24:** EmbCC has GCC extended inline asm with the fixed-register
  letters (a/b/c/d/S/D), `register T x __asm__("r10")` bindings for `r`, and
  the `inline` keyword — enough that the gcc branch of `embk_syscall.h`
  compiles under EmbCC, so the `__TINYC__` workaround dies for EmbCC. Proven
  on the OS: EmbCC-compiled `int $0x80` stubs made real write(1,…) and
  exit(42) syscalls (kernel `test embcc asm`). *(Scope was the userland rim only,
  "the kernel's asm stays the cross gcc's" — **that limit is gone**: the
  extended-asm assembler grew the kernel's full hardware vocabulary, every
  encoding byte-verified against objdump, and D-007 was revised accordingly. The
  template vocabulary is no longer `int $imm`.)* **File-scope asm
  followed:** a two-pass mini-assembler (src/asm/topasm.c) handles crt0's
  `_start` stub vocabulary — `.global`/`.globl`, labels (named + numeric-
  local), `and $imm,%reg`, `call sym` (PLT32), `jmp local-label`, `ret` —
  emitting a global `_start` symbol and a relocation to the C entry it
  calls. **The rim closed:** `va_arg`, complex declarators like
  `void (*arr[])(void)`, `__attribute__((weak))` and `&global` initializers all
  landed, so the **whole emlibc rim — crt0 plus syscalls — compiles under EmbCC
  and runs on the OS**, exiting 42. EmbCC compiles all of emlibc, fdlibm
  floating point included.

## Advanced diagnostics

Diagnostics comparable to or better than modern compilers:

* Colored error messages.
* Precise source locations (file:line from M2 on).
* Helpful suggestions and fix-it hints.
* Warnings with clear explanations.
* Notes showing where an error originated — including through macro expansion,
  which the newlib headers will exercise hard.

Rich diagnostics for C++ templates belong to the intended C++ frontend
(D-008) — which the roadmap honestly calls "a different project in size": a
goal after the C compiler closes the M4 loop, not a milestone beside it.

## Optimization framework

A modern optimization pipeline is post-M4 work by design: the ROADMAP has no
performance milestone before the total loop closes, because an optimizer bolted
onto a compiler that has never run a program optimizes the wrong thing.
Candidates, roughly in order of payoff-per-complexity:

* Constant folding.
* Dead code elimination.
* Copy propagation.
* Common subexpression elimination.
* Function inlining.
* Loop optimizations.
* Strength reduction.
* Peephole optimization.
* Register allocation improvements.
* Link-time optimization (LTO) — natural here, since the linker is already
  in-process (ARCHITECTURE §1).
* Profile-guided optimization (PGO).

Adopting SSA and a pass manager is the likely architectural step when this
begins; that revision goes through DECISIONS when it happens.

## Compiler infrastructure

EmbCC exposes reusable compiler components — this is already the committed
source layout (ARCHITECTURE §7), not a future refactor:

* Lexer (`src/lex`)
* Preprocessor (`src/cpp`)
* Parser / AST (`src/parse`)
* Semantic analyzer (`src/sema`)
* Intermediate representation — EmbIR (`src/ir`)
* Optimizer (`src/opt`, over EmbIR — landed: folding, strength reduction,
  value numbering/CSE, copy propagation, DCE; register allocation and
  stack-slot coalescing live in codegen)
* Code generator (`src/codegen`, `src/asm`)
* Linker (`src/link`)

The modular architecture lets future tools reuse the frontend without
reimplementing the compiler — with one standing constraint: everything stays
linkable into **one process**, because the OS has no `fork`/`exec`
(ARCHITECTURE §1). Reuse means libraries, never a `cc1`-style tool farm.

## Developer tooling

EmbCC should integrate naturally with EmStudio — the planned EmbLinkOS
development platform, in the role Xcode plays for Apple — and other tools,
through compiler services:

* Syntax-aware error reporting.
* Code completion.
* Static analysis.
* Refactoring support.
* Symbol indexing and source navigation.
* Documentation extraction.

Gate: this work starts only when EmStudio (or another real consumer) exists to
drive it — a service API designed without a client is guessed, not designed.

## Static analysis

Beyond compilation, EmbCC should help developers write safer code:

* Uninitialized variable detection.
* Buffer overflow warnings.
* Integer overflow analysis.
* Memory leak detection.
* Null pointer analysis.
* Unreachable code detection.
* Undefined behavior diagnostics.
* Thread-safety analysis (future).

THE RULE applies to analysis too: a checker ships when it genuinely finds the
class of bug it names, at a false-positive rate a human tolerates — not as a
checkbox.

## Debug information

Rich debugging information for native tools: source-level debugging, variable
and function metadata, stack unwinding, optimized-code debugging. DWARF is an
explicit early non-goal (ARCHITECTURE §8); it becomes real when an EmbLinkOS
debugger exists to consume it — likely alongside EmStudio.

## Cross compilation

Primarily x86-64, permanently one target until D-006's bar is met. If
EmbLinkOS itself ever runs on another architecture, EmbCC should be able to
follow:

* x86-64 (the target)
* AArch64 (candidate)
* RISC-V (candidate)

The gate is explicit: a new backend is justified by EmbLinkOS running on that
architecture, not by backend-collecting.

## Extensibility

Designed so future capabilities land without architectural upheaval:

* Custom optimization passes.
* Plugin support.
* Additional language frontends targeting EmbIR — concretely **C++**, the one
  additional language actually intended (D-008): it is the wall TCC will never
  clear, and the OS already ports C++ software. A novel language of our own is
  not planned (D-008 demoted it; D-002's ten-lines test remains the only way
  back).
* Whole-program analysis.
* Incremental compilation.
* Distributed compilation (note: on-OS this must respect the one-process
  model; "distributed" means across machines via EmbBuild, not sub-processes).
* Build cache integration.
* Sanitizers, fuzzing instrumentation, coverage instrumentation.

## Long-term vision

EmbCC aims to become the complete native compiler infrastructure for
EmbLinkOS — not only compiling C programs, but providing the foundation for
development tools, debugging, optimization, static analysis, IDE integration,
and future language support through a shared compiler architecture.

None of it is on a schedule, and all of it is downstream of the milestones
that make it real: a program the OS runs, a compiler that compiles itself, an
OS that builds its own toolchain. The expansive picture is earned through the
restrained one.
