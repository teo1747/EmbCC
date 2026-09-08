# src/ir

EmbIR, the intermediate form — ../../docs/ARCHITECTURE.md §3.

Linear three-address code over virtual registers. Width model: temps hold
promoted values (32-bit int class or 64-bit long/pointer class, the `w` field);
variables live in memory at true width (`size`), with extending loads and
truncating stores. Labels, branches, short-circuit lowering; pointer arithmetic
scaled here. Loads and stores carry a `vol` flag so the optimizer never touches
a `volatile` (MMIO) access.

The temporaries are single-assignment by construction, which is what makes the
local passes in `../opt` sound with no analysis. Full **SSA form** — dominance
frontiers, phi insertion, renaming and out-of-SSA — is built *on demand* inside
`../opt` for mem2reg at `-O2`, not carried in the IR itself.

This module also assembles extended inline asm (`asm_assemble`): the kernel's
full x86-64 vocabulary, every encoding byte-verified against objdump, with
operand registers resolved from constraints (fixed a/b/c/d/S/D, allocatable
`r`/`m`/`i`, `x` for xmm) and clobbered/template-written registers excluded from
the allocator.
