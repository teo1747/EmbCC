# src/codegen

IR → x86-64, System V AMD64 — ../../docs/ARCHITECTURE.md §4.

M1 state: deliberately naive — every vreg in a stack slot, every op
through eax, args in the six SysV registers, 16-byte call alignment.
Intra-unit calls patched here; no relocations exist yet.
