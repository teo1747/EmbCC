# EmbCC — Vision Beyond a C Compiler

*Status: **long-horizon picture**, not a plan. [VISION.md](VISION.md) states why
EmbCC exists and its discipline of restraint; [ROADMAP.md](ROADMAP.md) states
what is actually committed (M0–M4). This document records where the project
could go **after** the total loop closes — and every item here is governed by
DECISIONS D-006: a capability is added for a stated reason, never because a
compiler "should" have it. Where this document and a decision record disagree,
the decision record wins.*

EmbCC is more than a compiler that translates C source into machine code. Its
long-term goal is to become the complete native compiler infrastructure for
EmbLinkOS — deeply integrated with the operating system, the development tools,
and the build system.

## Core goals

* Support the C language, grown by need (D-002): the subset expands when a real
  program demands it, with "full C" as the limit of that process rather than an
  up-front target.
* Produce well-optimized native executables — *after* correct ones
  (ARCHITECTURE §3: correct-and-slow first; codegen quality is the first honest
  post-M4 reason to prefer EmbCC over TCC).
* Become self-hosting by compiling EmbCC with EmbCC (ROADMAP M3).
* Fast compilation without sacrificing code quality.
* Modern, clear, actionable diagnostics (this one is *not* deferred — M2
  requires diagnostics a human can act on).

## Native EmbLinkOS integration

EmbCC is designed specifically for EmbLinkOS and understands its architecture.

* Direct support for the EmbLinkOS ABI ([TARGET_ABI.md](TARGET_ABI.md)).
* **Executable format: ELF, always** — including any future native capability.
  Per D-003 (firm), a native format is an **ELF superset**: plain ELF the
  existing in-kernel loader already reads, plus `.note` sections a newer loader
  understands. Additive by construction — there is deliberately no second
  format, no migration, and no converter tax. The first candidate extension
  remains a declared capability manifest, gated on the capability model
  existing first.
* Tight integration with EmbBuild (M4 makes this real: EmbBuild builds EmbCC on
  the OS).
* Automatic discovery of the system SDK, runtime libraries, and headers
  (`/system/abi/include`, crt0/syscalls/libc as TARGET_ABI §4 lays them out).
* Native understanding of EmbLinkOS syscalls and services — starting with
  first-class fixed-register inline-asm constraints so the `__TINYC__`
  workaround in the syscall header can die (ARCHITECTURE §4).

## Advanced diagnostics

Diagnostics comparable to or better than modern compilers:

* Colored error messages.
* Precise source locations (file:line from M2 on).
* Helpful suggestions and fix-it hints.
* Warnings with clear explanations.
* Notes showing where an error originated — including through macro expansion,
  which the newlib headers will exercise hard.

Rich diagnostics for C++ templates belong to a possible C++ frontend — which
the roadmap honestly calls "a different project in size," not a milestone.

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
* Optimizer (future, over EmbIR)
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
* Additional language frontends targeting EmbIR — including the genuinely
  differentiated one: a language with EmbLink's typed values (records, tables,
  SQL-nulls) as first-class types, still gated on D-002's ten-lines test.
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
