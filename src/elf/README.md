# src/elf

Shared ELF structures used by asm + link — ../../docs/ARCHITECTURE.md §2.

M0 state: ELF64 definitions (elf.h, hand-written — no <elf.h>, the source
must stay inside the subset EmbCC will compile) and the relocatable-object
writer skeleton (write.c). No relocations, no executable output yet.
