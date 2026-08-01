/* Regression for the var_scope-staleness miscompile (the one that made the
 * embcc-built kernel OOM at -O1/-O2 while -O0 and gcc were fine). Shape mirrors
 * embkfs_find_orphan_inode_rec: a function reads a POINTER PARAM (`node`) both
 * before and INSIDE a loop, and the loop body has an ADDRESS-TAKEN local
 * (`child`, memcpy'd into) whose lexical scope is the loop body. Foldable dead
 * arithmetic gives DCE instructions to remove, which renumbers the stream.
 *
 * The bug: DCE renumbered instructions but left var_scope (irgen-stamped indices
 * read by codegen's coalesce_locals) stale, so `child`'s stale scope looked
 * disjoint from `node`'s live range and they were given the SAME stack slot --
 * memcpy(&child, ...) then clobbered `node`. Correct code keeps them apart.
 * Runs at -O0 (run.sh), -O1 (optimizer.sh), and is refereed against gcc. */
// expect-exit: 42

void *memcpy(void *, const void *, unsigned long);

struct blk { long id; long pad[3]; };   /* 32 bytes, like embk_block_ptr */

static long walk(const struct blk *node, const struct blk *table, int n)
{
    long acc = node->id;                  /* read the param up front */
    /* Dead foldable work: DCE drops it, renumbering the instruction stream --
     * the exact trigger that made var_scope go stale. */
    long d0 = 2 + 3, d1 = 7 * 6, d2 = (100 & 0xff) | 1, d3 = (1 << 4) - 5;
    (void)d0; (void)d1; (void)d2; (void)d3;
    for (int i = 0; i < n; i++) {
        struct blk child;                 /* address-taken loop-local */
        memcpy(&child, &table[i], sizeof child);
        acc += node->id + child.id;       /* `node` must survive the memcpy */
    }
    return acc + node->id;                /* ...and here */
}

int main(void)
{
    struct blk table[4] = { {1,{0,0,0}}, {2,{0,0,0}}, {3,{0,0,0}}, {4,{0,0,0}} };
    struct blk root = { 10, {0,0,0} };
    /* acc = 10 + sum_i(10 + table[i].id) + 10
     *     = 10 + (11+12+13+14) + 10 = 70. If `node` gets clobbered by the
     * memcpy into an overlapping `child`, node->id reads table[i].id and the
     * total is wrong. */
    long r = walk(&root, table, 4);
    return r == 70 ? 42 : 1;
}
