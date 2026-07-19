# Decisions

*ADR-style: each decision states what was chosen, what was rejected, and **why**,
so the reasoning outlives the memory of the conversation. Decisions made before
any code exists are marked as such — they are commitments, not observations, and
may be revisited by evidence (each records what would reopen it).*

---

## D-001 — EmbCC is a separate, parallel project, not OS work

**Decided:** 2026-07-19 (design). **Status:** firm.

EmbCC lives in its own repository, on its own clock. EmbLinkOS depends on none
of it and continues to use TCC.

**Why.** This is exactly how TCC and EmbBuild entered the system: prove the
thing standalone, adopt only when it is genuinely real. Smuggling a half-built
compiler into the OS would put a shippable, honest system at the mercy of an
unfinished one. Keeping them separate costs nothing — adoption, when earned, is
a one-line change in an EmbBuild manifest.

**Rejected:** developing EmbCC inside the `myos` tree "so it can be tested
easily." Testing against the OS does not require living in it.

**Reopens if:** never, really — but the *adoption* decision is separate and
governed by D-006.

---

## D-002 — Bootstrap as a **C compiler**, not a new language

**Decided:** 2026-07-19 (design). **Status:** firm for M1–M3; open beyond.

The first EmbCC compiles a subset of **C** targeting the EmbLink ABI.

**Why.** Three reasons, in order of weight:
1. **Testability from day one.** The OS already runs C. A C subset can be
   validated against real programs — and against TCC's output — the moment it
   emits an object. A new language would have to invent its own notion of
   "correct" simultaneously with its implementation.
2. **The self-hosting loop needs a language that can express a compiler**, and
   C demonstrably can.
3. **It does not foreclose the interesting future.** A language with EmbLink's
   typed values (records, tables, SQL-nulls — the shell's `sval` model) as
   first-class types is the genuinely differentiated idea, and it stays
   available. It is just not the opening move.

**Rejected:** starting with a novel language. The honest gate for that, recorded
now so it is not forgotten: *write ten lines of the program you wish you could
write, in the syntax you wish existed.* If those ten lines do not create real
desire, it is aesthetics, and aesthetics do not survive a multi-year compiler.

**Reopens if:** the ten-lines test passes convincingly, or a concrete program
turns out to be painful in C and obvious in the other language.

---

## D-003 — Emit **ELF**. A native format, if ever, is an ELF *superset*

**Decided:** 2026-07-19 (design). **Status:** firm.

EmbCC emits ELF in the exact shape EmbLinkOS's in-kernel loader accepts
(see TARGET_ABI.md). It does **not** invent a container format.

**Why.** ELF is not the bottleneck: the kernel already parses only the subset it
needs, and every tool that produces binaries here (cross-gcc, newlib, TCC) emits
ELF. A from-scratch format imposes a permanent tax — a two-format world, or a
converter on every artifact, forever — in exchange for nothing the OS currently
lacks.

**The insight that resolves it** (and it is the better design): if a native
format is ever wanted, make it an **ELF superset** — plain ELF the existing
loader already reads, *plus* extra sections or `.note` entries a newer loader
understands. Old tools keep working; new capability is additive; no fork, no
converter, no tax.

**The one thing that could justify extensions:** a **declared capability
manifest**. EmbLinkOS is capability-based, but authority currently comes only
from the parent at spawn (file-actions); a binary that *declares* what it needs
would be a real architectural step, and it pairs with EMBKFS's existing signing.
Note the ordering, which is the whole point: **the format is derived from a
capability-declaration model that does not exist yet.** Design the model first;
prototype it as an ELF `.note` or a sidecar file in `/data/apps/<name>/`; only
then consider format work.

**Rejected:** designing an "EmbLink executable format" up front. Designing the
container before deciding what it declares inverts the discipline the OS's own
docs insist on (derive the on-disk shape *last*, from the invariants).

**Reopens if:** a capability model lands and genuinely cannot be expressed in
ELF notes.

---

## D-004 — Grow **backward from a runnable artifact**, not forward from a lexer

**Decided:** 2026-07-19 (design). **Status:** firm.

M1 is "emit an object that links and *runs on the OS*, exit 42" — with the
frontend as thin as it must be. The C language surface grows afterward.

**Why.** The EmbLinkOS house rule is that a change is not done because it
compiles, but when a test exercises the invariant. For a compiler, the invariant
is *the machine runs what we emitted.* A project that starts with a beautiful
lexer and parser can go months before that is ever true, and every assumption
about the ELF/ABI end stays unvalidated the whole time. Starting at the output
end means the target contract (TARGET_ABI.md) is proven early, when it is cheap
to be wrong.

**Consequence:** early EmbCC will accept an embarrassingly small C subset and
that is correct. Breadth is the easy direction to grow.

---

## D-005 — Prove on the **host** first; the OS is the final judge, not the first

**Decided:** 2026-07-19 (inherited practice). **Status:** firm.

EmbCC development runs host-side (compare against gcc/TCC output, `readelf`,
`objdump`, cross-`ld`); on-OS boots confirm.

**Why.** Inherited from EmbLinkOS and repeatedly vindicated: the TCC header
incompatibility, the static-GOT bug, and the shared-library import gap were each
found on the host in seconds or minutes, where a boot cycle costs minutes and a
QEMU/TCG guest runs at a fraction of wall speed. The rule that emerged:
**reproduce on the host, confirm on the metal.**

---

## D-006 — Adoption is earned by capability, not authorship

**Decided:** 2026-07-19 (design). **Status:** firm.

EmbLinkOS switches from TCC to EmbCC only when EmbCC is *better for a stated
reason* — not because it is ours.

**Why.** THE RULE, applied to ourselves: a claim is only honest if the capability
is genuinely there. "We wrote it" is not a capability. Legitimate reasons would
be: it clears a wall TCC cannot (C++, TLS/`__thread`, codegen quality), it
enables a language that fits the OS's typed model, or the total-loop property
(the system reproducing its own toolchain) becomes a goal in itself.

**Until then:** EmbCC is a study project aimed at production, and the docs say
so plainly.

---

## D-007 — The kernel stays out of scope

**Decided:** 2026-07-19 (design). **Status:** firm.

EmbCC targets **userland**. The EmbLinkOS kernel is built by the cross gcc.

**Why.** "Rebuild-self" in EmbLinkOS has always meant the userland, stated
honestly. The kernel uses freestanding/`-mcmodel=kernel` codegen, custom linker
scripts, and inline asm that a young compiler has no business attempting.
Pretending otherwise would be the exact overclaim the project's docs exist to
prevent.

**Reopens if:** EmbCC ever becomes a serious optimizing compiler with proven
freestanding support — a decision for a much later year.
