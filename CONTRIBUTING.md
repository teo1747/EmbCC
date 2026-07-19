# Contributing to EmbCC

EmbCC inherits its working discipline from EmbLinkOS, because the discipline is
the reason that project's claims hold up. A compiler punishes dishonest testing
harder than almost anything else: it can produce a valid-looking artifact that
is wrong in a way nothing notices until a program dies at a wild address.

## THE RULE

> **A capability may only be claimed if it is genuinely present. A refusal is
> only honest if the thing is truly absent — not merely unimplemented and
> quietly faked.**

For a compiler this means: if a language feature is unsupported, **fail loudly at
compile time**. Never emit code that silently does something else. A missing
feature is fine and expected; a wrong one is a bug that will be found months
later by a program that mysteriously crashes.

## A change is not done because it compiles

It is done when **a test exercises the invariant**. For EmbCC the invariant is
almost always *"the machine runs what we emitted, and the result is correct."*

- A test that only checks the compiler exits 0 proves nothing.
- A test that checks the ELF "looks valid" proves the writer works, not the
  compiler.
- A test that **runs the program and asserts its output/exit code** is the real
  thing. Prefer these; `tests/exec/` is the directory that counts.

## Four ways a green test lies

Each of these has actually happened in EmbLinkOS. They are not hypotheticals.

1. **The binary under test was stale.** Something rebuilt, something didn't, and
   the test measured yesterday's artifact. Put a marker in new tests and check it
   appears; verify what you *think* you ran is what ran.
2. **Your code never ran at all.** A misfired kill, a redirect leaving the
   previous log in place, a test that silently skipped. If a test's marker line
   isn't in the output, doubt the harness before doubting the code.
3. **A green build is not a green test.** After touching a subsystem, re-run
   *that subsystem's* tests — not the ones in muscle memory. A whole class of
   pipeline bugs shipped this way.
4. **The clock you're timing with isn't wall time.** Under emulation the guest
   runs at a fraction of wall speed; "no output for a minute" is routinely *slow*,
   not hung. Take a snapshot (registers, a state dump) before concluding
   anything is hung.

## Prove on the host, confirm on the metal

Host-side iteration is seconds; an OS boot is minutes and runs slowly. So:

- Compare EmbCC's output against gcc/TCC with `readelf`, `objdump`, `nm`.
- Link EmbCC objects with the cross `ld` to check symbol closure independently
  of EmbCC's own linker.
- **Then** boot and run it — the OS is the final judge, not the first.

This is not a shortcut; it is how the TCC header break, the static-GOT NULL
deref, and the shared-library import gap were each found in minutes instead of
boot cycles.

## Record the decision, not just the code

Design decisions go in [docs/DECISIONS.md](docs/DECISIONS.md) with their
**rationale and what would reopen them**. A decision whose reasoning is lost gets
re-litigated every time someone new looks at it — or worse, silently reversed.

When something is learned the hard way — a target fact that cost a debugging
session — write it into [docs/TARGET_ABI.md](docs/TARGET_ABI.md) as an
"expensive fact." That file exists so the next person does not pay twice.

## Commits

- Milestone-sized, with messages that explain the **why**, not just the what.
- Format: `area: summary` (e.g. `codegen: SysV argument classification`).
- **Never add a `Co-Authored-By` line.**
- Name what a change does *not* handle rather than implying completeness.

## Scope discipline

EmbCC is the child project; **EmbLinkOS is the parent and wins any contest for
attention** (VISION §5). If a change to EmbCC would require destabilising the OS
to test, that is a signal the change is wrong or premature — not a reason to
touch the OS.
