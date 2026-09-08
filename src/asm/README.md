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
