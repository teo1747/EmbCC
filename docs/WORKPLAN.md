# Work plan — three streams, one loop

*Written 2026-07-20, when the project became a team of three. This is the
division of labor that keeps everyone off each other's critical path. It is
a working document: when reality disagrees with it, update it and note why
in DECISIONS.md if the disagreement was architectural.*

**Current state when this plan starts:** M1 closed on the OS (exit 42).
M2 in progress — control flow, the full int operator set, prototypes and
PLT32 relocations are done; `putchar` works through the real linker.
20/20 tests green. Everything so far lives in one stream.

## Why three streams and not three people on one

The language core is **serial**: every phase downstream of the type system
takes its shape from it, so parallelizing "types" produces merge conflicts,
not speed. What parallelizes is the work that *consumes stable contracts*:
the linker consumes the object format (frozen by the gABI and TARGET_ABI),
and the proving ground consumes the compiler's CLI (frozen since M1). Those
two contracts are the stream boundaries.

---

## Stream A — the language core

**Scope:** lex, parse, sema, EmbIR, codegen, and eventually the
preprocessor. The pace-setter for M2.

**Order of work (each increment = tests + commit, as until now):**
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
- **EmbLD** (stream B) gains a second output shape: ET_EXEC ELF *and*
  EMBX APP, from the same linked image. Both are "write the layout you
  already computed, in a different container."
- **`mkembx.py`** (in the OS tree) is the bridge until then: it
  repackages a fully-linked ELF into an `.embx`. It is not a stopgap to
  be ashamed of — it is exactly the right tool for the window where the
  linker does not exist yet, and it is what proves the format works.
- **The capability table** is the one thing that has no ELF source. Today
  it comes from `mkembx.py --cap NAME`. The end state is the *program*
  declaring its own authority in source, EmbCC recording it, EmbLD
  collecting it — a real compiler-side feature, and the only part of
  EMBX that reaches back into the frontend.

This *raises* stream B's priority: it now gates the native format as well
as self-hosting. It does not change stream A's order.

**Tool family, mapped onto the streams** (the names from the OS side):

| Tool | What it is | Where it lives |
|---|---|---|
| `embread` | EMBX dumper + verifier | **done** — `tools/embread/`, EMBX spec §9 |
| `EmbLD` | the integrated linker; emits ELF ET_EXEC and EMBX | stream B |
| `emlibc` | the OS's own non-POSIX libc | DECISIONS D-009, deferred; OS-side requirements exist |
| `EmbDBG` | debugger | design recorded (`docs/EMBDBG_Requirements.md`, D-010): DWARF line info first (host-debuggable via gdb, no EmbDBG needed), native `.embdbg` derived later. After M3. |

## Stream B — the integrated linker (M3's long pole, started now)

**Scope:** `src/link/` — ELF reading, symbol resolution, section merging,
relocation application, archive (.a) handling, ET_EXEC emission for the
EmbLink loader. One process, library-shaped (ARCHITECTURE §1: no fork/exec
on the target — the linker is a function the driver calls, never a
subprocess).

**Why it can start today:** its input format is already frozen — it links
objects produced by *gcc and TCC* long before EmbCC's own objects need it.
Its spec is TARGET_ABI §4, and every expensive fact there is a ready-made
test case:

- [x] `R_X86_64_PLT32` resolved as plain `PC32` in a static link
      (TCC patch 0001 — 407 wild jumps say hello)
- [ ] the GOT is built AND filled at link time when GOTPCREL appears
      (TCC patch 0003 — newlib's `_impure_ptr`, `CR2=0` on first stdio).
      Not yet exercised: the M1 program pulls no GOTPCREL member — the
      next increment, needed for a stdio program
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

**Next (B2), toward self-hosting (M3):** the GOT (GOTPCREL, built AND
filled — the first stdio program needs it, TCC patch 0003); real
`__init_array` bracket symbols (B1's weak-→0 is correct only because the
array is empty — a program with constructors needs the true bounds);
COMMON placed into a synthetic `.bss`; then link EmbCC's OWN sources and
close the stage1/stage2 fixed point.

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

1. **The on-OS harness.** Automate what M1's closure did by hand: take an
   object, link it (cross toolchain now, stream B's linker later), stage
   it via `tools/os-stage.sh`, boot, run, capture the exit code/output,
   report PASS/FAIL. Wire it as `make test-os` — kept OUT of `make test`
   (host iteration stays seconds; the OS is the final judge, not the
   gatekeeper of every commit).
2. **The corpus.** Grow `tests/golden/` toward real code: candidate
   programs from `/data/src`, reduced to the current subset, each diffed
   against gcc (exit code + stdout, as agrees-with-gcc.sh already does).
   The corpus is what stream A compiles *next*, so C effectively writes
   A's requirements.
3. **Diagnostics polish.** Colors when stderr is a tty, caret lines,
   "did you mean" — all inside `diag_fatal`'s one chokepoint
   (src/driver/util.c), deliberately isolated for exactly this work.
   VISION_LONGTERM's diagnostics section is the spec; M2's "a human can
   act on it" is the bar.
4. **Fuzzing the refusal boundary** (cheap, valuable): throw generated
   not-yet-supported C at embcc and assert it always *fails with a
   diagnostic* — never crashes, never silently emits an object. THE RULE,
   mechanized.

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

## The sync point

The three streams meet at **M3**: A's compiler compiles EmbCC's own
source, B's linker links it, C's harness proves stage2 is byte-identical
to stage1 and still passes every M1/M2 test. Nothing before that forces a
meeting; nothing after M4 needs one.
