# src/ir

EmbIR, the intermediate form — ../../docs/ARCHITECTURE.md §3.

State: linear three-address code over promoted virtual registers.
Width model: temps hold promoted values (32-bit int class or 64-bit
long/pointer class, the `w` field); variables live in memory at true
width (`size`), with extending loads and truncating stores. Labels,
branches, short-circuit lowering; pointer arithmetic scaled here.
Loads/stores carry a `vol` flag so the optimizer never touches a
`volatile` (MMIO) access.

Still **no SSA** — the temporaries are single-assignment already, which
is enough for the local passes in `../opt` (folding, strength reduction,
value numbering/CSE, copy propagation, DCE) to be sound without it. This
module also assembles extended inline asm (`asm_assemble`): the kernel's
full x86-64 vocabulary, every encoding byte-verified against objdump,
with operand registers resolved from constraints (fixed a/b/c/d/S/D,
allocatable `r`/`m`/`i`, `x` for xmm) and clobbered/template-written
registers excluded from the allocator.
