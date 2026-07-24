# Roadmap

*Each milestone ends in a **concrete acceptance test**, because in this project
a thing is not done when it compiles — it is done when a test exercises the
invariant. Milestones are ordered by what they *prove*, not by how much code
they contain.*

**Current position: M2 COMPLETE; M3 underway — EmbLD B1 done on the OS
2026-07-24.** The integrated linker links: EmbLD linked the M1 program
(crt0 + syscalls + an EmbCC object + newlib's libc.a) into an ET_EXEC and
the kernel ran it to exit 42, no cross-ld involved — the compiler and the
linker are now both ours end to end for a real program. EmbLD B2 followed
the same day: output-section grouping gives correct __init_array/ctors
bracket symbols (a constructor program runs its ctor) and COMMON
placement, and an EmbCC-compiled + EmbLD-linked printf program prints
correctly on the OS. The GOT was verified unneeded for this newlib (zero
GOTPCREL). What remains for M3's self-hosting acceptance: compile EmbCC's
own sources with EmbCC, link with EmbLD, and close the stage2-byte-
identical fixed point (WORKPLAN stream B).

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
(see WORKPLAN "The EMBX finding"). Landed already: `embread`, the EMBX
dumper/verifier, checked against the OS's own images.

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

- **Codegen quality** — the first honest reason to prefer EmbCC over TCC
- **`__thread`/TLS** — a real TCC wall; needs `PT_TLS` and the OS's
  `set_fs_base` contract
- **Dynamic linking output** — would let EmbCC build EmUI/GUI apps
  (TARGET_ABI §4b); TCC reached this only with a patch
- **C++** — the largest wall TCC will never clear; a different project in
  size, and now the stated long-term second language (DECISIONS D-008)
- **A language with EmbLink's typed values first-class** — not planned
  (DECISIONS D-008); D-002's ten-lines test remains the only gate back in
- **ELF-superset extensions** — gated on a capability model existing first
  (DECISIONS D-003)

---

## The honest expectation

M1 is reachable in a focused stretch. M2 is where most compilers stall, because
real headers are unforgiving. M3 is a genuine achievement. M4 is the one worth
telling people about — and none of it is on a schedule, because EmbLinkOS is the
parent project and wins any contest for attention (VISION §5).
