# Roadmap

*Each milestone ends in a **concrete acceptance test**, because in this project
a thing is not done when it compiles — it is done when a test exercises the
invariant. Milestones are ordered by what they *prove*, not by how much code
they contain.*

**Current position (2026-07-26): M3 closed, and the compiler has grown well
past it — it emits the native format, builds the OS's own libc (floating point
included), and gained an optimizer.** The one *named* milestone still open is M4
(EmbBuild builds EmbCC); everything below it in this file has landed, and a good
deal the original milestones did not anticipate landed alongside.

**Since M3 — the axes the milestones did not name:**

- **The fixed point still holds, now over fifteen sources.** The optimizer
  (`src/opt/opt.c`) joined the compiler, so the self-host source set grew 12 →
  15; `test embcc self` is **15/15 byte-identical on the OS**, opt included.
- **EmbLD runs on the OS, and closes the loop *with the link*.** Cross-built into
  an EmbLinkOS binary (`embld.elf`), it makes `test embcc selfhost` have the OS
  compile all fifteen sources **and relink them** into a working `embcc`,
  byte-identical to the host build — no cross-`ld`, no tcc in the loop. (M3
  proved object determinism; this proves the whole bootstrap on the metal.)
- **The native format is emitted by the toolchain.** EmbLD emits **EMBX**
  directly (`embld --embx --cap NAME`, byte-identical to the reference producer),
  on the host and **on the OS** (`test embld embx`). EmbCC compiles a program
  from source, EmbLD emits it as EMBX, and the loader births it holding exactly
  its declared capabilities (`test embcc embx`). The D-003-revised target, reached.
- **It builds the OS's own libc.** EmbCC compiles **emlibc** — EmbLinkOS's
  non-POSIX C library — on the OS, and emlibc self-hosts: `test emlibc math
  selfhost` compiles the whole libc *including real `fdlibm` floating point*
  (38 units) with EmbCC, links with EmbLD, and runs at 1e-12. Closing that took
  **floating-point codegen** (SSE, the XMM ABI) and a corpus-driven completion
  pass over the language — bitfields, designated array initializers, compound
  literals, a real `_Bool`, `_Static_assert`/`_Generic`/anonymous members,
  block-scope `extern`/`typedef`, weak undefined references, unsigned-64↔double —
  full suite green (80/80), the self-host fixed point holding throughout.

The essence of M4 is partly here — the OS *does* compile and link EmbCC's own
sources on itself (`test embcc selfhost`, the kernel oracle) — but doing it
*through EmbBuild from a manifest*, the total-loop framing below, is still the
open step.

---

