/* IR → x86-64, System V AMD64 (ARCHITECTURE §4). Deliberately naive:
 * every vreg lives in a stack slot, every operation goes through eax.
 * Correct-and-slow first — register allocation is a post-M4 reason to
 * exist, not an M2 one (ARCHITECTURE §3).
 *
 * Slot discipline: temporaries are stored as full 8 bytes (32-bit
 * results arrive zero-extended, so the slot is always well-defined);
 * variables occupy their real size (arrays their full extent) so their
 * address points at exactly sizeof(type) meaningful bytes.
 */
#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>

#include "../driver/util.h"

/* K13 — temporary stack-slot coalescing. At -O0 every temp (vreg >= nvars)
 * otherwise gets its own 8-byte slot, never reused, so a deep call chain's
 * frames overflow the kernel stack. This assigns each temp a 0-based index
 * into a SHARED pool, letting temps whose live ranges don't overlap reuse one
 * slot. *npool_out receives the pool size (distinct slots). Returns a malloc'd
 * per-temp index array (length nvregs-nvars), or NULL if there are no temps.
 *
 * Soundness. A temp is "coalescable" only when its whole live range lies within
 * ONE basic block (block ids below). For such temps a value defined at d and
 * last used at u is dead everywhere outside [d,u] within a straight-line run,
 * so two coalescable temps with disjoint [first,last] index ranges are never
 * simultaneously live — even across loop back-edges (each is reborn inside its
 * block every iteration). Temps that cross a block boundary (a `?:`/`&&`/`||`
 * result, say) are NOT coalesced: they keep a unique slot. The interval is the
 * span of EVERY appearance of the temp in ANY operand field — over-counting a
 * range only shrinks reuse, never makes it unsound, so a blind field scan (no
 * per-op operand table to get wrong) is deliberately used. Deterministic, which
 * the self-host fixed point requires. */
