# src/driver

argv, flags, orchestration — ../../docs/ARCHITECTURE.md §2.

Runs the whole pipeline in one process (§1 — the OS has no `fork`/`exec`, so
there is no `cc1`/`as` to spawn):

    lex → cpp → parse → sema → IR → opt → codegen → ELF

**Inputs.** `-c FILE.c` compiles; `-c FILE.asm` dispatches to `../as` (EmbAS),
the way gcc dispatches `.s`. `-E` stops after preprocessing.

**Flags.** `-o`, `-I`, `-isystem`, `-O` (`-O0`/`-O1`/`-O2`, ../opt + codegen),
`-g` (DWARF-4 line/frame/locals, ../debug), and the freestanding-mode switches
the kernel needs — `-mno-sse`, `-mno-sse2`, `-mno-80387`, `-mno-mmx`,
`-mgeneral-regs-only`, `-mno-red-zone`, `-mcmodel=kernel`. Also `--version`,
`--dump-predef`, `--emit-empty-object`.

Flags stay a **deliberate subset**; this is not a gcc-surface clone.

Linking is a separate tool, `embld` (../link, `tools/embld/`), for the reason in
ARCHITECTURE §6 — but it is the same code, callable in-process, never a
subprocess.

`util.c` holds the diagnostics chokepoint (`diag_fatal` and friends): the
caret/colour/suggestion machinery all funnels through here.
