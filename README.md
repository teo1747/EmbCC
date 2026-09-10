# EmbCC — a native C compiler for EmbLinkOS

**Status: the toolchain is real and builds the OS.** EmbCC compiles *itself*
(the self-hosting fixed point holds over all 16 sources), and the entire
EmbLinkOS **kernel** — 89 C translation units through `embcc`, 6 hand-written
`.asm` through `embas`, linked by `embld` — builds and **boots to the home
desktop** with no gcc, no nasm and no `ld` anywhere in the loop.

**EmbCC now emits for two machines.** EmbLinkOS became two architectures when
its aarch64 campaign closed (`myos/docs/ARM64.md`), and `--target=aarch64-elf`
answers it: one binary, two backends, chosen at run time (D-011). **60 of the
68 executable tests compile for aarch64 and RUN on it** under
`qemu-system-aarch64`. The eight that do not are refused loudly, not
miscompiled — see "Where aarch64 stands" below.

`embcc -c` compiles C to genuine x86_64-elf relocatable objects, cross-checked
against gcc on every test: the integer and floating types, pointers (incl.
function pointers), arrays, structs/unions/enums, bitfields, globals, the full
operator and statement set, C11 (`_Alignof`/`_Alignas`/`_Atomic`/`_Generic`/
`_Static_assert`), and the GNU extensions the kernel needs — statement
expressions, computed `goto`, `typeof`, `__attribute__`, `__builtin_*`, and a
real inline-asm assembler. Its preprocessor digests **real newlib headers**, so
`#include <stdio.h>` compiles, links and runs.

It has a genuine optimizer (`-O1`/`-O2`: SSA mem2reg, inlining, SCCP,
dominator-scoped global CSE, redundant-load elimination, strength reduction, and
a Chaitin-Briggs register allocator), clang-style caret diagnostics, and DWARF-4
debug info (`-g`) that our own **EmbDBG** reads back. **EmbLD** links, emitting
ET_EXEC ELF and the native **EMBX**; `embread` verifies EMBX images; **EmbAS**
assembles NASM/Intel source byte-identically to nasm.

The decision record below still governs.

**On test counts, honestly.** `make test` was 102/102 on the Linux x86-64 host
it was written on, where the host *was* the target: a test compiled with
`embcc`, linked with the host `cc`, and ran. On the Apple Silicon development
machine that is no longer true — x86-64 ELF objects neither link nor run there
— so `make test` reports **20/103**, and the 83 that fail all fail at
`ld: unknown file type`, not at anything EmbCC emitted. `make test-arm64` is
**60/68** and is currently the only suite on that machine that actually
executes compiled code; restoring the x86-64 half needs the same
QEMU treatment (see "What's next").

## Where it stands next to TCC

EmbLinkOS also hosts **TCC** (with four local patches), and TCC remains what the
OS ships by default: it works, it builds real C on the metal, and nothing is
being ripped out on a schedule.

What has changed is that EmbCC is no longer the speculative half of that pair.
It clears walls TCC does not — it compiles the **kernel**, it emits the native
**EMBX** format with a declared capability table, it produces debug info, and it
optimizes. Adoption remains what it always was: a one-line change in an EmbBuild
manifest, made when a concrete need makes it the better tool (DECISIONS D-006),
not because we wrote it.

EmbCC still develops in its own repository on its own clock (D-001), so the OS
stays shippable and honest while the compiler moves.

## Why it exists

An OS is defined by the code that owns the machine and its contracts, and all of
that is already EmbLinkOS's own. Building it with gcc/TCC/ELF no more dilutes
that ownership than writing the kernel in C does — tools are leverage, not
authorship.

But there is a rarer form: the closed, self-consistent loop where language,
compiler, format, loader, and OS are one thing, beholden to nothing external.
Oberon did it. TempleOS did it. EmbCC is a deliberate step toward that loop.

