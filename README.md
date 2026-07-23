# EmbCC — a native C compiler for EmbLinkOS

**Status: M1 complete — EmbLinkOS ran EmbCC's output (exit 42, 2026-07-20).
M2 in progress.** The decision record below still governs. `embcc -c` compiles
a growing C subset to genuine x86_64-elf relocatable objects, cross-checked
against gcc on the host and proven on the OS. No preprocessor yet (M2 brings
it), no linker yet (M3); nothing in EmbLinkOS depends on this.

EmbCC is the intended *native* C compiler for **EmbLinkOS** — a compiler written
for, and eventually *by*, the OS itself. It is the next ring of ownership after
the kernel, the filesystem, the shell, and the build tool: the point at which
even the toolchain that produces the OS's programs belongs to the OS.

## This is not today's compiler

EmbLinkOS already hosts **TCC** (with four local patches) and builds real C on
itself: static tools, the shell rebuilding the shell, EmbBuild rebuilding
EmbBuild. TCC + ELF work, ship, and are not going anywhere soon.

EmbCC is deliberately a **separate, parallel project** so the OS stays honest
and shippable while the compiler grows on its own clock — until it is genuinely
good enough to earn adoption. That is exactly how TCC and EmbBuild arrived:
prove the thing standalone, adopt when it is real. Adoption, when it comes, is a
one-line change in an EmbBuild manifest.

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

- **A C compiler first, not a new language.** Compiling a subset of C to the
  EmbLink ABI makes every increment testable *on the OS* the day it can emit a
  valid object. A language of its own is a possible future, not the opening move.
- **Emit ELF today; grow into EMBX + emlibc.** The *current* target is ELF
  linked against newlib — the shape the in-kernel loader binds (there is no
  `ld.so`; **the kernel is the linker**), and the substrate for porting foreign
  source. The *native* target EmbCC grows into (DECISIONS D-003/D-009) is
  **EMBX**, EmbLinkOS's own capability-carrying format
  (`myos/docs/EMBX_Specification_v2.md`, byte-exact, working loader), linked
  against **emlibc**, the OS's own non-POSIX libc
  (`myos/docs/EMLIBC_Requirements.md`). ELF stays as the porting lane — a dual
  *loader*, not a converter, mirroring EMBKFS-native-plus-FAT32 for disks. The
  earlier "ELF superset only" plan was **revised** once the capability model
  landed and the OS's author chose to own the format; D-003 records why.
- **The milestones are loops.** EmbCC compiles a program the OS runs (exit 42);
  then EmbCC compiles *itself*; then EmbBuild builds EmbCC from `/data/src` on
  the OS. Each is the self-hosting loop, one ring deeper.

## Documents

| Doc | What it is |
|---|---|
| [docs/VISION.md](docs/VISION.md) | Why a native compiler; the ownership thesis; the own-the-stack vs host-the-world tension |
| [docs/VISION_LONGTERM.md](docs/VISION_LONGTERM.md) | The post-M4 horizon: compiler infrastructure, tooling, optimization, analysis — each gated by D-006, none scheduled |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Decisions already made, each with its rationale (ADR-style) |
| [docs/TARGET_ABI.md](docs/TARGET_ABI.md) | **The grounding doc.** The exact EmbLinkOS contract EmbCC must emit — syscalls, crt0, and the precise ELF the in-kernel loader accepts |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Intended compiler structure and phases |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestones M0–M4, each with a concrete acceptance test |
| [docs/WORKPLAN.md](docs/WORKPLAN.md) | The team's three parallel streams (core, linker, proving ground) and the process that keeps them off each other's critical path |
| [CONTRIBUTING.md](CONTRIBUTING.md) | The discipline inherited from EmbLinkOS (prove on the host, selftest the invariant, THE RULE) |

## When work starts

Read [docs/ROADMAP.md](docs/ROADMAP.md) **M0** first. The first milestone emits an
ELF object for the EmbLink target and runs it on the OS; the compiler then grows
*backward* from that testable end rather than forward from a lexer. The reason is
in [CONTRIBUTING.md](CONTRIBUTING.md): in this project a thing is not done because
it compiles, it is done when a test exercises the invariant.
