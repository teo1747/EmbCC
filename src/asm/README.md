# src/asm

Instruction encoding — ../../docs/ARCHITECTURE.md §2 (integrated; there is no
external assembler on the target).

`emit.c` encodes what codegen emits, width-parameterized (REX.W for the 64-bit
forms): mov/movsx/movzx/movsxd at 1/2/4/8 bytes, the alu ops, both the division
and shift families (signed and unsigned), lea, setcc over the signed and
unsigned condition sets, call/jmp/jcc rel32, the SSE scalar float ops and the
int↔float conversions, and the push/pop forms the prologue needs.

`topasm.c` is the two-pass mini-assembler for **file-scope** `__asm__` — crt0's
`_start` stub vocabulary: `.global`/`.globl`, named and numeric-local labels,
`and $imm,%reg`, `call sym` (PLT32), `jmp local-label`, `ret`.

Anything unencodable is a missing function — a build failure, never a wrong
byte.

Two neighbours do related work: `../ir` assembles *extended* inline asm
(operands bound from constraints), and `../as` is EmbAS, the standalone
NASM/Intel-syntax assembler for whole `.asm` files.

## emit_arm64.c

The AArch64 encoder, the counterpart of `emit.c`. Same contract: exactly the
encodings the aarch64 codegen emits, and an instruction it cannot encode is a
missing function that fails at build time rather than a silently wrong word.

**Every encoding is refereed by `aarch64-elf-objdump`.** `tools/a64check`
emits each one and prints what it CLAIMS the instruction is;
`tests/golden/arm64-encoding.sh` disassembles the bytes and diffs the two — 98
instructions including the displacement of every branch. A backend that
assembles its own instructions has no assembler to catch a wrong bit, and a
wrong bit is a silently wrong program rather than a build failure. That test
has already earned its place: it caught a signed 4-byte load being encoded
into an unallocated word (there is no "load signed word into a W register" on
AArch64 — a 32-bit load already delivers every bit), which objdump prints as
`.inst 0x… ; undefined` and the CPU traps on.