Note the tension honestly: this points *opposite* to EmbLinkOS's ports story
(git, CPython, C++, TCC — "meet the existing software world on its own terms and
refuse to fake it"). One soul says *host the world*, the other *own the stack*.
Both are legitimate; EmbCC is the second, entered with eyes open. See
[docs/VISION.md](docs/VISION.md).

## The shape of the plan

- **A C compiler, not a new language.** Compiling C to the EmbLink ABI made
  every increment testable *on the OS* from the day it could emit a valid
  object. A language of our own is not planned (D-008); C++ is the one intended
  second language.
- **Both ELF and EMBX, today.** ELF linked against newlib is the shape the
  in-kernel loader binds (there is no `ld.so`; **the kernel is the linker**) and
  the substrate for porting foreign source. The *native* target (DECISIONS
  D-003/D-009) is **EMBX**, EmbLinkOS's own capability-carrying format
  (`myos/docs/EMBX_Specification_v2.md`, byte-exact, working loader), linked
  against **emlibc**, the OS's own non-POSIX libc
  (`myos/docs/EMLIBC_Requirements.md`). ELF stays as the porting lane — a dual
  *loader*, not a converter, mirroring EMBKFS-native-plus-FAT32 for disks. The
  earlier "ELF superset only" plan was **revised** once the capability model
  landed and the OS's author chose to own the format; D-003 records why.
- **The milestones are loops.** EmbCC compiles a program the OS runs (exit 42
  — M1, closed); then EmbCC compiles *itself* (M3, closed); then EmbBuild builds
  EmbCC from `/data/src` on the OS (M4 — the manifest exists and builds it on
  the host; running it on the metal is the open step). Each is the self-hosting
  loop, one ring deeper.

## Documents

| Doc | What it is |
|---|---|
| [docs/VISION.md](docs/VISION.md) | Why a native compiler; the ownership thesis; the own-the-stack vs host-the-world tension |
| [docs/VISION_LONGTERM.md](docs/VISION_LONGTERM.md) | The horizon past the named milestones: C++, deeper analysis, compiler services — gated by D-006. Optimization and diagnostics have since landed off this list; see `src/opt`, `src/codegen`, `src/driver/util.c` |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Decisions already made, each with its rationale (ADR-style) |
| [docs/TARGET_ABI.md](docs/TARGET_ABI.md) | **The grounding doc.** The exact EmbLinkOS contract EmbCC must emit — syscalls, crt0, and the precise ELF the in-kernel loader accepts |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Intended compiler structure and phases |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M4, each with a concrete acceptance test, and what is open past them |
| [docs/USAGE.md](docs/USAGE.md) | The `embcc`/`embas`/`embld`/`embdbg` CLI reference |
| [tests/harness/](tests/harness/) | The aarch64 proving ground: a bare-metal QEMU `virt` image with an ARM-semihosting syscall floor, so compiled code is RUN on the architecture it was compiled for |
| [docs/todo.md](docs/todo.md) | The evidence-backed completeness audit: what C we do not yet compile, ranked by a real corpus |
| [docs/WORKPLAN.md](docs/WORKPLAN.md) | The team's three streams (core, linker, proving ground), what each is working on now, and the process that keeps them off each other's critical path |
| [docs/EMBDBG_Requirements.md](docs/EMBDBG_Requirements.md) | Producer-side debug-info requirements + the DWARF-bridge decision (D-010); the byte format & kernel contract live OS-side in `myos/docs/EMBDBG_Specification.md` |
| [src/embx/embx.h](src/embx/embx.h) | The EMBX container, byte-exact — mirrors the kernel's loader header; written by EmbLD, read by `embread` |
| [CONTRIBUTING.md](CONTRIBUTING.md) | The discipline inherited from EmbLinkOS (prove on the host, selftest the invariant, THE RULE) |

## Where to start reading

For the *why*, read [docs/VISION.md](docs/VISION.md), then
[docs/DECISIONS.md](docs/DECISIONS.md) — the arguments are settled there, with
their reopen conditions.

For the *how*, read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the phase
structure, then [docs/TARGET_ABI.md](docs/TARGET_ABI.md), which is the grounding
doc: the exact contract the OS enforces, and the expensive facts that cost a
debugging session each.

To build and run it:

```sh
make && make embdbg     # embdbg is not in `all`, and the golden tests need it
make test               # x86-64: needs a Linux x86-64 host to run the exec half
make test-arm64         # aarch64: compiles AND runs, under qemu-system-aarch64
```

`make test-arm64` needs `aarch64-elf-gcc`, `qemu-system-aarch64`, and an
aarch64 newlib (`EMBCC_AARCH64_NEWLIB`, default `~/cross/newlib-aarch64-c99`).
It links each test into a bare-metal image and runs it on QEMU's `virt`
machine — the same machine EmbLinkOS itself targets — with ARM semihosting
carrying stdout and the exit status back to the host. See
[tests/harness/aarch64/](tests/harness/aarch64/).

Then [docs/USAGE.md](docs/USAGE.md) for the CLI.

## Where aarch64 stands

Working, and proven by running it: the integer and floating types, pointers,
arrays, structs and unions by value (AAPCS64 — including the composite-return
rules and the hidden `x8` pointer), the full operator and statement set,
globals, string literals, computed `goto`, and calls both direct and through
function pointers. `--target=aarch64-elf` produces real `EM_AARCH64` ET_REL
objects with `R_AARCH64_CALL26` / `ADR_PREL_PG_HI21` / `ADD_ABS_LO12_NC` /
`ABS64` relocations that `aarch64-elf-ld` links against stock newlib.

Refused loudly, each with a diagnostic naming what is missing (THE RULE):

| Gap | Why it is not a small fix |
|---|---|
| Inline asm | EmbCC's assembler (`src/as`) is x86-64 NASM syntax. aarch64 needs its own, and the kernel's inline asm is the single biggest thing standing between this backend and compiling the ARM kernel. |
| `va_start` | AAPCS64's `va_list` is a five-field struct over a register save area, not SysV's `__va_list_tag`. Calling a variadic function (`printf`) already works — defining one does not. |
| Atomics | `__sync_*` lower to `ldxr`/`stxr` retry loops rather than a single locked instruction. |
| HFA struct arguments | A struct of floats is passed in up to four `v` registers by a rule with no SysV counterpart, so irgen's classification cannot express it. |
| `-g` | The DWARF emitter describes `rbp`-relative frame offsets; aarch64 slots are `sp`-relative. |

The backend is also naive where the x86 one is not: no slot coalescing, no
residency cache, no register allocator, so frames are wider and the code is
longer. That is the same order the x86 backend was built in (D-005), not an
oversight.

## What's next

- **An aarch64 assembler**, and with it inline asm — the gate on compiling the
  EmbLinkOS ARM kernel, which uses it throughout.
- **The x86-64 exec suite, restored on a non-Linux host.** The aarch64 harness
  (`tests/harness/`) shows the shape: a bare-metal image under
  `qemu-system-x86_64` with `isa-debug-exit` where aarch64 uses semihosting.
  Until then the x86-64 backend's regression cover on this machine is the
  golden tests — `self-host.sh` and `embbuild-kernel.sh` above all, which do
  compare real generated code.
- **M4's OS half** — ship the source and `build.ebm` to `/data/src/embcc/`, run
  the OS's own EmbBuild on it, and have that on-OS-built EmbCC compile the M1
  program to exit 42. The manifest and a host reference walker already exist;
  what remains is orchestration on the metal. This is the total loop, and the
  last named milestone.
- **The kernel, through EmbBuild on the OS** — the same step for the bigger
  prize; the two blockers (an on-OS assembler, `kernel_end`) are closed.
- **The C gaps that remain** — VLA, `_Complex`, 80-bit `long double`. Each is
  refused loudly today rather than miscompiled; `docs/todo.md` ranks them
  against a real corpus.
- **Past that, only if earned** (D-006): C++ as the second language (D-008),
  `__thread`/TLS, and dynamic-linking output. Candidates, not commitments.
