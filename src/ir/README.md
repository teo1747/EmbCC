# src/ir

EmbIR, the intermediate form — ../../docs/ARCHITECTURE.md §3.

State: linear three-address code over promoted virtual registers.
Width model: temps hold promoted values (32-bit int class or 64-bit
long/pointer class, the `w` field); variables live in memory at true
width (`size`), with extending loads and truncating stores. Labels,
branches, short-circuit lowering; pointer arithmetic scaled here.
Still no SSA and no passes — that revision belongs to the optimizer
era (VISION_LONGTERM).
