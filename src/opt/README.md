# src/opt

IR-level optimizer — local, per-function passes over EmbIR, run at `-O1` and
above (`opt_run`). Each pass is proven safe by EmbIR's single-assignment
temporaries: a vreg with exactly one definition holds an invariant value, so no
control-flow analysis is needed to know it is the same everywhere. The driver
iterates the passes to a fixpoint.

- **`pass_fold` — constant folding, algebraic identities, strength reduction.**
  Folds constant operations (width- and sign-correct); simplifies `x+0`, `x*1`,
  `x*0`, `x&0`, … ; and reduces `x*2^k`→`x<<k`, unsigned `x/2^k`→`x>>k`,
  `x%2^k`→`x&(2^k-1)` by retargeting a single-use literal in place (signed
  division stays `idiv` — it rounds toward zero).
- **`pass_lvn` — local value numbering (CSE).** Within a basic block, a pure
  computation, address arithmetic, constant, or memory read whose inputs match
  an earlier one becomes a copy of that earlier result. Loads and variable reads
  carry a memory version bumped by any store/call, so a load after a store is
  never reused; **`volatile` accesses are never numbered**, preserving every MMIO
  access (volatility is tracked from the type through to the IR — see `../sema`).
- **`pass_copyprop` — copy propagation.** Replaces uses of a single-assignment
  `MOV`'s destination with its source.
- **`pass_dce` — dead-code elimination.** Drops a pure instruction whose result
  is unused. Loads, divides (÷0 traps), stores, calls, and branches are never
  pure.

Register allocation and stack-slot coalescing are **not** here — those live in
`../codegen` (they need the frame/register model). This module is purely IR→IR.
