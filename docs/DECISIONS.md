# Decisions

*ADR-style: each decision states what was chosen, what was rejected, and **why**,
so the reasoning outlives the memory of the conversation. Decisions made before
any code exists are marked as such — they are commitments, not observations, and
may be revisited by evidence (each records what would reopen it).*

---

## D-001 — EmbCC is a separate, parallel project, not OS work

**Decided:** 2026-07-19 (design). **Status:** firm — the *separation* is
unchanged, though the "EmbLinkOS depends on none of it" half is now a choice
rather than a necessity: EmbCC can build the kernel (see D-007, revised), and
the OS still ships TCC because D-006 governs adoption.

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

## D-003 — Emit **EMBX** (native, capability-carrying) + keep ELF for porting

**Decided:** 2026-07-19 (design). **REVISED 2026-07-24 — the reopen condition
at the bottom triggered.** The original decision (emit ELF, native format only
as an ELF superset) and its reasoning are kept below because the reasoning is
still correct; what changed is the conclusion, and honestly so — see the
revision block. **Realized 2026-07-26:** EmbLD emits EMBX natively (`embld
--embx --cap NAME`) — byte-identical to the reference producer, on the host and
on the OS — so the revised conclusion is not just decided but produced by the
toolchain (ELF stays the porting substrate; the dual loader is unchanged).

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

*(That ordering is exactly what happened, and it is why this decision was
revised rather than overturned: the capability model landed in EmbLinkOS first,
and only then did the format follow — as **EMBX**, which EmbLD now emits with a
declared capability table. The one piece still outstanding is the program
declaring its own authority **in source**, so EmbCC records it and EmbLD
collects it; today it comes from `--cap` on the link line.)*

**Rejected:** designing an "EmbLink executable format" up front. Designing the
container before deciding what it declares inverts the discipline the OS's own
docs insist on (derive the on-disk shape *last*, from the invariants).

### Revision — 2026-07-24

This decision **predicted its own supersession** and got the ordering right: it
said a declared capability manifest was the one thing that could justify a
native format, and to **build the model first**. That is exactly what happened.

1. **The capability model landed** in EmbLinkOS: a per-process capability set,
   seeded at init and attenuated at spawn, with a spawn-syscall path and a
   first handle-install gate (kernel `capabilities.h`, `sys_getcaps`,
   `SPAWN_ACTION_SET_CAPS`). The model came first, from invariants — the
   discipline this decision insisted on.
2. **Then the format**, EMBX (`myos/docs/EMBX_Specification_v2.md`), byte-exact,
   with a **working in-kernel loader** that enforces the capability check at
   load (§6 step 9). The declaration flows: binary table → step-9 check → the
   process's cap set → a gated handle.

**Honest note on the reopen basis.** This decision's stated reopen was "if the
model *cannot be expressed in ELF notes*." Strictly, it could have been — a
`.note.embx.caps` section would have worked. The reopen is therefore on a
*different* basis than anticipated: a deliberate **ownership** choice, made by
the OS's author, consistent with how EmbLinkOS already runs a dual-format world
for filesystems (EMBKFS native for its own data, FAT32 to read foreign disks).
Executables now mirror that: **EMBX for programs built for EmbLink, ELF kept as
the porting substrate** (foreign source recompiled — the git/CPython/C++ path).
It is a dual *loader*, not a converter. So EmbCC's eventual output is EMBX; ELF
stays for as long as porting does.

**Reopens if:** never for "ELF-only" again. The open question is now the
inverse — when EmbCC gains a relocation model, whether EMBX's `.embdll` linkage
contract (spec §4.2, still deferred) is the right shape.

**Original reopen (kept for the record):** a capability model lands and
genuinely cannot be expressed in ELF notes.

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

**Where that stands (2026-09-08).** Two of the three legitimate reasons are now
met by evidence rather than intent: EmbCC clears a wall TCC cannot — it builds
and boots the **kernel**, with freestanding codegen, a full inline-asm
assembler, and an optimizer (D-007, revised) — and the total-loop property is
real, with the self-hosting fixed point holding on the OS over 16 sources.

**This still is not adoption.** Being able to build the kernel is a capability;
choosing EmbCC over TCC for the OS's official build is a separate decision, and
this one governs it. The OS ships TCC until someone has a concrete reason to
switch. What has changed is that the case would now be argued from what the
compiler demonstrably does.

---

## D-007 — The kernel stays out of scope

**Decided:** 2026-07-19 (design). **REVISED 2026-09-08 — the reopen condition
was met.**

**The original decision.** EmbCC targets **userland**. The EmbLinkOS kernel is
built by the cross gcc.

**Why (as written in 2026-07).** "Rebuild-self" in EmbLinkOS has always meant
the userland, stated honestly. The kernel uses freestanding/`-mcmodel=kernel`
codegen, custom linker scripts, and inline asm that a young compiler has no
business attempting. Pretending otherwise would be the exact overclaim the
project's docs exist to prevent.

