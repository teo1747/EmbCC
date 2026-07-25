/* The optimizer (VISION_LONGTERM, now begun). A small set of local IR
 * passes run per function between irgen and codegen. Everything is opt-in:
 * at level 0 opt_run does nothing, so -O0 output is byte-for-byte what it
 * was before the optimizer existed — which is what keeps the self-host
 * fixed point (self-host builds at -O0).
 *
 * The IR is a linear three-address form (ir.h) whose expression
 * temporaries are single-assignment: each new_temp result is written by
 * exactly one instruction. That is what makes these passes safe without a
 * full CFG/SSA construction — a value in a single-def vreg is invariant, so
 * constant folding, copy propagation, and dead-code elimination need only
 * per-function def/use counts.
 */
#ifndef EMBCC_OPT_OPT_H
#define EMBCC_OPT_OPT_H

#include "../ir/ir.h"

/* Optimize every function of the unit in place at the given level
 * (0 = none, >=1 = the local passes). */
void opt_run(struct ir_unit *iu, int level);

#endif
