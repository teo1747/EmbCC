# src/asm

Instruction encoding — ../../docs/ARCHITECTURE.md §2 (integrated; no
external assembler exists on-OS).

M1 state: exactly the encodings codegen emits (mov/add/sub/imul with
[rbp+disp], call rel32, prologue/epilogue). Anything unencodable is a
missing function — a build failure, never a wrong byte.
