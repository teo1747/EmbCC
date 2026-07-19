# Vision — why EmbCC exists

*Status: design record. Written before any code, so the reasoning survives the
gap between deciding and building.*

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
layout, the build tool). Applied to a compiler *today*, it says: **TCC works, so
this is not urgent.** Which is exactly why EmbCC is a separate repository with a
long clock, and why the OS depends on none of it.

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

Until one of those lands, EmbCC is a study project that happens to be aimed at
production. That is a fine thing to be, as long as the docs say so — and they do.

## 5. What EmbCC is *not*

- **Not a TCC replacement on any schedule.** TCC is the OS's compiler until
  something better is proven better.
- **Not a new executable format.** ELF stays. See DECISIONS D-003.
- **Not the kernel's compiler.** The EmbLinkOS kernel is built with the cross
  gcc and will be for the foreseeable future; "rebuild-self" in EmbLinkOS has
  always meant the *userland*, honestly scoped.
- **Not a reason to slow the OS down.** If EmbCC ever competes with OS work for
  attention, the OS wins. It is the parent project.

## 6. The measure of success, in order

1. It compiles a C program that **runs on EmbLinkOS** (exit 42).
2. It compiles **itself**.
3. **EmbBuild builds it, on the OS, from `/data/src`.**
4. Someone chooses it over TCC for a real reason.

Steps 1–3 are engineering. Step 4 is the only one that makes it a compiler
rather than an exercise — and it is allowed to take years.
