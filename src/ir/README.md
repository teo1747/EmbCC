# src/ir

EmbIR, the intermediate form — ../../docs/ARCHITECTURE.md §3.

State: linear three-address code over int-typed vregs, now with
labels, conditional branches (BRZ), and 0/1 comparisons; && and ||
are lowered to short-circuit branches here. Still no SSA and no
passes — that revision belongs to the optimizer era (VISION_LONGTERM).
