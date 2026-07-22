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

/* Frame layout: variables first, each occupying its real size rounded
 * up to 8, then one 8-byte slot per temporary. Returns the per-vreg
 * displacement table (caller frees). */
static int *layout_frame(struct ir_func *fn, int *frame_out)
{
    struct func *f = fn->src;
    int *disp = xmalloc((size_t)(fn->nvregs ? fn->nvregs : 1)
                        * sizeof *disp);
    int running = 0;

    for (int i = 0; i < f->nvars; i++) {
        int sz = (ty_size(f->var_tys[i]) + 7) & ~7;
        running += sz;
        disp[i] = -running;
    }
    for (int t = f->nvars; t < fn->nvregs; t++) {
        running += 8;
        disp[t] = -running;
    }
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

static void gen_func(struct ir_func *fn, struct code *text,
                     struct sites *st)
{
    struct func *f = fn->src;
    int frame;
    int *sd = layout_frame(fn, &frame);

    /* Branch targets and sites are function-local; both arrays are
     * resolved before this function returns. */
    int *label_off = xmalloc((size_t)(fn->nlabels ? fn->nlabels : 1)
                             * sizeof *label_off);
    for (int i = 0; i < fn->nlabels; i++)
        label_off[i] = -1;
    struct brsite {
        int patch_off;
        int label;
    } *brs = NULL;
    int nbrs = 0, capbrs = 0;

    code_align(text, 16, 0x90);
    f->code_off = text->len;

    x86_prologue(text, frame);
    for (int i = 0; i < f->nparams; i++)
        x86_store_arg(text, i, sd[i]);

    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_CONST:
            x86_mov_eax_imm(text, i->imm, i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_MOV:
            x86_load_slot(text, sd[i->a], 8, 0, 8);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            x86_load_slot(text, sd[i->a], i->w, 0, i->w);
            x86_alu_eax_mem(text,
                            i->op == IR_ADD ? '+' :
                            i->op == IR_SUB ? '-' :
                            i->op == IR_MUL ? '*' :
                            i->op == IR_AND ? '&' :
                            i->op == IR_OR ? '|' : '^',
                            sd[i->b], i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_DIV:
        case IR_MOD:
            x86_load_slot(text, sd[i->a], i->w, 0, i->w);
            if (i->sign)
                x86_cdq(text, i->w);
            else
                x86_zero_edx(text);
            x86_div_mem(text, sd[i->b], i->sign, i->w);
            if (i->op == IR_MOD)
                x86_mov_eax_edx(text, i->w); /* remainder lives in edx */
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_SHL:
        case IR_SHR:
            x86_load_slot(text, sd[i->a], i->w, 0, i->w);
            x86_mov_ecx_mem(text, sd[i->b], 4);
            x86_shift_eax_cl(text,
                             i->op == IR_SHL ? '<' :
                             i->sign ? '>' : 'u', i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_NEG:
        case IR_BNOT:
            x86_load_slot(text, sd[i->a], i->w, 0, i->w);
            if (i->op == IR_NEG)
                x86_neg_eax(text, i->w);
            else
                x86_not_eax(text, i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_CMP:
            x86_load_slot(text, sd[i->a], i->w, 0, i->w);
            x86_cmp_eax_mem(text, sd[i->b], i->w);
            x86_setcc_eax(text, cc_for(i->pred, i->sign));
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_LDVAR:
            x86_load_slot(text, sd[i->a], i->size, i->sign, i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_STVAR:
            x86_load_slot(text, sd[i->a], 8, 0, 8);
            x86_store_slot(text, sd[i->dst], i->size);
            break;
        case IR_ADDR:
            x86_lea_rax_slot(text, sd[i->a]);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_STRADDR: {
            struct strsite ss;
            ss.patch_off = x86_lea_rax_rip(text);
            ss.str_off = i->label;  /* resolved to an offset below */
            PUSH(st->str, st->nstr, st->capstr, ss);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        }
        case IR_LOAD:
            x86_load_slot(text, sd[i->a], 8, 0, 8); /* the address */
            x86_load_mem_rax(text, i->size, i->sign, i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_STORE:
            x86_mov_rcx_slot(text, sd[i->a]);       /* the address */
            x86_load_slot(text, sd[i->b], 8, 0, 8); /* the value */
            x86_store_mem_rcx(text, i->size);
            break;
        case IR_EXT:
            /* re-extend from the low `size` bytes of the temp's slot */
            x86_load_slot(text, sd[i->a], i->size, i->sign, i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_LABEL:
            label_off[i->label] = text->len;
            break;
        case IR_JMP:
        case IR_BRZ: {
            int patch;
            if (i->op == IR_BRZ) {
                x86_load_slot(text, sd[i->a], i->w, 0, i->w);
                x86_test_eax(text, i->w);
                patch = x86_jz_rel32(text);
            } else {
                patch = x86_jmp_rel32(text);
            }
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
            for (int k = 0; k < i->nargs; k++)
                x86_load_arg(text, k, sd[i->args[k]]);
            if (i->callee->is_varargs)
                x86_zero_eax(text); /* SysV: al = # of vector args = 0 */
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
            x86_store_slot(text, sd[i->dst], 8);
            break;
        }
        case IR_RET:
            if (i->a >= 0)
                x86_load_slot(text, sd[i->a], 8, 0, 8);
            x86_epilogue(text);
            break;
        }
    }

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
                  struct strsite **strs, int *nstrs)
{
    struct sites st = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

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
}