**M3 — how it closed (2026-07-24; twelve sources at the time, fifteen now):**
The self-built compiler (`embcc.elf`,
itself compiled by EmbCC and linked by EmbLD) was staged to EmbLinkOS and,
under the kernel `test embcc self` oracle, **recompiled all twelve of its
own source files on the OS — every object byte-for-byte identical to the
reference the host's gcc-built embcc produced** (`12/12 objects
byte-identical`). A compiler and the compiler it produces agree exactly:
the classic bootstrap fixed point, closed on the target OS itself. STT_FILE
uses the source basename, so an object depends on content, not on the build
path, which is what lets host and OS objects match bit-for-bit.

Getting there, the on-OS runs earned D-005 twice over: the first `test
embcc` run (the M1 program compiled on the OS, tcc-linked, **exit 0x2A =
42**) page-faulted until a codegen bug all 54 host tests missed was fixed —
the 7th+ scalar parameter was read from a phantom register instead of the
incoming stack slot, so EmbCC's own ten-argument `codegen_unit` wrote a
NULL `*next` (tests/exec/many-params.c). With that fixed the compiler ran
clean, and then reproduced itself exactly.

**Host acceptance (also 2026-07-24):**
EmbCC now compiles **all twelve of its own source files**, and EmbLD
links them against the real crt0/syscalls/newlib into a well-formed
EmbLinkOS `ET_EXEC` (`embcc-stage1.elf`, ~704 KB) with every symbol
resolved — the compiler and the linker are both ours, end to end, for
EmbCC itself (tests/golden/self-host.sh). Codegen is deterministic (every
object byte-identical across runs — the property the fixed point rests
on). Closing the language for self-hosting took, in test-covered
increments: adjacent string concatenation, a variable in scope within its
own initializer, noreturn-tail control flow, relocatable aggregate
initializers for globals and static locals (the predef/keyword string
tables), struct field designators, and SysV variadic function definitions
with a register save area (diag_fatal). A latent uninitialized-read in the
IR builder — a garbage struct-return count that crashed ~half the time —
was found and fixed while bootstrapping. **The stage1==stage2
byte-identical fixed point is on-OS**: stage1 links against newlib for the
EmbLinkOS syscall ABI, so it runs on the OS, not the host — the OS is the
final judge (D-005), as M1 and M2 were.

Earlier M3 milestones (2026-07-24): EmbLD B1 linked the M1 program
(crt0 + syscalls + an EmbCC object + newlib's libc.a) into an ET_EXEC and
the kernel ran it to exit 42, no cross-ld involved. EmbLD B2 followed:
output-section grouping gives correct __init_array/ctors bracket symbols
(a constructor program runs its ctor) and COMMON placement, and an
EmbCC-compiled + EmbLD-linked printf program prints correctly on the OS.
The GOT was verified unneeded for this newlib (zero GOTPCREL).

**M2 COMPLETE — confirmed on the OS 2026-07-24.** The
acceptance test passed on EmbLinkOS itself: value.c, wire.c, sval.c and
tally.c (~1,030 lines of the real sval SDK, from the OS tree, UNMODIFIED)
compiled by EmbCC, linked into tally.elf against crt0/syscalls/newlib, packed
into the EMBKFS image, and run under the OS's own `test extern` oracle —
`ls / | tally | get rows` returns OK, including the extern|extern chain that
drives EmbCC's serialize AND deserialize paths. The installed binary was
427,480 bytes (the EmbCC build) versus the stock gcc build's 365,240, so it
was demonstrably EmbCC's code the kernel ran. That is the same
cross-check EmbBuild and TCC were each held to (ROADMAP M2). On the host, a
gcc-built harness drives the EmbCC-compiled SDK through a full
serialize/deserialize round trip and its output matches gcc's exactly
(tests/golden/emblinkos-sdk.sh).

**M1 COMPLETE — confirmed on the OS 2026-07-20.** The
exit-42 object compiled by `embcc -c`, linked against the real crt0/newlib,
was loaded by the EmbLinkOS kernel and exited 0x2A
(`/data/apps/embcc42/embcc42.elf`, pid 6, `[syscall] exit code=0x2A`). The
whole TARGET_ABI end of the system is validated while the compiler is still
small enough to change cheaply — which was the entire point of doing M1 before
a real frontend. M2 is in progress, growing the subset in test-covered
increments — control flow, the full operator set, externals with PLT32
relocations, and now the type system: char/short/int/long with unsigned
variants, pointers (arithmetic, comparison, deref/address-of, p[i]),
void returns, sizeof, casts, and C's exact conversion rules (promotions,
usual arithmetic conversions, signed narrowing, unsigned div/shift/compare)
— every implicit conversion materialized in the tree and cross-checked
against gcc. Arrays (incl. 2-D, decay, sizeof) and string literals in
.rodata landed next, with variadic external calls (al=0 per SysV):
**an EmbCC-compiled hello world runs — puts and printf work.** Globals
followed (.data/.bss, static/extern, OBJECT symbols), then the last big
language surface: structs/unions/enums with SysV layout and padding,
member access, typedef (incl. anonymous-struct form), enums as folded
int constants — struct assignment/params/returns refused pending SysV
classification. The preprocessor landed
next and passed its judge: REAL newlib headers — <stdint.h> and <stddef.h>
(the cdefs/_default_types chain that broke TCC) preprocess, compile, and
run through EmbCC, with EmbCC's own compiler headers (include/) covering
stddef/stdarg/stdbool/float. Function pointers landed next
(declarators, decay, indirect calls via r11, PC32 function-address
relocations), pulling ?:, the comma operator, and ++/-- on arbitrary
lvalues along with them — and with that, **the M2 goal sentence works: a
program that #includes <stdio.h> and calls the real libc compiles, links,
and runs** (host-proven; golden-tested). What remains: the tally.c + sval
acceptance on the OS itself.

**Target update, 2026-07-24 (DECISIONS D-003 revised):** EmbLinkOS now has a
native binary format, **EMBX**, with a working in-kernel loader and a live
capability contract. EmbCC's eventual output is EMBX; ELF stays as the porting
substrate. This does **not** change M1–M2 — an EMBX APP is fully linked
(spec §4.1), so the producer of an `.embx` is the LINKER, which makes M3's
integrated linker the gate for the native format as well as for self-hosting
(see WORKPLAN "The EMBX finding"). **Landed since:** `embread`, the EMBX
dumper/verifier, and then EmbLD's native **EMBX emitter** (`embld --embx --cap
NAME`) — byte-identical to the reference producer, checked against the OS's own
images, runs on the host and on the OS.

---

## M0 — Scaffolding and the host harness

**Goal:** a place to put code and a way to know it works.

- Build system (a plain Makefile; EmbCC is a host program at this stage)
- Test runner for `tests/` (see ARCHITECTURE §7)
- The **ELF writer skeleton** and the predefined-macro table (ARCHITECTURE §5) —
  both are on the critical path and both can be built before any parsing
- A `--version` that prints something honest

**Acceptance:** `make && make test` runs green with a trivial test, on the host.

*Note the ordering: this milestone deliberately contains no parser.*

---

## M1 — **Exit 42** (the milestone that matters)

**Goal:** the OS runs something EmbCC produced.

Compile a program small enough that the frontend can be crude — but real enough
that the object is genuine:

```c
static int twice(int x) { return x + x; }
int main(void) { return twice(21); }
```

- EmbCC emits a **relocatable ELF object** for `x86_64-elf` (linking is still
  done by the existing toolchain — DECISIONS D-004, ARCHITECTURE §6)
- The object links with `crt0.o + syscalls.o + libc.a` into an `ET_EXEC`
- The kernel loads it and it **exits 42**

**Acceptance:** on EmbLinkOS, the produced binary runs and exits 42 — the same
bar `test tcc link` set for TCC. On the host, `readelf`/`objdump` agree the
object is well-formed and the symbol table is sane.

**Why this is M1 and not M4:** it validates the entire TARGET_ABI end of the
system while the compiler is still small enough to change cheaply.

---

## M2 — A useful C subset, against real headers

**Goal:** compile programs that `#include <stdio.h>` and call the real libc.

