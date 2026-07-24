# EmbDBG — Debug Information Requirements

*Status: **requirements and a format decision, no byte layout.** Written now, in
the discipline this project insists on: the reasoning survives the gap between
deciding and building, and the on-disk shape is derived **last**, from invariants
that must exist first. This document governs the **producer** side (EmbCC) and
still deliberately does NOT specify the `.embdbg` byte format — see §6.*

**The OS side now exists.** `myos/docs/EMBDBG_Specification.md` (2026-07-24)
specifies the byte-exact `.embdbg` format **and** the kernel debugging contract
(`CAP_DEBUG`, `SPAWN_ACTION_DEBUG`, syscalls 69–75, exception routing) — i.e. it
supplies the consumer and the invariants D-010 §6/§8-Q1 said the format had to be
derived from. Crucially, **that spec does not revise D-010**: it keeps DWARF as
the host bridge and derives the native `.embdbg` from a real consumer, which is
exactly this document's §3. Its own staging even lets its M3 consume "D-010's
DWARF line info converted" through a bridge. So the two documents are the paired
producer/consumer halves the project's docs always come in — this one owns what
EmbCC emits and when; that one owns the bytes and the kernel mechanism.

**Consumes:** the EMBX spec's reservation — `EMBX_COMPAT_DEBUG_SIDECAR` and
`EMBX_SEG_DEBUG` (`myos/docs/EMBX_Specification_v2.md` §3.3/§3.6), the `build_id`
the format binds to (§3.4). **Paired with:** the OS-side format+contract spec
above, and EmbDBG (the debugger it specifies).

## The producer finding: the LINKER owns the absolute-addressed `.embdbg`

The OS spec's `.embdbg` addresses are **absolute vaddrs** (its §2, §5.3–5.5),
which it can assume because an EMBX APP is fully linked with no load slide (EMBX
§4.1). But **EmbCC emits `ET_REL` — relocatable objects** — where those absolute
addresses do not exist yet. This is the EMBX finding again, one channel over:

> Just as "EmbCC emits EMBX" means "EmbLD emits EMBX," **"EmbCC produces
> `.embdbg`" means the linker (or a post-link step) does.** EmbCC's job is to
> emit *relocatable* debug info — DWARF line info in the `.o`, carrying
> relocations against section symbols exactly as DWARF always does in a
> relocatable object (D-010 step 1). Resolving those to the absolute vaddrs the
> `.embdbg` format wants is a **link-time** act, so it lands with or after EmbLD
> (M3, WORKPLAN stream B), by one of two routes the OS spec already allows:
> EmbLD emitting `.embdbg` directly from the linked image, or a
> DWARF→`.embdbg` bridge over the linked binary.

The upshot for EmbCC's own work is clean: **step 1 (emit relocatable DWARF line
info) is unchanged and can proceed whenever**, because relocatable line info
needs no linker. Only the *absolute-addressed `.embdbg` production* waits for
link. This sharpens rather than delays the plan.

---

## 1. What this is for, before what it looks like

Debug information answers three questions a debugger asks, in ascending cost:

1. **Where am I?** — a machine address ↔ a source `file:line`. Backtraces,
   breakpoints by line, "which line faulted."
2. **What can I see?** — the local variables and parameters live at this point,
   each at a stack offset or in a register, with a name.
3. **What is it?** — the type of each variable, so a value is rendered as
   `struct value { VAL_INT, 42 }` and not eight raw bytes.

The whole of debug info is scaffolding around those three answers. They are
listed in order because that is the order they should be **built**: line info is
the cheapest and buys the largest fraction of a debugger's usefulness; types are
the most expensive and the least urgent.

## 2. What already exists, and what does not

The honest inventory, because a requirements doc that overstates the starting
point plans against a fiction.

| Piece | State |
|---|---|
| `file:line` at every AST node | **Exists** — it is what diagnostics already print (`diag_fatal`, the `# line` preprocessor markers the lexer consumes). The producer already *knows* answer (1) everywhere. |
| Line info emitted into the object | **Absent.** EmbCC emits `.text`, `.rodata`, `.data`, `.bss`, `.symtab`, `.rela.text`. No `.debug_*`, no line table. |
| Function/object symbols | **Exists** — `.symtab` carries STT_FUNC / STT_OBJECT with sizes, which a debugger can already use for coarse "which function" attribution. |
| Local-variable locations | **Absent.** sema assigns every local a frame slot (`var_index` → `[rbp+disp]` in codegen), so the data exists at compile time; nothing records it. |
| Type descriptions | **Absent.** The `struct type` graph is complete and precise; it is thrown away after codegen. |
| A debugger to read any of it | **Absent.** EmbDBG does not exist. There is no `ptrace` equivalent, no breakpoint mechanism, no stack-unwind contract named on EmbLinkOS yet. |

