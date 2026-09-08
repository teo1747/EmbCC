# tools

The tools around the compiler — the standalone front-ends over code that lives
in `src/`, plus the generators.

## The toolchain binaries

Each is a thin `main` over a library in `src/`, because nothing on the target
may be a subprocess (ARCHITECTURE §1).

- **embas/** — `embas -f elf64 foo.asm -o foo.o`, the standalone NASM/Intel
  assembler (`src/as`). Also reachable as `embcc -c foo.asm`. Its correctness
  bar is byte-identity with `nasm -f elf64` on the kernel corpus.
- **embld/** — `embld`, the integrated linker (`src/link`). Emits ET_EXEC ELF
  and EMBX.
- **embread/** — dumps and VERIFIES an EMBX image: the spec's §6 load sequence
  and every §8 parse-time guard, run on the host so a producer bug is found here
  rather than as a one-word refusal from the kernel. It is the verifier EmbLD is
  checked against.
- **embdbg/** — EmbDBG, our own debug-info reader. Consumes the DWARF-4 that
  `embcc -g` emits: symbolizes an address to `func:file:line`, inspects frames
  and locals, disassembles, and drives a small TUI — with no gdb in the loop.
  See `docs/EMBDBG_Requirements.md`.

## Generators and harness

- **gen-predef.sh** — regenerates `src/cpp/predef.c` from the reference gcc
  (ARCHITECTURE §5). The only way that table may change.
- **gen-selfhost-ref.sh** — builds the reference objects for the self-hosting
  fixed point (the 16 sources) and relinks stage1. See `docs/SELFHOST_ONOS.md`.
- **gen-embbuild-manifest.sh** — generates `build.ebm`, the EmbBuild manifest
  that builds EmbCC on the OS, with every unit's header closure **derived**
  (`cc -MM`) rather than hand-maintained.
- **gen-kernel-manifest.sh** — the same, for the EmbLinkOS kernel build.
- **embbuild-run.sh** — a host reference EmbBuild: the same typed-manifest walk
  the OS's EmbBuild does, path-mapped onto the host tree. The host half of the
  two-implementation oracle.
- **os-stage.sh**, **os-build-embld.sh** — stage the tree into an OS image and
  cross-build `embld.elf` for the OS.