**Reopens if:** EmbCC ever becomes a serious optimizing compiler with proven
freestanding support — a decision for a much later year.

### The revision

That reopen condition is satisfied, and it was satisfied by building each of the
three named obstacles rather than by lowering the bar:

- **Freestanding codegen** — `-mno-sse`, `-mno-red-zone`, `-mcmodel=kernel` and
  the rest, with a float op in SSE-off mode refused loudly rather than emitted.
- **Inline asm** — a real extended-asm assembler covering the kernel's full
  hardware vocabulary, every encoding byte-verified against objdump.
- **Linker scripts** — not needed: EmbLD auto-provides the end-of-image and
  bracket symbols a script would define, plus higher-half LMA (`p_paddr`).
- **The assembler** — EmbAS assembles the kernel's hand-written `.asm`
  byte-identically to nasm.
- **An optimizer worth the name** — SSA mem2reg, inlining, SCCP, global CSE and
  Chaitin-Briggs register allocation, with `-O0`/`-O1`/`-O2` kernels all booting.

**The result:** all 89 kernel C translation units compile under `embcc`, the 6
`.asm` assemble under `embas`, `embld` links the image, and it boots to the home
desktop behaviourally identical to the gcc build — 193 lines of boot output, 0
faults. **No kernel C was changed to achieve this.**

**What has NOT changed.** The kernel is still *EmbLinkOS's* code, and kernel
design questions (the debugging contract, `CAP_DEBUG`, syscall numbering) remain
out of EmbCC's scope — that half of D-007 stands. The OS's official kernel build
also still uses the cross gcc; being *able* to build the kernel is not adoption,
which D-006 governs. What died is the claim that EmbCC *cannot* and should not
try.

---

## D-008 — Target languages: **C, then C++.** No language of our own is planned

**Decided:** 2026-07-20. **Status:** current intent; C++ is unscheduled.

EmbCC's languages are **C** (the M1–M4 path) and, in the long term, **C++**.
The novel-language direction that VISION.md §4.2 called "the most interesting
long-term direction" — EmbLink's typed values (records, tables, SQL-nulls) as
first-class types — is **not planned**. It is demoted from "interesting future"
to "possible if it ever earns itself"; D-002's ten-lines gate remains the only
door back in, and nobody is expected to walk through it.

**Why.**
1. What the OS actually needs is the ability to build the software that exists,
   and that software is C and C++. The ports story already proved the demand:
   C++/libstdc++ was ported *before* any native compiler work began, and C++
   is the wall TCC will never clear — making it the clearest D-006-legitimate
   capability EmbCC could ever deliver.
2. A novel language multiplies every cost in this repo — testability against
   existing compilers disappears, self-hosting gains a second bootstrap
   problem, and adoption requires rewriting working programs. The payoff was
   always speculative; stating "not planned" is more honest than leaving it
   glowing in the vision docs as an implied someday.

**Order still holds:** C++ comes after the C compiler closes the M4 loop, not
alongside it. It is a frontend-and-sema project of a different size (name
mangling, overloading, templates, EH/unwinding, a C++ runtime against newlib's
C-only world) and it gets its own decision record when it becomes concrete.

**Reopens if:** the ten-lines test (D-002) passes convincingly for the typed
values language — the gate is unchanged, only the expectation is.


---

## D-009 — Own libc: **emlibc**, non-POSIX, EmbLink-shaped

**Decided:** 2026-07-23. **Status:** REALIZED (2026-07-26). emlibc is implemented
(`myos/user/emlibc/`), EmbCC compiles it on the OS **floating point included**
(real `fdlibm` math), and it self-hosts and ships as EMBX — the closed loop the
last paragraph below names, reached: `test emlibc math selfhost` has the OS
build the whole libc with EmbCC + EmbLD and run it. Requirements:
`myos/docs/EMLIBC_Requirements.md`.

EmbCC's link target is **emlibc**, EmbLinkOS's own C library, not
newlib (nor musl/glibc). The requirements doc is the OS-side artifact; EmbCC
consumes its contract, the same relationship it has with the EMBX format spec.

**Why.** The ownership loop D-003 and D-008 point at closes only if the *library*
is owned too: language + compiler + libc + format + loader + OS, one system.
musl/glibc were weighed and declined — both are written against the Linux
syscall ABI and would drag the system toward host-the-world, the pole
EmbLinkOS deliberately sits opposite. The OS's author is "only half okay" with
depending on a ported POSIX libc forever.

