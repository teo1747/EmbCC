# Roadmap

*Each milestone ends in a **concrete acceptance test**, because in this project
a thing is not done when it compiles — it is done when a test exercises the
invariant. Milestones are ordered by what they *prove*, not by how much code
they contain.*

**Current position: M1 COMPLETE — confirmed on the OS 2026-07-20.** The
exit-42 object compiled by `embcc -c`, linked against the real crt0/newlib,
was loaded by the EmbLinkOS kernel and exited 0x2A
(`/data/apps/embcc42/embcc42.elf`, pid 6, `[syscall] exit code=0x2A`). The
whole TARGET_ABI end of the system is validated while the compiler is still
small enough to change cheaply — which was the entire point of doing M1 before
a real frontend. M2 is in progress, growing the subset in test-covered
increments — control flow, the full int operator set, and now prototypes +
external calls: objects carry R_X86_64_PLT32 relocations against UNDEF
symbols and an EmbCC program calls putchar through the real linker (output
diffed against gcc's build). Next: types beyond int (char, pointers,
arrays — unlocking strings and printf), then globals with .data/.bss, and
the preprocessor last, judged against newlib's headers.

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