- Preprocessor complete enough for newlib's headers (the full predefined-macro
  family — ARCHITECTURE §5 — is non-negotiable here)
- Types: integers, pointers, arrays, structs/unions, enums, function pointers
- Control flow, the operators, initializers, `static`/`extern` linkage
- Diagnostics with file:line that a human can act on

**Acceptance:** compile a **real EmbLinkOS userland program** from
`/data/src` — start with `tally.c` and the `sval` SDK — link it with the
existing toolchain, install it, and run the project's own oracle:
`ls / | tally | get rows` must agree with `ls / | count`. That is the same
cross-check EmbBuild and TCC were each held to.

---

## M3 — Self-hosting

**Goal:** EmbCC compiles EmbCC.

- The **integrated linker** lands (ARCHITECTURE §6), because self-hosting means
  one binary that compiles *and* links
- EmbCC's own source stays inside the subset EmbCC implements (ARCHITECTURE §7)

**Acceptance, in stages — each is a real checkpoint:**
1. `embcc` (built by gcc) compiles all of EmbCC's sources → `embcc-stage1`
2. `embcc-stage1` compiles EmbCC again → `embcc-stage2`
3. **`embcc-stage2` is byte-identical to `embcc-stage1`** (the classic
   fixed-point check — it catches whole classes of codegen bugs nothing else
   will), and `embcc-stage2` still passes M1 and M2's tests

**DONE — 2026-07-24, all three stages, the fixed point closed on the OS:**
1. The gcc-built `embcc` compiles all twelve sources; EmbLD links them into
   a resolved EmbLinkOS `ET_EXEC` (tests/golden/self-host.sh). Codegen is
   deterministic — objects byte-identical across runs.
2 & 3. That `embcc.elf`, staged to EmbLinkOS, ran under the kernel `test
   embcc self` oracle and recompiled all twelve sources ON THE OS: **12/12
   objects byte-identical** to stage1's own reference objects — a compiler
   reproducing itself exactly (stage1 ≡ stage2 at the object level). And
   `test embcc` had already shown stage1 compiles the M1 program on the OS
   to a running, tcc-linked **exit 42**. The basename-only STT_FILE (an
   object depends on content, not build path) is what makes host and OS
   objects match. The on-OS bringup earned D-005 by catching the
   stack-parameter codegen bug the whole host suite missed.

---

## M4 — On the OS, by the OS

**Goal:** EmbLinkOS builds EmbCC, with EmbBuild, from `/data/src`.

- EmbCC ships as `/data/apps/embcc/embcc.elf`
- Its source ships to `/data/src/embcc/` with an `embbuild.manifest`
- EmbBuild compiles and links it **on the OS**

**Acceptance:** `embbuild /data/src/embcc/build.ebm` produces a working
`embcc.elf` on the OS; that on-OS-built EmbCC then compiles the M1 program and
**it exits 42**. This is the total loop: the OS builds the compiler that builds
the OS's programs.

---

## Beyond M4 — only if earned

These are candidates, not commitments, and each needs a stated reason
(DECISIONS D-006):

- **Codegen quality** — *started*: an optimizer (`src/opt/opt.c`) is in the
  pipeline and rides through the self-host fixed point (15/15).
- **`__thread`/TLS** — a real TCC wall; needs `PT_TLS` and the OS's
  `set_fs_base` contract
- **Dynamic linking output** — would let EmbCC build EmUI/GUI apps
  (TARGET_ABI §4b); TCC reached this only with a patch
- **C++** — the largest wall TCC will never clear; a different project in
  size, and now the stated long-term second language (DECISIONS D-008)
- **A language with EmbLink's typed values first-class** — not planned
  (DECISIONS D-008); D-002's ten-lines test remains the only gate back in
- **The native format, not merely an ELF-superset** — *done*: EmbLD emits EMBX
  with a declared capability table (the capability model this was gated on now
  exists, in EmbLinkOS), byte-identical to the reference producer, on host and OS.

---

## The honest expectation

M1 is reachable in a focused stretch. M2 is where most compilers stall, because
real headers are unforgiving. M3 is a genuine achievement. M4 is the one worth
telling people about — and none of it is on a schedule, because EmbLinkOS is the
parent project and wins any contest for attention (VISION §5).
