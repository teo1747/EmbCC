# src/link

EmbLD — the integrated linker. ../../docs/ARCHITECTURE.md §6 is the spec, and
TARGET_ABI §4 is the contract it must satisfy.

Library-shaped, never a subprocess (§1: no `fork`/`exec` on the target). The
`embld` tool in `tools/embld/` is a thin `main` over `link_run`.

**What it does.** Reads ELF relocatable objects and `.a` archives; resolves
symbols (archive members pulled to a fixed point, dead members excluded); merges
input sections into output sections by name, the way a linker script's
`*(.init_array)` does, so `__init_array_start`/`__stop_` bracket symbols are the
real group bounds; places COMMON into `.bss`; applies relocations; emits
**ET_EXEC** (never PIE) with a correct `e_entry` and W^X PT_LOAD segments that
the in-kernel loader maps.

**The expensive facts, each learned from a TCC failure** (TARGET_ABI §4):
static links emit **no PLT** (no resolver exists — a PLT slot is a wild jump
with a valid-looking ELF); weak undefined symbols bind to 0 with no relocation;
archive semantics must reach a fixed point.

**Also emits EMBX** — `--embx --cap NAME` writes EmbLinkOS's own
capability-carrying format from the same linked image (D-003 revised), verified
by `tools/embread`.

**Linker-defined symbols** (`-Ttext`, `kernel_end`-style end symbols) and
higher-half LMA (`p_paddr`) are supported — they are what let the kernel link
with no external tools.

It links EmbCC itself, and it links the EmbLinkOS kernel.