static int *coalesce_temps(struct ir_func *fn, int nvars, int *npool_out)
{
    int nins = fn->nins, nvr = fn->nvregs;
    int ntemp = nvr - nvars;
    *npool_out = 0;
    if (ntemp <= 0)
        return NULL;

    /* basic-block id per instruction: a new block begins at a label and after
     * any branch/jump/return/ud2. */
    int *blk = xmalloc((size_t)(nins ? nins : 1) * sizeof *blk);
    int b = 0;
    for (int i = 0; i < nins; i++) {
        enum ir_op op = fn->ins[i].op;
        if (op == IR_LABEL) b++;
        blk[i] = b;
        if (op == IR_JMP || op == IR_BRZ || op == IR_BRNZ ||
            op == IR_RET || op == IR_UD2)
            b++;
    }

    /* [first,last] instruction index over every appearance of each temp. */
    int *first = xmalloc((size_t)ntemp * sizeof *first);
    int *last  = xmalloc((size_t)ntemp * sizeof *last);
    for (int k = 0; k < ntemp; k++) { first[k] = -1; last[k] = -1; }
    for (int i = 0; i < nins; i++) {
        struct ir_ins *in = &fn->ins[i];
        int vs[4]; int nv = 0;
        vs[nv++] = in->dst; vs[nv++] = in->a; vs[nv++] = in->b; vs[nv++] = in->c;
        for (int j = 0; j < nv; j++) {
            int v = vs[j];
            if (v >= nvars && v < nvr) {
                int k = v - nvars;
                if (first[k] < 0) first[k] = i;
                last[k] = i;
            }
        }
        if (in->op == IR_CALL)
            for (int a = 0; a < in->nargs; a++) {
                int v = in->argv[a].vreg;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
        if (in->op == IR_ASM && in->asm_ir) {
            struct ir_asm *ia = in->asm_ir;
            for (int a = 0; a < ia->nin; a++) {
                int v = ia->in[a].temp;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
            for (int a = 0; a < ia->nout; a++) {
                int v = ia->out[a].temp;
                if (v >= nvars && v < nvr) {
                    int k = v - nvars;
                    if (first[k] < 0) first[k] = i;
                    last[k] = i;
                }
            }
        }
    }

    /* order temps by first-appearance (ties by temp index), via buckets keyed
     * on the first index — O(nins+ntemp) and deterministic. Never-appearing
     * temps bucket at `nins`. */
    int *head = xmalloc((size_t)(nins + 1) * sizeof *head);
    for (int i = 0; i <= nins; i++) head[i] = -1;
    int *nxt = xmalloc((size_t)ntemp * sizeof *nxt);
    for (int k = ntemp - 1; k >= 0; k--) {
        int fi = first[k] < 0 ? nins : first[k];
        nxt[k] = head[fi]; head[fi] = k;
    }

    /* linear scan: reuse a freed pool index for a coalescable temp once its
     * previous occupant is dead; a non-coalescable temp takes a fresh index it
     * never gives back. */
    int *slot = xmalloc((size_t)ntemp * sizeof *slot);
    int *freelist = xmalloc((size_t)ntemp * sizeof *freelist);
    int *act_last = xmalloc((size_t)ntemp * sizeof *act_last);
    int *act_idx  = xmalloc((size_t)ntemp * sizeof *act_idx);
    int nfree = 0, nact = 0, pool = 0;
    for (int i = 0; i <= nins; i++) {
        for (int k = head[i]; k >= 0; k = nxt[k]) {
            if (first[k] < 0) {          /* never referenced: throwaway slot */
                slot[k] = pool++;
                continue;
            }
            int coalescable = (blk[first[k]] == blk[last[k]]);
            /* expire actives dead before this temp is defined */
            for (int a = 0; a < nact; ) {
                if (act_last[a] < first[k]) {
                    freelist[nfree++] = act_idx[a];
                    act_last[a] = act_last[nact - 1];
                    act_idx[a]  = act_idx[nact - 1];
                    nact--;
                } else {
                    a++;
                }
            }
            int idx = (coalescable && nfree > 0) ? freelist[--nfree] : pool++;
            slot[k] = idx;
            if (coalescable) {
                act_last[nact] = last[k];
                act_idx[nact]  = idx;
                nact++;
            }
        }
    }
    *npool_out = pool;

    free(blk); free(first); free(last); free(head); free(nxt);
    free(freelist); free(act_last); free(act_idx);
    return slot;
}

/* Frame layout: variables first, each occupying its real size rounded
 * up to 8, then a coalesced pool of 8-byte temporary slots (K13). Returns the
 * per-vreg displacement table (caller frees). */
static int *layout_frame(struct ir_func *fn, int *frame_out,
                         int *scratch_base_out, int *sret_slot_out,
                         int *va_save_out, int *va_tag_out)
{
    struct func *f = fn->src;
    int *disp = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1)
                        * sizeof *disp);
    int running = 0;
    enum arg_class rcls[2];

    /* A function returning a MEMORY-class struct is handed a hidden
     * pointer in rdi; it must survive until the return, so it gets a
     * slot of its own. */
    *sret_slot_out = 0;
    if (f->ret_ty->kind == TY_STRUCT && ty_classify(f->ret_ty, rcls) == 0) {
        running += 8;
        *sret_slot_out = -running;
    }

    for (int i = 0; i < f->nvars; i++) {
        int sz = (ty_size(f->var_tys[i]) + 7) & ~7;
        running += sz;
        /* Alignment of a local's stack slot: the greater of its type's natural
         * alignment (a struct with an aligned(16) member, e.g. struct thread's
         * fpu_state, is itself 16-aligned) and any __attribute__((aligned(N)))
         * on the declarator. The base is rbp-running and rbp is 16-aligned on
         * entry, so rounding running up to N makes the base N-aligned — for
         * N <= 16. A larger request would need the stack realigned dynamically
         * (rbp can't promise it); refuse loudly rather than silently
         * under-align (THE RULE). */
        int al = f->var_aligns ? f->var_aligns[i] : 0;
        int tal = ty_align(f->var_tys[i]);
        if (tal > al) al = tal;
        if (al > 1) {
            if (al > 16)
                diag_fatal(f->file, f->line,
                           "a local in '%s' needs %d-byte alignment, exceeding "
                           "the 16-byte stack alignment EmbCC can guarantee",
                           f->name, al);
            running = (running + al - 1) & ~(al - 1);
        }
        disp[i] = -running;
    }
    /* Temporaries share a coalesced pool of 8-byte slots (K13) instead of one
     * slot each — the temp region is `npool` slots wide, not (nvregs-nvars). */
    int npool = 0;
    int *tslot = coalesce_temps(fn, f->nvars, &npool);
    int temp_base = running;
    for (int t = f->nvars; t < fn->nvregs; t++)
        disp[t] = -(temp_base + (tslot[t - f->nvars] + 1) * 8);
    running = temp_base + npool * 8;
    free(tslot);
    /* struct-return temporaries sit above the outgoing area */
    running += fn->scratch_bytes;
    *scratch_base_out = -running;
    /* A variadic function reserves the SysV register save area (6 int
     * eightbytes + 8 SSE sixteen-bytes = 176) plus one __va_list_tag (24)
     * that va_start initializes. Named source uses a single va_list, so
     * one tag suffices (a second concurrent va_list is a future seam). */
    *va_save_out = 0;
    *va_tag_out = 0;
    if (f->is_varargs) {
        running += 176;
        *va_save_out = -running;
        running += 24;
        *va_tag_out = -running;
    }
    /* The outgoing stack-argument area is the BOTTOM of the frame, so
     * it starts exactly at rsp and a call can address it as [rsp+off]
     * without moving rsp — which also keeps the 16-byte alignment the
     * ABI requires at every call, since the frame is a multiple of 16. */
    running += fn->outgoing_bytes;
    *frame_out = (running + 15) & ~15;
    return disp;
}

struct callsite {
    int patch_off;        /* offset of the rel32 field in text */
    struct func *target;
};

/* Growable site lists shared across the unit's functions. */
struct sites {
    struct callsite *call;
    int ncall, capcall;
    struct extcall *ext;
    int next, capext;
    struct strsite *str;
    int nstr, capstr;
    struct gsite *g;
    int ng, capg;
    struct fsite *f;
    int nf, capf;
};

#define PUSH(arr, n, cap, item)                                          \
    do {                                                                 \
        if ((n) == (cap)) {                                              \
            (cap) = (cap) ? (cap) * 2 : 16;                              \
            (arr) = xrealloc((arr), (size_t)(cap) * sizeof *(arr));      \
        }                                                                \
        (arr)[(n)++] = (item);                                           \
    } while (0)

/* setcc opcode byte per predicate; pointers and unsigned integers use
 * the unsigned condition set (b/be/a/ae). */
static int cc_for(enum binop pred, int sign)
{
    switch (pred) {
    case B_EQ: return 0x94;               /* sete */
    case B_NE: return 0x95;               /* setne */
    case B_LT: return sign ? 0x9c : 0x92; /* setl / setb */
    case B_LE: return sign ? 0x9e : 0x96; /* setle / setbe */
    case B_GT: return sign ? 0x9f : 0x97; /* setg / seta */
    case B_GE: return sign ? 0x9d : 0x93; /* setge / setae */
    default:
        fprintf(stderr, "embcc: internal: bad cmp predicate %d\n", pred);
        exit(1);
    }
}

