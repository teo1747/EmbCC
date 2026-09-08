# src/opt

IR-level optimizer — per-function passes over EmbIR, run at `-O1` and above
(`opt_run`), iterated to a fixpoint. Register allocation and stack-slot
coalescing are **not** here — they need the frame/register model and live in
`../codegen`. This module is purely IR→IR.

Two families. The **local** passes are proven safe by EmbIR's single-assignment
temporaries: a vreg with exactly one definition holds an invariant value, so no
control-flow analysis is needed to know it is the same everywhere.

- **`pass_fold`** — constant folding (width- and sign-correct), algebraic
  identities (`x+0`, `x*1`, `x*0`, `x&0`, …), and strength reduction:
  `x*2^k`→`x<<k`, unsigned `x/2^k`→`x>>k`, `x%2^k`→`x&(2^k-1)`. Signed division
  stays `idiv` — it rounds toward zero.
- **`pass_lvn`** — local value numbering (CSE) within a basic block. Loads and
  variable reads carry a memory version bumped by any store or call, so a load
  after a store is never reused. **`volatile` accesses are never numbered.**
- **`pass_copyprop`** — replaces uses of a single-assignment `MOV`'s destination
  with its source.
- **`pass_dce`** — drops a pure instruction whose result is unused. Loads,
  divides (÷0 traps), stores, calls and branches are never pure. It also remaps
  `var_scope` as it compacts (see below).
- **`pass_immfold`** — folds constant operands into instruction immediates.
- **`pass_storefwd`** — store-to-load forwarding and dead-store elimination.

The **global** passes (`-O2`) build real control-flow machinery — the CFG, the
dominator tree (Cooper-Harvey-Kennedy) and dominance frontiers:

- **`pass_mem`** — SSA-based **mem2reg**. Inserts phi-functions at the iterated
  dominance frontier of each scalar local's definitions, renames, then destructs
  SSA by realising each phi as copies on its incoming edges. This is what
  promotes locals out of memory before the register allocator ever sees them.
- **`pass_gcse`** — dominator-scoped global common-subexpression elimination by
  value numbering, with a cost model so it never lengthens a live range for a
  computation cheaper than the move that would replace it.
- **`pass_loadcse`** — global redundant-load elimination over available
  expressions. Run once per outer round rather than every fixpoint iteration.
- **`pass_sccp`** — sparse conditional constant propagation: constants and
  branch reachability together, so unreachable blocks are dropped rather than
  optimized.

Function **inlining** runs ahead of the per-function pipeline at `-O2`.

## The verifier

`EMBCC_VERIFY=1` (set for the whole test suite) checks after every optimizing
compile that no pass dropped a live value or left `var_scope` indices stale. It
exists because a stale-`var_scope`-after-DCE bug let a loop-local share a stack
slot with a live parameter — it passed the entire suite and was only caught by
booting the kernel. That class of bug is now a hard, loud failure.
