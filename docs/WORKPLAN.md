# Work plan — three streams, one loop

*Written 2026-07-20, when the project became a team of three. This is the
division of labor that keeps everyone off each other's critical path. It is
a working document: when reality disagrees with it, update it and note why
in DECISIONS.md if the disagreement was architectural.*

**Current state (2026-09-08).** All three streams have delivered what this plan
set them, and the M3 sync point they were aimed at is closed:

- **Stream A** — the language core is done through the C the corpus needs:
  the full type system, the whole statement and expression grammar, C11 and the
  GNU extensions, floating point, and a preprocessor that eats real newlib
  headers *and* the kernel's. Past its original scope it also grew the optimizer
  (`src/opt`, local + global with SSA on demand) and caret diagnostics.
- **Stream B** — **EmbLD is done and doing more than the plan asked.** It links
  EmbCC itself, it links the kernel (linker-defined end symbols, higher-half
  `p_paddr`), and it emits **EMBX** with a capability table, on the host and on
  the OS. **EmbAS** joined it: the standalone NASM/Intel assembler, byte-
  identical to nasm on all 6 kernel `.asm`. The toolchain owns the whole build.
- **Stream C** — the suite is **102/102** and gates every merge; the corpus grew
  from candidate programs to *the whole EmbLinkOS kernel* (89 units) and emlibc
  including fdlibm; the diagnostics polish item shipped; and the optimizer got
  an IR verifier (`EMBCC_VERIFY=1`) after a miscompile proved the suite alone
  was not enough.

**The one named milestone still open is M4's OS half** — running EmbBuild on the
metal from `build.ebm`. It needs all three streams only in the sense that all
three are already finished for it; the work is orchestration.