The pattern to notice: **every input the first two layers need already exists
inside the compiler.** The work is emission and a format, not analysis. That is
what makes line info a genuinely small increment when its time comes — and what
makes rushing the format the only real risk.

## 3. The format decision — DWARF, or native `.embdbg`?

This is the fork, and it has the same shape as the EMBX-vs-ELF decision
(DECISIONS D-003), for the same reason: EmbLinkOS is **own-the-stack**, but
debugging is one of the few places where **meeting the existing world** has
concrete, immediate value.

**DWARF** is what every existing debugger reads. If EmbCC emitted DWARF line
info into `.debug_line`, `gdb` and `lldb` could debug EmbCC output **today**, on
the host, with no EmbDBG written at all — an enormous force multiplier while the
compiler is young. Against it: DWARF is large, and its expressiveness is
calibrated for optimizing compilers (location lists that vary by PC range, a
Turing-complete expression stack). EmbCC is correct-and-slow with every value in
a stack slot; it would use a rounding error of DWARF's surface.

**A native `.embdbg`** is the ownership play, and the EMBX spec already carved
its slot: a sidecar, not mapped, stripped from the shipped image, its presence a
single compat bit. It would be exactly as large as EmbLinkOS's own debugging
needs and no larger — the EMBKFS/EMBX aesthetic of "byte-exact, one reader, no
decode path that never runs."

**The resolution, and it mirrors the OS's dual-loader stance:**

> **DWARF is the bridge; `.embdbg` is the owned form — and the bridge comes
> first because it needs no consumer we have not built.** EmbCC's first debug
> output should be **minimal DWARF line info** (`.debug_line` + the two
> `.debug_info`/`.debug_abbrev` stubs a line program requires), because it is
> debuggable by tools that already exist, on the host, the day it lands — the
> same "prove it on the host first" discipline that got M1 through M4 (D-005).
> A native `.embdbg` is derived **later**, from what EmbDBG actually turns out to
> need, once EmbDBG exists to have needs. This is not a reversal of the
> own-the-stack thesis; it is the same move EMBX makes — ELF as the porting lane,
> the native format for the owned world — applied to the debug channel.

This keeps the project honest: no format is designed against an absent consumer,
and the first useful debugging arrives against a consumer (gdb) that is real.

## 4. The incremental path

Each step is independently useful and independently testable, in the project's
manner — the acceptance test is "a debugger does the thing," never "the bytes
look plausible."

1. **Line info (DWARF `.debug_line`).** Address → `file:line`. Acceptance:
   `gdb` on the host sets a breakpoint by line in an EmbCC-compiled program,
   stops there, and `bt` names the right lines; and a faulting EmbCC program
   reports the source line. The compiler already has every input.
   **DONE — 2026-07-24.** `embcc -g` emits `.debug_line` + a one-DIE
   `.debug_info` CU + `.debug_abbrev` (DWARF v4, 32-bit, addr_size 8), with
   relocations against `.text`/`.debug_line`/`.debug_abbrev` so the ET_REL
   addresses resolve at link. Proven on the host the honest way: `gdb -batch`
   reads the table from an EmbCC object and resolves `info line dbg.c:3` to
   `0x13 <add+19>` (post-prologue) and line 8 to `<main+…>` — the debugger
   attributes each line to the correct function (`bt` names functions from the
   ELF `.symtab`, no subprogram DIEs needed yet). Gate:
   `tests/golden/debug-line.sh`. The line table is built from a per-function
   `(offset,line)` table codegen collects (irgen stamps every `ir_ins` with
   its statement line via a cursor); the DWARF bytes live in
   `src/debug/dwarf.c`. `-g` is opt-in and deterministic — with it off, output
   is byte-for-byte as before, so the M3 self-host fixed point still closes
   (now 14 sources: `dwarf.c` self-compiles and `stage1≡stage2` holds).
   Not yet: emission at LINK time of the absolute-addressed native form, and
   proving it on a *linked* binary (EmbLD carrying/relocating `.debug_*`) —
   see "the producer finding" above; the object-level host proof stands.
2. **Frame + locals.** `.debug_info` DIEs for each function and its locals with
   `DW_AT_location` = a constant frame-base offset (EmbCC's uniform stack-slot
   model makes every location a single `DW_OP_fbreg`, the trivial case DWARF
   was over-built for). Acceptance: `gdb` prints a local by name at a
   breakpoint with the right value.