**What keeps it tractable** (and why it is not "rewrite a libc"): most of a libc
is OS-agnostic (string/math/malloc/printf number-formatting) and may be **lifted**
from a permissive source — D-006 applied to ourselves, a from-scratch `cosf` is
authorship without capability. POSIX lives only in the thin OS-facing rim
(I/O, process, time, entropy), and EmbLinkOS **already owns that rim**
(`crt0.c`, `syscalls.c`, the errno map). So emlibc is incremental: own the rim,
grow the agnostic bulk header by header, expose the capability-aware surface
newlib cannot (`getcaps`, spawn+file-actions instead of `fork`), and eventually
be **compiled by EmbCC itself** (the closed loop) and shipped as **EMBX**.

**Rejected:** a POSIX-compatible libc (musl/glibc port), because POSIX
compatibility is exactly the thing the OS's non-POSIX model refuses. picolibc
remains a legitimate *interim* upgrade over newlib if raw completeness is wanted
before emlibc exists — but as a stopgap, never the destination.

**Order:** after the C compiler is real (M1–M3) and alongside/after C++ (D-008);
emlibc is not the opening move. It gets its own milestones when it becomes
concrete.

**Reopens if:** the interim (picolibc) proves good enough that owning the libc
never earns itself under D-006 — the same earn-by-capability gate everything
here answers to.

---

## D-010 — Debug info: **DWARF as the bridge, native `.embdbg` derived last**

**Decided:** 2026-07-24. **Status:** landing as predicted. Requirements written
(`docs/EMBDBG_Requirements.md`); the OS side carries the byte-exact format AND
the kernel debugging contract (`myos/docs/EMBDBG_Specification.md`) — the
consumer and invariants this decision said `.embdbg` must be derived from.
**Realized 2026-07-26:** EmbCC emits **DWARF line info** (`-g`, the host bridge),
and **EmbLD emits the native `.embdbg`** at link time (`emit_embdbg`) — exactly
the split this decision predicted: the LINKER, not the compiler, produces the
absolute-addressed sidecar, because EmbCC's ET_REL objects carry only
*relocatable* line info (the EMBX finding, one channel over). DWARF stays the
bridge; the `.embdbg` byte layout was still derived from what EmbDBG actually
needs, not ahead of it.

EmbCC's first debug output was **minimal DWARF line info**, because it is
debuggable by tools that already exist (gdb/lldb) on the host the day it lands —
no EmbDBG required. *(2026-09-08: EmbDBG now exists too — `tools/embdbg/` reads
the DWARF back with no gdb in the loop, and the native `.embdbg` was derived
from its real needs, exactly in the order this decision set.)* A **native `.embdbg`** sidecar is the eventual owned form,
but its byte layout is derived **later**, from what EmbDBG (which did not exist
when this was decided) actually needs.

**Why.** This is the DECISIONS D-003 fork again — own-the-stack vs
meet-the-world — and it resolves the same way, for the same reason. A byte-exact `.embdbg` *at the time of this decision* would have been designed
against a producer that emitted nothing (EmbCC put no line/local/type info in its
objects) and a consumer that did not exist.
That is the exact inversion D-003 was reopened *with eyes open about*: derive the
on-disk shape last, from invariants. DWARF line info sidesteps it entirely — its
consumer (gdb) is real, so "prove it on the host first" (D-005) applies to
debugging exactly as it did to codegen. And EmbCC is unusually well-placed to
emit it: it already knows `file:line` at every node (diagnostics use it), every
local lives in a fixed stack slot (one `DW_OP_fbreg`, DWARF's trivial case), and
`rbp` is kept as a frame pointer, so unwinding is a pointer walk with no CFI.

**The dual-form stance, stated so it is not re-litigated:** DWARF is the bridge
for host debugging and stays for as long as that is useful; `.embdbg` is the
native form EmbDBG consumes, mirroring EMBX's ELF-for-porting /
native-for-the-owned-world split (D-003) and EMBKFS's FAT32 / native split. The
EMBX spec already reserved the slot — `EMBX_COMPAT_DEBUG_SIDECAR`, a *compat* bit
(a loader that does not understand debug info ignores it), and `EMBX_F_STRIPPED`
for its absence.

**Rejected:** a byte-exact `.embdbg` format now. Designing the container before
there is a producer or a consumer is D-003's mistake at a smaller scale, and
`EMBDBG_Requirements.md` §6 refuses it explicitly.

**Order:** after M3 — *and that is how it went* (ARCHITECTURE §8 lists DWARF among the deliberate early
non-goals; VISION_LONGTERM gates debug info on a debugger existing to consume
it). The one honest exception is DWARF line info, cheap enough and useful enough
— it would help debug the self-hosting compiler *through* M3 — that it is the
one step plausibly worth pulling earlier.

**Reopens if:** EmbDBG's real needs turn out not to fit a DWARF-derived model,
or the kernel debugging contract (the open question that gates a native
debugger, D-007) lands and dictates a shape.
