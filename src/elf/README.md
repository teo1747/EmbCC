# src/elf

Shared ELF structures used by asm + link — ../../docs/ARCHITECTURE.md §2.

State: ELF64 definitions (elf.h, hand-written — no <elf.h>, the source
must stay inside the subset EmbCC will compile) and the relocatable-
object writer: sections, symbols (gABI local-then-global order enforced),
and .rela.text with R_X86_64_PLT32 entries. TARGET_ABI §4a is why PLT32:
it is what gcc emits, and every linker here resolves it as PC32 when
static. No executable output yet (the integrated linker is M3).
