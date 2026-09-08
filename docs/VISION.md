# Vision — why EmbCC exists

*Status: the founding design record, written before any code, so the reasoning
survives the gap between deciding and building. The argument below still stands
as written; §4–§6 carry dated notes where the project has since answered them.
What was a thesis is now a working toolchain — see the README for state.*

## 1. The question that started it

> "What is even yours if you just use all existing tools?"

It deserves a straight answer, because the honest one is what makes EmbCC a
*choice* rather than an anxiety.

**Everything that matters is already yours.** An OS is the code that owns the
machine and defines the contracts everything else obeys. In EmbLinkOS that is:
the kernel (scheduler, VMM, SMP, interrupts), EMBKFS (a from-scratch filesystem
with compression, encryption, snapshots, verified boot), the **non-POSIX syscall
ABI**, the capability/handle model, the no-`fork`/`exec` spawn-with-file-actions
design, the structured shell, EmUI and the compositor, the boot chain. None of
that was handed to you.

gcc, ELF, and newlib are not the OS — they are *the scaffolding the world agreed
on so you didn't have to write a C compiler before you could write a scheduler.*
The kernel is written in C, a language you didn't invent, and it is still
unmistakably yours. Every OS does this: Linux is built with gcc, emits ELF, uses
make; Windows uses MSVC and PE. **Tools are leverage, not authorship.**

So EmbCC is not a fix for a deficiency. Nothing is missing.

## 2. What EmbCC actually is

It is a reach for a **rarer kind of authorship**: the closed, self-consistent
loop where the language, the compiler, the executable format, the loader, and
the OS are one system, beholden to nothing outside it.

That lineage is real and small — Oberon (Wirth's language, compiler, and OS as
one design), TempleOS (HolyC), the Smalltalk machines. It is not "more of an
OS." It is *more total authorship*, and wanting it is a legitimate intellectual
and aesthetic goal, not vanity.

## 3. The tension, stated honestly

EmbLinkOS has a strong existing soul: **host the world.** The ports story — git,
CPython 3.14, C++/libstdc++, TCC — is built on meeting real software on its own
terms and refusing to fake anything (`docs/PORTS.md`'s THE RULE: a refusal is
only honest if the capability is genuinely absent). Every port made the OS more
*compatible*.

EmbCC pulls the other way: **own the stack.** A native compiler is a step away
from the shared ecosystem and toward a self-contained world.

Both are legitimate. They are not, however, the same project, and drifting
between them one `.c` file at a time is how a system ends up half-committed to
each. The resolution EmbLinkOS has used at every previous fork applies here:

> **Go native when it fits the model *and* a near-term need makes the standard
> option concretely fail. Otherwise keep the standard option and defer the
> native one until something demands it.**

That rule has resolved four forks already (the shell, the TTY, the userspace
layout, the build tool). Applied to a compiler when this was written, it said:
**TCC works, so this is not urgent.** Which is exactly why EmbCC is a separate
repository with a long clock, and why the OS depends on none of it.

*(2026-09-08.) The "concretely fail" half of the rule has since been met on one
axis: TCC cannot build the EmbLinkOS kernel, and EmbCC can — it compiles all 89
kernel units, EmbAS assembles the 6 `.asm`, EmbLD links, and the result boots.
That does not auto-adopt anything; D-006 still governs, and the OS still ships
TCC. It does mean the native-option case is now made of evidence rather than
intent.)*

## 4. What would make EmbCC genuinely worth adopting

Not "we wrote it ourselves." Adoption should be earned by at least one of:

1. **Capability TCC cannot reach.** TCC's known walls in EmbLinkOS are C++, TLS
   (`__thread`), and codegen quality. A compiler that clears one of those buys
   something concrete.
2. **A language that fits the OS's model.** EmbLinkOS's shell speaks typed
   values — records, tables, SQL-style nulls, a serializer. A language with
   those as *first-class types* rather than a library would fit the OS the way
   EmbBuild fit where make did not. This is the most interesting long-term
   direction, and the one that must be justified by a program you actually want
   to write (see DECISIONS D-002).
3. **Total-loop integrity.** The self-hosting milestone — EmbCC compiling
   EmbCC, built by EmbBuild, on the OS — is a real property, not just a stunt:
   it means the system can reproduce its own toolchain without an outside host.

*(2026-09-08.) Two of the three have landed.* **(1)** is met on the kernel:
codegen quality and freestanding support TCC does not reach, and EmbCC now
builds the kernel end to end. **(3)** is met: the self-hosting fixed point holds
over all 16 sources, on the host and on the OS. **(2)** remains open by choice —
D-008 demoted a language of our own, and C++ is the intended second language.

That leaves **(4)**, someone choosing it over TCC for a real reason, as the one
measure still outstanding. It is the only one that was ever going to be decided
by other people rather than by us.

## 5. What EmbCC is *not*

- **Not a TCC replacement on any schedule.** TCC is the OS's compiler until
  something better is proven better.
- **Not a new executable format.** ELF stays. See DECISIONS D-003.
- ~~**Not the kernel's compiler.**~~ **Revised 2026-09-08.** This was written
  when "rebuild-self" in EmbLinkOS honestly meant the *userland* only. It no
  longer does: EmbCC compiles the whole kernel, EmbAS assembles its hand-written
  `.asm` byte-identically to nasm, EmbLD links the image, and it boots to the
  desktop behaviourally identical to the gcc build. The cross gcc is still what
  the OS's official build uses; it is no longer the only thing that *can* build
  the kernel.
- **Not a reason to slow the OS down.** If EmbCC ever competes with OS work for
  attention, the OS wins. It is the parent project.

## 6. The measure of success, in order

1. ~~It compiles a C program that **runs on EmbLinkOS** (exit 42).~~ **Done**
   — M1, confirmed on the OS 2026-07-20.
2. ~~It compiles **itself**.~~ **Done** — M3, the fixed point closed on the OS
   2026-07-24 and still holds over 16 sources.
3. **EmbBuild builds it, on the OS, from `/data/src`.** *In progress* — the
   manifest exists and builds EmbCC on the host; running it on the metal is the
   open step (M4).
4. Someone chooses it over TCC for a real reason. **Open.**

Steps 1–3 are engineering. Step 4 is the only one that makes it a compiler
rather than an exercise — and it is allowed to take years.