**What each stream is doing now** is at the bottom of this file, under
[Where the streams go next](#where-the-streams-go-next).

## Why three streams and not three people on one

The language core is **serial**: every phase downstream of the type system
takes its shape from it, so parallelizing "types" produces merge conflicts,
not speed. What parallelizes is the work that *consumes stable contracts*:
the linker consumes the object format (frozen by the gABI and TARGET_ABI),
and the proving ground consumes the compiler's CLI (frozen since M1). Those
two contracts are the stream boundaries.

---

## Stream A — the language core

**Scope:** lex, parse, sema, EmbIR, codegen, and the preprocessor. The
pace-setter for M2.

**Order of work as planned — all six steps are done** (each increment = tests +
commit, as throughout):
1. **Types I:** `char`, `unsigned`, `long`, pointers, `sizeof`, casts.
   The type representation decided here is the project's single most
   load-bearing data structure — expect this to be slow and reviewed.
2. **Types II:** arrays, string literals, `.rodata`, data relocations
   (R_X86_64_PC32/GOTPCREL against data symbols). Unlocks `puts`/`printf`.
3. **Globals:** file-scope variables, `.data`/`.bss`, `static`/`extern`
   linkage for objects.
4. **Structs/unions/enums**, member access, layout per SysV.
5. **Preprocessor:** #include, #define (object + function macros),
   conditionals — wired to the 348-macro table from M0 and judged by one
   test only: real newlib headers (ARCHITECTURE §5).
6. **M2 acceptance:** compile `tally.c` + the `sval` SDK from `/data/src`,
   run the oracle on the OS (`ls / | tally | get rows` vs `ls / | count`).

**Contract A exports:** the CLI (`embcc -c FILE.c -o FILE.o`) and the
object shape (ET_REL, .text/.rela.text/.symtab). Both are stable; stream B
and C build against them without asking.

## The EMBX finding: the linker owns the native format

*Added 2026-07-24, when EMBX landed (DECISIONS D-003, revised).*

EMBX changes the toolchain's shape less than it looks, and the reason is
one line of the spec: **an APP is "fully linked, fixed virtual addresses,
no relocations" (§4.1).** EmbCC emits *relocatable objects*. So the thing
that can produce an `.embx` is whatever performs the final link — never
the compiler proper.

**Therefore "EmbCC emits EMBX" means "EmbLD emits EMBX."** Concretely:

- **EmbCC** keeps emitting ELF relocatable objects. Unchanged by EMBX.
- **EmbLD** (stream B) gained a second output shape: ET_EXEC ELF *and*
  EMBX APP, from the same linked image. Both are "write the layout you
  already computed, in a different container." **Done** —
  `embld --embx --cap NAME`, byte-identical to the reference producer, on the
  host and on the OS.
- **`mkembx.py`** (in the OS tree) was the bridge: it repackages a fully-linked
  ELF into an `.embx`. Not a stopgap to be ashamed of — exactly the right tool
  for the window before the linker existed, and what proved the format worked.
  EmbLD has since taken over the job.
- **The capability table** is the one thing that has no ELF source. It comes
  from `--cap NAME` on the link line today. The end state is the *program*
  declaring its own authority in source, EmbCC recording it, EmbLD collecting
  it — a real compiler-side feature, still open, and the only part of EMBX that
  reaches back into the frontend.

This *raised* stream B's priority: it gated the native format as well as
self-hosting. It did not change stream A's order.

**Tool family, mapped onto the streams** (the names from the OS side):

| Tool | What it is | Where it lives |
|---|---|---|
| `embread` | EMBX dumper + verifier | **done** — `tools/embread/`, EMBX spec §9 |
| `EmbLD` | the integrated linker; emits ELF ET_EXEC and EMBX | **done** — `src/link` + `tools/embld/`; links EmbCC and the kernel, on host and OS |
| `EmbAS` | standalone NASM/Intel assembler | **done** — `src/as` + `tools/embas/`; byte-identical to nasm on all 6 kernel `.asm` (A1) |
| `emlibc` | the OS's own non-POSIX libc | **EmbCC compiles it**, fdlibm floating point included, and it self-hosts on the OS (D-009) |
| `EmbDBG` | debugger | **v0 done** — `embcc -g` emits DWARF-4 line/frame/locals and `tools/embdbg` reads it back (symbolize, inspect, disassemble, TUI), no gdb needed. `docs/EMBDBG_Requirements.md` tracks what is past v0. |

## Stream B — the integrated linker (M3's long pole — delivered)

**Scope:** `src/link/` — ELF reading, symbol resolution, section merging,
relocation application, archive (.a) handling, ET_EXEC emission for the
EmbLink loader. One process, library-shaped (ARCHITECTURE §1: no fork/exec
on the target — the linker is a function the driver calls, never a
subprocess).

**Why it could start before the compiler was ready:** its input format was
already frozen — it links objects produced by *gcc and TCC* long before EmbCC's
own objects needed it. Its spec is TARGET_ABI §4, and every expensive fact there
was a ready-made test case. All of them now pass:

- [x] `R_X86_64_PLT32` resolved as plain `PC32` in a static link
      (TCC patch 0001 — 407 wild jumps say hello)
- [x] the GOT question, **answered by measurement rather than built on
      spec**: this newlib (`newlib-c99`) emits *zero* GOTPCREL relocations and
      zero COMMON in libc.a — verified, so no GOT was written (D-006 / THE
      RULE). It slots in as an output section the day a corpus produces one.
      TARGET_ABI §3's `_impure_ptr` GOT fact came from a different newlib build
- [x] weak undefined symbols bind to 0, no relocation emitted
      (crt0's `__init_array_start`/`__tls_*` bracket symbols — how B1
      linked with no linker-script-defined symbols)
- [x] archive semantics: members pulled to satisfy undefined symbols,
      iterated to a fixed point (libc.a's two-way deps); dead members
      excluded, proven byte-identical to linking the live members alone
- [x] output: ET_EXEC (never PIE), correct e_entry via `_start`,
      two PT_LOAD segments (W^X) the in-kernel loader maps

**B1 — DONE (2026-07-24).** EmbLD linked the M1 program
(`crt0.o + syscalls.o + EmbCC-object + libc.a`) into an ET_EXEC, and the
EmbLinkOS kernel ran it: `[syscall] exit code=0x2A` — exit 42, no
cross-ld anywhere. Proven on the host first (structure vs cross-ld from
identical inputs — the data segment byte-for-byte the same; D-005), then
on the OS. `tests/golden/embld-b1.sh` keeps the host half green;
`embld-link.sh` covers the linker's mechanics.

**B2 — DONE (2026-07-24).** Output-section grouping (input sections
merged by name, the way a linker script's `*(.init_array)` does), so:
- **`__init_array`/`ctors` bracket symbols** are the real group bounds —
  a program with constructors runs them (proven: a ctor sets a global,
  `_start` walks the brackets, exits 42; with B1's weak-→0 it would have
  exited 0). Closes the B1 caveat.
- **COMMON** (tentative defs) placed into `.bss` and usable.
- **The GOT was found NOT to be needed:** this newlib (`newlib-c99`) has
  **zero GOTPCREL relocations** and zero COMMON in libc.a — verified,
  not assumed. TARGET_ABI §3's `_impure_ptr` GOT fact was a *different*
  newlib build. So no GOT was built: D-006 / THE RULE — a feature no
  input needs is not written. It slots in as an output section
  (OSEC_GOT) the day a corpus actually produces GOTPCREL.

Bonus validation on the OS: an EmbCC-compiled, EmbLD-linked **printf**
program printed `hello from embld 42` and exited — the whole libc stdio
path (printf -> vfprintf -> _write -> syscall) works through our linker,
not just exit-42.

**B3 and past it — DONE.** The self-hosting loop closed: EmbCC compiles its own
sources, EmbLD links them, and the stage1/stage2 fixed point holds — 16/16
byte-identical, on the host and **on the OS** (`test embcc selfhost` compiles
*and relinks* on the metal, no cross-`ld` and no tcc in the loop). Past M3,
EmbLD gained EMBX output with a capability table, linker-defined end symbols and
higher-half LMA — the two features that let the **kernel** link with no external
tools. EmbAS closed the last one (nasm).

**Contract B also consumes:** `src/embx/embx.h` — the EMBX container,
byte-exact, mirroring the kernel's `embx.h` (which is the authority).
`embread` reads what EmbLD will write, so it is stream B's verifier from
day one: produce an image, run `embread` over it, and every §8 guard is
checked on the host before the OS ever sees it.

**Contract B consumes:** `src/elf/elf.h` (shared structures — additions
coordinated in review, never forked). **Nothing in stream B may block on
stream A**, and vice versa.

## Stream C — the proving ground

**Scope:** everything that makes a claim checkable.

1. **The on-OS harness — done.** Take an object, link it (with EmbLD now, not
   the cross toolchain), stage it via `tools/os-stage.sh`, boot, run, capture
   the exit code/output, report PASS/FAIL. Kept OUT of `make test` (host
   iteration stays seconds; the OS is the final judge, not the gatekeeper of
   every commit). The kernel oracles `test embcc self` and `test embcc selfhost`
   are the sharpest instances.
2. **The corpus — done, and it kept escalating.** From candidate programs in
   `/data/src`, to TinyCC and newlib (~500 files), to **emlibc including
   fdlibm**, to **the whole EmbLinkOS kernel** (89 units). Each step was diffed
   against gcc. The corpus is what stream A compiles *next*, so C effectively
   wrote A's requirements — `docs/todo.md` is that ranking, and what remains of
   it is the language gap list.
3. **Diagnostics polish — done.** Colour when stderr is a tty, caret lines with
   real columns, span underlines, "did you mean?", and notes for previous
   declarations and macro expansions — all inside `diag_fatal`'s one chokepoint
   (src/driver/util.c), deliberately isolated for exactly this work. Semantic
   errors get carets too, not just syntax ones.
4. **Fuzzing the refusal boundary** — *still open*, and still cheap and
   valuable: throw generated unsupported C at embcc and assert it always *fails
   with a diagnostic* — never crashes, never silently emits an object. THE RULE,
   mechanized. The nearest thing shipped so far is the optimizer's IR verifier
   (`EMBCC_VERIFY=1`), which mechanizes the same instinct one layer down.

## Process — what keeps three people fast

- **`make test` green is the merge contract.** No green, no merge, no
  exceptions — the suite is what lets three people touch a compiler
  without silently breaking each other.
- **Branch per increment, one review before merge to `Teo`.** The
  reviewer checks two things above all: does anything claim more than it
  does (THE RULE), and does anything foreclose a VISION_LONGTERM seam.
- **DECISIONS.md is where arguments end.** Three people means three
  opinions; an ADR with rationale and a reopen-condition stops the same
  debate from re-running weekly. Cross-stream interface changes (elf.h,
  the CLI) get a decision line, not a drive-by edit.
- **Commit style stays as is:** `area: summary`, why over what, name what
  is NOT handled, never a Co-Authored-By line (CONTRIBUTING).
- **When a target fact costs a debugging session, it goes into
  TARGET_ABI.md as an expensive fact** — that file is why the next person
  pays once.

## The sync point — reached

The three streams met at **M3**, as planned: A's compiler compiled EmbCC's own
source, B's linker linked it, C's harness proved stage2 byte-identical to stage1
and still passing every test. Closed on the OS 2026-07-24, and it has held
through every change since — 16/16 sources, with `-O0` output byte-identical by
construction so the fixed point survives optimizer work.

## Where the streams go next

The plan's structure still holds, so the streams keep their boundaries:

- **A (core)** — the remaining C gaps, each currently refused loudly rather than
  miscompiled: VLA, `_Complex`, 80-bit `long double`. Past those, codegen
  quality is open-ended; `-O2` `.text` is at 1.63× gcc `-O0` and the headroom
  is real.
- **B (linker/format)** — the capability table declared *in source* rather than
  on the link line, which is the one part of EMBX that reaches into the
  frontend, and therefore the one item needing an A/B handshake. Dynamic-linking
  output stays a candidate, not a commitment (D-006).
- **C (proving ground)** — M4's OS half is C-shaped work: ship the tree and
  `build.ebm` to `/data/src/embcc/`, run the OS's EmbBuild, confirm exit 42. The
  same for the kernel manifest. Then the refusal-boundary fuzzing above.

Nothing here forces another three-way meeting. M4 is orchestration over
contracts that are already stable.