/* A pending branch: where its rel32 displacement must be patched, and the
 * label it targets. File scope because EmbCC's own subset (which compiles
 * this file) does not permit block-scope struct definitions. */
struct brsite {
    int patch_off;
    int label;
};

/* -g: collect the (offset,line) line table into each ir_func. Off by default
 * so ordinary output is untouched (the self-host fixed point depends on it).
 * codegen runs one unit at a time, single threaded — a file-scope flag is
 * sound; codegen_unit sets it from want_debug. */
static int g_want_debug;

/* -mno-sse: never emit an SSE/xmm instruction. A kernel built before it turns
 * on CR4.OSFXSR needs this — any SSE op #UDs. Set by codegen_unit; the varargs
 * prologue skips its xmm spill, and a float operation is refused loudly rather
 * than silently emitting a faulting instruction (THE RULE). */
static int g_no_sse;

/* ---- local register (RAX) residency cache (the -O codegen step) ----
 *
 * Every vreg still owns a stack slot, but a value just computed into RAX
 * need not be reloaded from its slot to be used again. The cache records
 * which TEMP's value RAX currently holds and in which load shape, so an
 * identical reload is elided. Soundness rests on three facts:
 *   - only TEMPS are cached; their slots are never address-taken, so their
 *     memory is stable from the (single) store that defines them;
 *   - STORES are never elided, so a memory operand (a binary op's second
 *     source, read straight from a slot) is always the current value;
 *   - the cache is invalidated (cg_reset) wherever RAX is clobbered without
 *     a cg_load/cg_store fixing it, and at every basic-block boundary.
 * Off (g_regcache 0) the helpers are exactly the old direct calls, so -O0
 * output is byte-for-byte unchanged — which the self-host fixed point needs.
 */
static int g_regcache;        /* enabled only when optimizing */
static int rc_nvars;          /* vregs < this are locals/params (aliasable) */
static int rc_vreg = -1;      /* the temp whose value RAX holds, or -1 */
static int rc_size, rc_sign, rc_w;   /* the exact shape RAX holds it in */

static void cg_reset(void) { rc_vreg = -1; }

/* Load vreg into RAX, eliding the load when RAX already holds it. */
static void cg_load(struct code *text, const int *sd, int vreg,
                    int size, int sign, int w)
{
    if (g_regcache && vreg >= rc_nvars && rc_vreg == vreg &&
        rc_size == size && rc_sign == sign && rc_w == w)
        return;
    x86_load_slot(text, sd[vreg], size, sign, w);
    if (g_regcache && vreg >= rc_nvars) {
        rc_vreg = vreg; rc_size = size; rc_sign = sign; rc_w = w;
    } else {
        cg_reset();               /* a local (or cache off): do not cache */
    }
}

/* Store RAX (8 bytes) to a temp's slot and record that a (valw,0,valw)
 * reload reproduces RAX. A 32-bit result is zero-extended into the slot, so
 * for valw==4 both a 4- and an 8-byte reload would match the stored bytes. */
static void cg_store(struct code *text, const int *sd, int vreg, int valw)
{
    x86_store_slot(text, sd[vreg], 8);
    if (g_regcache) {
        rc_vreg = vreg; rc_size = valw; rc_sign = 0; rc_w = valw;
    }
}

static void gen_func(struct ir_func *fn, struct code *text,
                     struct sites *st)
{
    struct func *f = fn->src;
    int frame;
    int scratch_base;
    int sret_slot;
    int va_save, va_tag;
    int *sd = layout_frame(fn, &frame, &scratch_base, &sret_slot,
                           &va_save, &va_tag);
    /* Set by the parameter pass below, read by IR_VA_START: how many
     * named arguments the integer and SSE register files hold, and the
     * rbp offset of the first stack-passed argument (the overflow area). */
    int va_named_int = 0, va_named_sse = 0, va_overflow = 16;

