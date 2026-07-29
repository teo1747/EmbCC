# src/as

EmbAS — a standalone **NASM/Intel-syntax** assembler (A1). The last external
tool in the EmbLinkOS kernel build was nasm: the kernel's hand-written `.asm`
are NASM syntax, which the compiler's inline-asm encoder (`../asm`, AT&T and
operand-resolved) cannot read. Rather than port nasm, EmbCC grows its own
assembler front-end so the toolchain owns the whole build.

Two entry points, one implementation (`as_assemble`):

- **`embas -f elf64 foo.asm -o foo.o`** — the standalone tool (`tools/embas`).
- **`embcc -c foo.asm -o foo.o`** — the driver dispatches a `.asm` input here,
  as gcc dispatches `.s`. So `embcc` is compiler **and** assembler.

## Pipeline

`preprocess` (strip comments, expand `%macro`, mangle `.local` labels against the
last global) → parse each line into an **item** in its section (fixed bytes, a
relaxable local jump, or a label) → `place` (relax jumps to a rel8/rel32 fixpoint
so short jumps match nasm, then assign label offsets) → `emit_bytes` (patch
in-file references; turn external/absolute ones into relocations) → ELF via the
shared writer (`../elf/write.c`).

- **Directives:** `section`/`segment`, `global`/`extern`, `align`,
  `db`/`dw`/`dd`/`dq`, `resb`/`resw`/`resd`/`resq` (`.bss` reserve), `incbin`
  (embed a raw file — the AP-trampoline blob), `%macro`/`%endmacro` with
  `%1..%N` (the `isr0..255` stub).
- **Relocations:** `R_X86_64_PC32` (call/jump to an extern, addend −4) and
  `R_X86_64_64` (`mov reg, label` → movabs). A reference to a symbol **defined**
  in this object goes against its **section symbol + addend**, as nasm does.
- **Output format:** `-f bin` (flat binary, for the 16/32-bit boot stages and AP
  trampoline) is **not** implemented — it reports an error rather than
  miscompiling. Only `-f elf64` (the 64-bit kernel objects) is supported.

## Correctness bar: byte-identical to nasm

The only reference an assembler can be checked against on the host is nasm.
All 6 kernel ELF `.asm` assemble to objects whose **code, symbol table, and
relocations are byte-identical to `nasm -f elf64`** — matching nasm's
section-appearance ordering, its leading `STT_FILE` symbol, `STT_SECTION`
symbols, and a definition-sequence-ordered symbol table. The residual
differences (the `.strtab` string order and the section-header-table placement)
are linker-invisible and live in the shared ELF writer; a real link of an
`embas` object yields byte-identical relocated `.text` to the nasm object.
See `tests/golden/assembler.sh`.