3. **Types.** DIEs for the `struct type` graph — base types, pointers, arrays,
   structs/unions/enums with member offsets (which the SysV-classification work
   already computes exactly). Acceptance: `gdb` renders a `struct value` with
   named fields, matching a gcc-built program's rendering.
4. **EmbDBG, and only then `.embdbg`.** A debugger native to EmbLinkOS — needing
   the OS's own breakpoint/step/inspect contract, which is a **kernel** design
   question and out of EmbCC's scope (D-007). When EmbDBG's needs are concrete,
   the native `.embdbg` sidecar is derived from them and the DWARF emitter
   becomes the host-debugging bridge it always was.

## 5. Requirements the format must meet, whichever it is

Stated now so they constrain the byte layout when it is finally derived, rather
than being discovered after it is frozen:

- **Deterministic.** No timestamps, no absolute host paths baked in
  (DECISIONS §2.5, the same rule that lets M3's stage2 be byte-identical). Debug
  output must not break reproducibility.
- **Strippable, and its absence lossless.** A program runs identically with the
  debug channel removed. EMBX already encodes this: the info is a **sidecar**,
  `EMBX_F_STRIPPED` says it is gone, `EMBX_COMPAT_DEBUG_SIDECAR` says it is
  present — a *compat* bit, because a loader that does not understand debug info
  simply ignores it (§3.6: compat bits are safe to ignore).
- **Optimization-honest.** The moment codegen stops putting every value in a
  fixed slot (a post-M4 "codegen quality" reason to exist, per D-006), a
  variable's location varies by PC and "optimized out" becomes a real state. The
  format must be able to *say* "not available here" rather than point at a stale
  slot — THE RULE applied to debug info: a debugger showing a confidently wrong
  value is worse than one that admits it cannot see.
- **One reader.** Whatever the native form becomes, it earns exactly one
  consumer (EmbDBG) and one dumper, the `embread`/`emlibc` discipline — not a
  speculative tool suite (EMBX §9: thirteen tool names before one parser is a
  plan, not a project).

## 6. What this document deliberately does NOT specify, and why

**No byte layout.** Not the DWARF version to target, not a `.embdbg` header, not
section offsets. This is not an omission; it is the decision.

A byte-exact format needs a producer that emits the data (EmbCC emits none) and
a consumer that reads it (EmbDBG does not exist). Designing the container first
is precisely the inversion DECISIONS D-003 was reopened *with eyes open about*,
and the EMBX spec's own §4.2 refuses to repeat it inside one document — "defining
an import/export format before EmbCC can emit a relocation is designing against a
producer that does not exist." The debug channel is the same shape: define the
model (§1–§5), build the producer against a **real** consumer (gdb, via DWARF),
and derive the native on-disk shape **last**, from what EmbDBG turns out to need.

## 7. Ordering

**After M3.** Self-hosting is the milestone that makes EmbCC a real compiler;
debug info is a quality-of-life layer on top of a compiler that works, exactly as
ARCHITECTURE §8 lists DWARF among the deliberate early non-goals ("none belong
before a program runs") and VISION_LONGTERM gates it on "an EmbLinkOS debugger
existing to consume it." The one honest exception is **step 1 (host DWARF line
info)**: it is cheap, its consumer (gdb) already exists, and it would make every
*later* milestone — including debugging the self-hosting compiler through M3 —
materially easier. That single step is the one plausibly worth pulling earlier;
everything past it waits for EmbDBG.

## 8. Open questions (for when this becomes concrete)

1. **The kernel debugging contract.** ~~EmbDBG needs the OS to expose
   breakpoint/step/register-inspect over some interface.~~ **Answered
   (2026-07-24):** `myos/docs/EMBDBG_Specification.md` §6 designs exactly this —
   `CAP_DEBUG` (a new capability class, so a debugger is attenuated not
   omnipotent), `SPAWN_ACTION_DEBUG`, syscalls 69–75, and the `isr_handler`
   exception-routing change. It is a kernel design (D-007 keeps it out of EmbCC),
   reserved not yet built. This unblocks the path past step 3 on the OS side; the
   EmbCC side is still gated on steps 1–3.
2. **Stack unwinding.** Backtraces past the current frame need either frame
   pointers (EmbCC keeps `rbp` as a frame pointer today — a gift for this) or
   CFI. With rbp-based frames, unwinding is a pointer walk and needs no `.eh_frame`
   — another way the correct-and-slow codegen makes debugging *easier*, not harder.
3. **The native `.embdbg` shape.** Derived from §1–§5 and EmbDBG's real needs,
   not before. The EMBX sidecar slot and the compat bit are already reserved for
   it.