    /* Branch targets and sites are function-local; both arrays are
     * resolved before this function returns. */
    int *label_off = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1)
                             * sizeof *label_off);
    for (int i = 0; i < fn->nlabels; i++)
        label_off[i] = -1;
    struct brsite *brs = NULL;
    int nbrs = 0, capbrs = 0;

    /* -g: expose each source variable's frame slot (rbp-relative) so the
     * DWARF emitter can write DW_OP_fbreg. sd is indexed by vreg; params and
     * locals are vregs [0, nvars), which is what dbgvars reference. */
    if (g_want_debug) {
        int nv = fn->src->nvars ? fn->src->nvars : 1;
        fn->var_off = xmalloc((size_t)nv * sizeof *fn->var_off);
        for (int v = 0; v < fn->src->nvars; v++)
            fn->var_off[v] = sd[v];
    }

    code_align(text, 16, 0x90);
    f->code_off = text->len;

    x86_prologue(text, frame);
    /* Variadic: spill the whole argument register file into the save area
     * FIRST, before the parameter pass below uses rcx/rax as scratch and
     * so clobbers the vararg registers. Storing a register does not alter
     * it, so the named-parameter loads that follow still see rdi..r9 and
     * xmm0..7 intact. The SSE slots are 16 apart (SysV) but only their low
     * 8 bytes — a double — are stored, which is all vfprintf reads. */
    if (f->is_varargs) {
        for (int r = 0; r < 6; r++)
            x86_store_mem_reg(text, REG_RBP, va_save + r * 8,
                              x86_argreg(r), 8);
        /* The SSE half of the save area is skipped under -mno-sse (the xmm
         * spill would #UD before CR4.OSFXSR is set). Sound because a callee
         * built -mno-sse takes no floating varargs, so va_arg never reads it;
         * callers must likewise pass al=0 (they do — no float args exist). */
        if (!g_no_sse)
            for (int r = 0; r < 8; r++)
                x86_movs_store_base(text, REG_RBP, va_save + 48 + r * 16,
                                    r, 8);
    }
    {   /* The same two-file split, in reverse. A hidden return pointer
         * (sret) consumes rdi BEFORE any real parameter, and MEMORY
         * parameters arrive on the caller's stack at [rbp+16...]. */
        int ireg = 0, freg = 0;
        enum arg_class rcls[2];
        int ret_mem = f->ret_ty->kind == TY_STRUCT &&
                      ty_classify(f->ret_ty, rcls) == 0;
        if (ret_mem) {
            x86_store_arg(text, ireg++, sret_slot);
        }
        int incoming = 16; /* saved rbp + return address */
        for (int i = 0; i < f->nparams; i++) {
            struct type *pt = f->param_tys[i];
            enum arg_class cls[2];
            int n = ty_classify(pt, cls);
            if (pt->kind != TY_STRUCT) {
                /* Register file exhausted -> the argument arrived on the
                 * caller's stack at [rbp+incoming] (mirrors the caller's
                 * on_stack decision in irgen). Copy the eightbyte into the
                 * local; without this the 7th+ integer parameter read a
                 * nonexistent register (argregs[6]). */
                int in_reg = ty_is_float(pt) ? (freg < 8) : (ireg < 6);
                if (!in_reg) {
                    x86_load_reg_mem(text, REG_RAX, REG_RBP, incoming, 8);
                    x86_store_slot(text, sd[i], 8);
                    incoming += 8;
                } else if (ty_is_float(pt)) {
                    x86_movs_store(text, freg++, sd[i], ty_size(pt));
                } else {
                    x86_store_arg(text, ireg++, sd[i]);
                }
                continue;
            }
            if (n == 0) {
                /* MEMORY: copy it out of the caller's frame into ours,
                 * so its address is a normal local. */
                int sz = ty_size(pt);
                x86_lea_reg_slot(text, REG_RCX, sd[i]);
                for (int off = 0; off < sz; off += 8) {
                    int chunk = sz - off >= 8 ? 8 : sz - off;
                    x86_load_reg_mem(text, REG_RAX, REG_RBP,
                                     incoming + off, chunk >= 8 ? 8 : chunk);
                    x86_store_mem_reg(text, REG_RCX, off, REG_RAX,
                                      chunk >= 8 ? 8 : chunk);
                }
                incoming += (sz + 7) & ~7;
                continue;
            }
            /* registers -> the parameter's own storage */
            x86_lea_reg_slot(text, REG_RCX, sd[i]);
            for (int k = 0; k < n; k++) {
                if (cls[k] == CLASS_SSE)
                    x86_movs_store_base(text, REG_RCX, k * 8, freg++, 8);
                else
                    x86_store_mem_reg(text, REG_RCX, k * 8,
                                      x86_argreg(ireg++), 8);
            }
        }
        va_named_int = ireg;
        va_named_sse = freg;
        va_overflow = incoming;
    }

    rc_nvars = fn->src->nvars;
    cg_reset();
    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        /* -g: a row where the source line changes. text->len is the .text
         * offset this instruction's code begins at (the switch below emits
         * it). Multiple IR ops from one statement share a line and collapse
         * to a single row; ops with no line (0) inherit the last row. */
        if (g_want_debug && i->line) {
            struct ir_line *last = fn->nlines ? &fn->lines[fn->nlines - 1]
                                              : (struct ir_line *)0;
            if (last && last->off == text->len) {
                last->line = i->line;   /* same PC: the latest line wins */
            } else if (!last || last->line != i->line) {
                if (fn->nlines == fn->linecap) {
                    fn->linecap = fn->linecap ? fn->linecap * 2 : 8;
                    fn->lines = xrealloc(fn->lines,
                                         (size_t)fn->linecap * sizeof *fn->lines);
                }
                fn->lines[fn->nlines].off = text->len;
                fn->lines[fn->nlines].line = i->line;
                fn->nlines++;
            }
        }
        /* -mno-sse: an operation that would touch an xmm register (float
         * math, an int<->float conversion) has no non-SSE lowering — refuse
         * it rather than emit a #UD. The kernel reaches this never (it has no
         * float math); if a caller does, the diagnostic names why. */
        if (g_no_sse && (i->flt || i->op == IR_I2F || i->op == IR_F2I ||
                         i->op == IR_F2F))
            diag_fatal(fn->src->file, i->line,
                       "floating point needs SSE, which -mno-sse forbids");
        switch (i->op) {
        case IR_CONST:
            x86_mov_eax_imm(text, i->imm, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_MOV:
            cg_load(text, sd, i->a, 8, 0, 8);
            cg_store(text, sd, i->dst, 8);
            break;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            if (i->flt) {
                cg_reset();
                x86_movs_load(text, 0, sd[i->a], i->w);
                x86_sse_alu_mem(text,
                                i->op == IR_ADD ? '+' :
                                i->op == IR_SUB ? '-' : '*',
                                sd[i->b], i->w);
                x86_movs_store(text, 0, sd[i->dst], i->w);
                break;
            }
            cg_load(text, sd, i->a, i->w, 0, i->w);
            x86_alu_eax_mem(text,
                            i->op == IR_ADD ? '+' :
                            i->op == IR_SUB ? '-' :
                            i->op == IR_MUL ? '*' :
                            i->op == IR_AND ? '&' :
                            i->op == IR_OR ? '|' : '^',
                            sd[i->b], i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_DIV:
        case IR_MOD:
            if (i->flt) { /* only DIV is ever float; MOD is integers */
                cg_reset();
                x86_movs_load(text, 0, sd[i->a], i->w);
                x86_sse_alu_mem(text, '/', sd[i->b], i->w);
                x86_movs_store(text, 0, sd[i->dst], i->w);
                break;
            }
            cg_load(text, sd, i->a, i->w, 0, i->w);
            if (i->sign)
                x86_cdq(text, i->w);
            else
                x86_zero_edx(text);
            x86_div_mem(text, sd[i->b], i->sign, i->w);
            if (i->op == IR_MOD)
                x86_mov_eax_edx(text, i->w); /* remainder lives in edx */
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_SHL:
        case IR_SHR:
            cg_load(text, sd, i->a, i->w, 0, i->w);
            x86_mov_ecx_mem(text, sd[i->b], 4);
            x86_shift_eax_cl(text,
                             i->op == IR_SHL ? '<' :
                             i->sign ? '>' : 'u', i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_NEG:
        case IR_BNOT:
            cg_load(text, sd, i->a, i->w, 0, i->w);
            if (i->op == IR_NEG)
                x86_neg_eax(text, i->w);
            else
                x86_not_eax(text, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_CMP:
            if (i->flt) {
                /* ucomis sets the UNSIGNED flags, so >,>= use seta/setae
                 * directly and <,<= are the same test with the operands
                 * swapped — which is also what makes NaN compare false
                 * in every direction. */
                cg_reset();
                int swap = i->pred == B_LT || i->pred == B_LE;
                x86_movs_load(text, 0, sd[swap ? i->b : i->a], i->w);
                x86_ucomis_mem(text, sd[swap ? i->a : i->b], i->w);
                if (i->pred == B_EQ || i->pred == B_NE)
                    x86_set_float_eq(text, i->pred == B_NE);
                else
                    x86_setcc_eax(text,
                                  cc_for(i->pred == B_LT ? B_GT :
                                         i->pred == B_LE ? B_GE : i->pred,
                                         0));
                cg_store(text, sd, i->dst, 4);   /* the 0/1 result is an int */
                break;
            }
            cg_load(text, sd, i->a, i->w, 0, i->w);
            x86_cmp_eax_mem(text, sd[i->b], i->w);
            x86_setcc_eax(text, cc_for(i->pred, i->sign));
            cg_store(text, sd, i->dst, 4);        /* the 0/1 result is an int */
            break;
        case IR_I2F:
            cg_reset();
            x86_cvtsi2s(text, sd[i->a], i->size, i->w);
            x86_movs_store(text, 0, sd[i->dst], i->w);
            break;
        case IR_F2I:
            cg_reset();
            x86_cvtts2si(text, sd[i->a], i->size, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_F2F:
            cg_reset();
            x86_cvts2s(text, sd[i->a], i->size);
            x86_movs_store(text, 0, sd[i->dst], i->w);
            break;
        case IR_LDVAR:
            cg_load(text, sd, i->a, i->size, i->sign, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_STVAR:
            cg_load(text, sd, i->a, 8, 0, 8);
            x86_store_slot(text, sd[i->dst], i->size);   /* dst is a local */
            break;
        case IR_ADDR:
            x86_lea_rax_slot(text, sd[i->a]);
            cg_store(text, sd, i->dst, 8);
            break;
        case IR_STRADDR: {
            struct strsite ss;
            ss.patch_off = x86_lea_rax_rip(text);
            ss.str_off = i->label;  /* resolved to an offset below */
            PUSH(st->str, st->nstr, st->capstr, ss);
            cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_GADDR: {
            struct gsite gs;
            gs.patch_off = x86_lea_rax_rip(text);
            gs.glob = i->glob;
            PUSH(st->g, st->ng, st->capg, gs);
            cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_FADDR: {
            struct fsite fs;
            fs.patch_off = x86_lea_rax_rip(text);
            fs.target = i->callee;
            PUSH(st->f, st->nf, st->capf, fs);
            cg_store(text, sd, i->dst, 8);
            break;
        }
        case IR_LOAD:
            cg_load(text, sd, i->a, 8, 0, 8);       /* the address */
            x86_load_mem_rax(text, i->size, i->sign, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_STORE:
            x86_mov_rcx_slot(text, sd[i->a]);       /* the address -> rcx */
            cg_load(text, sd, i->b, 8, 0, 8);       /* the value -> rax */
            x86_store_mem_rcx(text, i->size);
            break;
        case IR_EXT:
            /* re-extend from the low `size` bytes of the temp's slot */
            cg_load(text, sd, i->a, i->size, i->sign, i->w);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_BSWAP:
            cg_load(text, sd, i->a, i->size, 0, i->size == 8 ? 8 : 4);
            x86_bswap(text, i->size);
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_FENCE:
            x86_mfence(text);   /* leaves RAX untouched: cache stays valid */
            break;
        case IR_UD2:
            x86_ud2(text);
            break;
        case IR_XCHG:
            cg_reset();
            x86_mov_rcx_slot(text, sd[i->a]);            /* address -> rcx */
            x86_load_slot(text, sd[i->b], i->size, 0,
                          i->size == 8 ? 8 : 4);         /* new value -> rax */
            x86_xchg_rax_mem_rcx(text, i->size);         /* atomic; rax = old */
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_XADD:
            cg_reset();
            x86_mov_rcx_slot(text, sd[i->a]);            /* address -> rcx */
            x86_load_slot(text, sd[i->b], i->size, 0,
                          i->size == 8 ? 8 : 4);         /* addend -> rax */
            x86_lock_xadd_rcx(text, i->size);            /* atomic; rax = old */
            cg_store(text, sd, i->dst, i->w);
            break;
        case IR_CMPXCHG:
            cg_reset();
            x86_load_slot(text, sd[i->c], i->size, 0,
                          i->size == 8 ? 8 : 4);         /* desired -> rax.. */
            x86_mov_reg_reg(text, REG_RDX, REG_RAX);     /* ..-> rdx */
            x86_mov_rcx_slot(text, sd[i->a]);            /* object ptr -> rcx */
            x86_load_slot(text, sd[i->b], 8, 0, 8);      /* &expected -> rax */
            x86_mov_reg_reg(text, REG_RSI, REG_RAX);     /* save in rsi */
            x86_load_reg_mem(text, REG_RAX, REG_RSI, 0, i->size); /* rax=*exp */
            x86_lock_cmpxchg_rcx(text, i->size);         /* CAS; ZF=matched */
            x86_store_mem_reg(text, REG_RSI, 0, REG_RAX, i->size);/* *exp=seen */
            x86_setcc_eax(text, 0x94);                   /* setz: dst = matched */
            cg_store(text, sd, i->dst, 4);
            break;
        case IR_MEMCPY: {
            /* a struct copy: 8 bytes at a time, then the tail */
            cg_reset();
            x86_load_slot(text, sd[i->a], 8, 0, 8);
            x86_mov_reg_reg(text, REG_RCX, REG_RAX);       /* dst */
            x86_load_slot(text, sd[i->b], 8, 0, 8);
            x86_mov_reg_reg(text, REG_RDX, REG_RAX);       /* src */
            int off = 0;
            while (off < i->size) {
                int chunk = i->size - off;
                chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4 : chunk >= 2 ? 2 : 1;
                x86_load_reg_mem(text, REG_RAX, REG_RDX, off, chunk);
                x86_store_mem_reg(text, REG_RCX, off, REG_RAX, chunk);
                off += chunk;
            }
            break;
        }
        case IR_MEMZERO: {
            cg_reset();
            x86_load_slot(text, sd[i->a], 8, 0, 8);
            x86_mov_reg_reg(text, REG_RCX, REG_RAX);
            x86_mov_eax_imm(text, 0, 8);
            int off = 0;
            while (off < i->size) {
                int chunk = i->size - off;
                chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                      : chunk >= 2 ? 2 : 1;
                x86_store_mem_reg(text, REG_RCX, off, REG_RAX, chunk);
                off += chunk;
            }
            break;
        }
        case IR_LABEL:
            cg_reset();      /* a merge point: RAX is unknown here */
            label_off[i->label] = text->len;
            break;
        case IR_JMP:
        case IR_BRZ:
        case IR_BRNZ: {
            int patch;
            if (i->op == IR_JMP) {
                patch = x86_jmp_rel32(text);
            } else {
                cg_load(text, sd, i->a, i->w, 0, i->w);
                x86_test_eax(text, i->w);
                patch = i->op == IR_BRZ ? x86_jz_rel32(text)
                                        : x86_jnz_rel32(text);
            }
            cg_reset();      /* control splits: don't carry RAX across */
            if (nbrs == capbrs) {
                capbrs = capbrs ? capbrs * 2 : 16;
                brs = xrealloc(brs, (size_t)capbrs * sizeof *brs);
            }
            brs[nbrs].patch_off = patch;
            brs[nbrs].label = i->label;
            nbrs++;
            break;
        }
        case IR_CALL: {
            /* SysV walks TWO register files independently: integers and
             * pointers take rdi..r9, floats take xmm0..7. */
            cg_reset();     /* a call clobbers every caller-saved register */
            int ireg = 0, freg = 0;

            /* MEMORY-class aggregates go to the outgoing area first,
             * while rax/rcx/rdx are still free to copy with. */
            for (int k = 0; k < i->nargs; k++) {
                struct ir_arg *a = &i->argv[k];
                if (!a->on_stack)
                    continue;
                if (!a->is_struct) {
                    /* a scalar that ran out of registers: its slot
                     * already holds the value, extended to 8 bytes */
                    x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                    x86_store_mem_reg(text, REG_RSP, a->stk_off,
                                      REG_RAX, 8);
                    continue;
                }
                x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                x86_mov_reg_reg(text, REG_RDX, REG_RAX); /* src */
                int sz = a->size;
                for (int off = 0; off < sz; ) {
                    int chunk = sz - off;
                    chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                          : chunk >= 2 ? 2 : 1;
                    x86_load_reg_mem(text, REG_RAX, REG_RDX, off, chunk);
                    x86_store_mem_reg(text, REG_RSP,
                                      a->stk_off + off, REG_RAX, chunk);
                    off += chunk;
                }
            }
            /* A struct returned in MEMORY takes rdi as a hidden pointer
             * to the caller's scratch, before any real argument. */
            if (i->retsize && i->retnclass == 0) {
                x86_lea_reg_slot(text, REG_RDI,
                                 scratch_base + i->scratch);
                ireg++;
            }
            for (int k = 0; k < i->nargs; k++) {
                struct ir_arg *a = &i->argv[k];
                if (a->on_stack)
                    continue; /* placed above */
                if (a->is_struct) {
                    x86_load_slot(text, sd[a->vreg], 8, 0, 8);
                    for (int q = 0; q < a->nclass; q++) {
                        if (a->cls[q] == CLASS_SSE)
                            x86_movs_load_base(text, freg++, REG_RAX,
                                               q * 8, 8);
                        else
                            x86_load_reg_mem(text, x86_argreg(ireg++),
                                             REG_RAX, q * 8, 8);
                    }
                    continue;
                }
                if (a->cls[0] == CLASS_SSE)
                    x86_movs_load(text, freg++, sd[a->vreg], a->size);
                else
                    x86_load_arg(text, ireg++, sd[a->vreg]);
            }
            if (i->indirect)
                x86_mov_r11_slot(text, sd[i->a]);
            /* al = the number of VECTOR registers used. Zero was right
             * only while no floats existed; a variadic callee reads it
             * to find the register save area, so a wrong al is exactly
             * the kind of silent wrongness THE RULE is about. */
            if (i->call_varargs) {
                if (freg)
                    x86_mov_al_imm(text, freg);
                else
                    x86_zero_eax(text);
            }
            if (i->indirect) {
                x86_call_r11(text);
            } else {
                int patch = x86_call_rel32(text);
                if (i->callee->has_defn) {
                    struct callsite cs;
                    cs.patch_off = patch;
                    cs.target = i->callee;
                    PUSH(st->call, st->ncall, st->capcall, cs);
                } else {
                    struct extcall ec;
                    ec.patch_off = patch;
                    ec.callee = i->callee;
                    PUSH(st->ext, st->next, st->capext, ec);
                }
            }
            if (i->retsize) {
                /* The value of a struct call is the ADDRESS it landed
                 * at: the scratch we reserved. A MEMORY return already
                 * wrote there; a register return is unpacked into it. */
                if (i->retnclass > 0) {
                    x86_lea_reg_slot(text, REG_RCX,
                                     scratch_base + i->scratch);
                    int ir = 0, fr = 0;
                    for (int q = 0; q < i->retnclass; q++) {
                        if (i->retcls[q] == CLASS_SSE)
                            x86_movs_store_base(text, REG_RCX, q * 8,
                                                fr++, 8);
                        else
                            x86_store_mem_reg(text, REG_RCX, q * 8,
                                              ir++ == 0 ? REG_RAX
                                                        : REG_RDX, 8);
                    }
                }
                x86_lea_rax_slot(text, scratch_base + i->scratch);
                cg_store(text, sd, i->dst, 8);
                break;
            }
            if (i->flt)
                x86_movs_store(text, 0, sd[i->dst], i->w);   /* stays reset */
            else
                cg_store(text, sd, i->dst, i->w);
            break;
        }
        case IR_ASM: {
            /* Extended asm. Load each input from its stack slot into the
             * fixed register its constraint chose, emit the assembled
             * template, then store each output register through the
             * lvalue address (also in a slot). Clobbers need nothing:
             * every live value is in memory, never a register, across the
             * asm. A scratch that avoids the output register carries the
             * address so the result register survives the store. */
            cg_reset();   /* the template may clobber any register */
            struct ir_asm *ia = i->asm_ir;
            /* Inputs carry their VALUE: a GPR ('r'/fixed) operand loads from its
             * slot into the register; an xmm ('x', reg 16..23) uses movss/movsd
             * into the xmm register instead. */
            for (int k = 0; k < ia->nin; k++)
                if (ia->in[k].reg >= 16)
                    x86_movs_load(text, ia->in[k].reg - 16,
                                  sd[ia->in[k].temp], ia->in[k].size);
                else
                    x86_load_reg_mem(text, ia->in[k].reg, REG_RBP,
                                     sd[ia->in[k].temp], 8);
            for (int k = 0; k < ia->codelen; k++)
                code_byte(text, ia->code[k]);
            /* The address scratch must not be an OUTPUT register, or loading
             * it would clobber a result before it is stored (e.g. cpuid's
             * four a/b/c/d outputs). Pick one free of every operand. Only GPR
             * operands (reg < 16) can collide with a GPR scratch. */
            int used16[16] = { 0 };
            for (int k = 0; k < ia->nin; k++)
                if (ia->in[k].reg < 16) used16[ia->in[k].reg] = 1;
            for (int k = 0; k < ia->nout; k++)
                if (ia->out[k].reg < 16) used16[ia->out[k].reg] = 1;
            int scr = -1;
            static const int scr_pool[] = { REG_RCX, REG_RDX, REG_RSI,
                                            REG_RDI, 8, 9, 10, 11 };
            for (unsigned p = 0; p < sizeof scr_pool / sizeof scr_pool[0]; p++)
                if (!used16[scr_pool[p]]) { scr = scr_pool[p]; break; }
            /* Outputs store the result register THROUGH the lvalue address
             * (held in the operand's slot). xmm results go out via movss/movsd. */
            for (int k = 0; k < ia->nout; k++) {
                x86_load_reg_mem(text, scr, REG_RBP,
                                 sd[ia->out[k].temp], 8);
                if (ia->out[k].reg >= 16)
                    x86_movs_store_base(text, scr, 0, ia->out[k].reg - 16,
                                        ia->out[k].size);
                else
                    x86_store_mem_reg(text, scr, 0, ia->out[k].reg,
                                      ia->out[k].size);
            }
            break;
        }
        case IR_VA_START:
            /* Build a __va_list_tag on the frame and point the va_list at
             * it. Layout (SysV): gp_offset u32, fp_offset u32,
             * overflow_arg_area ptr, reg_save_area ptr. */
            cg_reset();
            x86_mov_eax_imm(text, va_named_int * 8, 4);
            x86_store_mem_reg(text, REG_RBP, va_tag + 0, REG_RAX, 4);
            x86_mov_eax_imm(text, 48 + va_named_sse * 16, 4);
            x86_store_mem_reg(text, REG_RBP, va_tag + 4, REG_RAX, 4);
            x86_lea_reg_slot(text, REG_RAX, va_overflow);
            x86_store_mem_reg(text, REG_RBP, va_tag + 8, REG_RAX, 8);
            x86_lea_reg_slot(text, REG_RAX, va_save);
            x86_store_mem_reg(text, REG_RBP, va_tag + 16, REG_RAX, 8);
            /* *ap = &tag  (i->a holds the address of the va_list) */
            x86_mov_rcx_slot(text, sd[i->a]);
            x86_lea_reg_slot(text, REG_RAX, va_tag);
            x86_store_mem_reg(text, REG_RCX, 0, REG_RAX, 8);
            break;
        case IR_RET:
            cg_reset();
            if (i->a >= 0 && f->ret_ty->kind == TY_STRUCT) {
                enum arg_class rc[2];
                int rn = ty_classify(f->ret_ty, rc);
                int sz = ty_size(f->ret_ty);
                x86_load_slot(text, sd[i->a], 8, 0, 8);
                x86_mov_reg_reg(text, REG_RDX, REG_RAX); /* the value */
                if (rn == 0) {
                    /* MEMORY: copy into the caller's buffer and hand
                     * the pointer back in rax, as the ABI requires. */
                    x86_load_slot(text, sret_slot, 8, 0, 8);
                    x86_mov_reg_reg(text, REG_RCX, REG_RAX);
                    for (int off = 0; off < sz; ) {
                        int chunk = sz - off;
                        chunk = chunk >= 8 ? 8 : chunk >= 4 ? 4
                              : chunk >= 2 ? 2 : 1;
                        x86_load_reg_mem(text, REG_RAX, REG_RDX, off,
                                         chunk);
                        x86_store_mem_reg(text, REG_RCX, off, REG_RAX,
                                          chunk);
                        off += chunk;
                    }
                    x86_load_slot(text, sret_slot, 8, 0, 8);
                } else {
                    /* Small enough to travel in registers: eightbytes
                     * take the next register of their OWN class, so
                     * INTEGER fills rax then rdx and SSE fills xmm0
                     * then xmm1. The address is held in rcx because rdx
                     * is itself a destination. */
                    x86_mov_reg_reg(text, REG_RCX, REG_RDX);
                    int ir = 0, fr = 0;
                    for (int q = 0; q < rn; q++) {
                        if (rc[q] == CLASS_SSE)
                            x86_movs_load_base(text, fr++, REG_RCX,
                                               q * 8, 8);
                        else
                            x86_load_reg_mem(text,
                                             ir++ == 0 ? REG_RAX : REG_RDX,
                                             REG_RCX, q * 8, 8);
                    }
                }
            } else if (i->a >= 0 && i->flt) {
                x86_movs_load(text, 0, sd[i->a], i->w);
            } else if (i->a >= 0) {
                x86_load_slot(text, sd[i->a], 8, 0, 8);
            }
            x86_epilogue(text);
            break;
        }
    }

    /* Every function ends with an epilogue, whether or not its last
     * statement was a return. A void function may legally fall off the
     * end (sema only demands a return from value-returning ones), and
     * without this it fell straight into the NEXT function's code —
     * silently, since nothing crashes until a stray ret runs. A dead
     * `leave; ret` after an explicit return costs two bytes. */
    x86_epilogue(text);

    for (int n = 0; n < nbrs; n++) {
        int target = label_off[brs[n].label];
        if (target < 0) {
            fprintf(stderr, "embcc: internal: label %d in '%s' was never "
                            "placed\n", brs[n].label, f->name);
            exit(1);
        }
        int from = brs[n].patch_off + 4;
        code_patch32(text, brs[n].patch_off,
                     (unsigned long)(unsigned int)(target - from));
    }
    free(brs);
    free(label_off);
    free(sd);

    f->code_len = text->len - f->code_off;
}

void codegen_unit(struct ir_unit *iu, struct code *text,
                  struct extcall **ext, int *next,
                  struct strsite **strs, int *nstrs,
                  struct gsite **gs, int *ngs,
                  struct fsite **fs, int *nfs, int want_debug, int optimize,
                  int no_sse)
{
    struct sites st = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    g_want_debug = want_debug;
    g_regcache = optimize;
    g_no_sse = no_sse;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &st);

    /* All targets are placed now; resolve the intra-unit calls.
     * rel32 is relative to the end of the call instruction. */
    for (int n = 0; n < st.ncall; n++) {
        int from = st.call[n].patch_off + 4;
        long rel = (long)st.call[n].target->code_off - from;
        code_patch32(text, st.call[n].patch_off,
                     (unsigned long)(unsigned int)rel);
    }
    free(st.call);

    /* String sites still carry the string INDEX; turn it into the
     * .rodata offset the driver's relocations speak. */
    for (int n = 0; n < st.nstr; n++)
        st.str[n].str_off = iu->strs[st.str[n].str_off].off;

    *ext = st.ext;
    *next = st.next;
    *strs = st.str;
    *nstrs = st.nstr;
    *gs = st.g;
    *ngs = st.ng;
    *fs = st.f;
    *nfs = st.nf;
}
