# src/asm

Instruction encoding — ../../docs/ARCHITECTURE.md §2 (integrated; no
external assembler exists on-OS).

State: the encodings codegen emits, width-parameterized (REX.W for the
64-bit forms): mov/movsx/movzx/movsxd at 1/2/4/8 bytes, alu ops, both
division and shift families (signed and unsigned), lea, setcc with the
signed and unsigned condition sets, call/jmp/jcc rel32. Anything
unencodable is a missing function — a build failure, never a wrong
byte.
