# src/elf

Shared ELF structures used by asm, as, and link — ../../docs/ARCHITECTURE.md §2.

- **elf.h** — ELF64 definitions, hand-written. There is no `<elf.h>` dependency:
  the source must stay inside the subset EmbCC compiles, because EmbCC compiles
  itself.
- **write.c** — the relocatable-object writer: sections, symbols (gABI
  local-then-global ordering enforced), and `.rela.*`. Shared by the compiler
  (`../asm`) and the standalone assembler (`../as`), which is why EmbAS output
  can be byte-identical to nasm's.

Relocations emitted include `R_X86_64_PLT32` (TARGET_ABI §4a: it is what gcc
emits, and every linker here resolves it as PC32 in a static link),
`R_X86_64_PC32`, `R_X86_64_64`, and `R_X86_64_32S` for the kernel's
`&global + addend` initializers.

Executable output lives in `../link` (EmbLD), which emits ET_EXEC ELF and EMBX.
