/* IR → x86-64, System V AMD64 (ARCHITECTURE §4). Deliberately naive:
 * every vreg lives in a stack slot, every operation goes through eax.
 * Correct-and-slow first — register allocation is a post-M4 reason to
 * exist, not an M1 one (ARCHITECTURE §3).
 */
#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>

#include "../driver/util.h"

/* vreg v lives at [rbp + slot_disp(v)] */
static int slot_disp(int v) { return -8 * (v + 1); }

static int frame_size(int nvregs)
{
    int n = nvregs * 8;
    return (n + 15) & ~15;
}

struct callsite {
    int patch_off;        /* offset of the rel32 field in text */
    struct func *target;
};

/* setcc condition byte for each comparison predicate (signed int). */
static int cc_for(enum binop pred)
{
    switch (pred) {
    case B_EQ: return 0x94; /* sete  */
    case B_NE: return 0x95; /* setne */
    case B_LT: return 0x9c; /* setl  */
    case B_LE: return 0x9e; /* setle */
    case B_GT: return 0x9f; /* setg  */
    case B_GE: return 0x9d; /* setge */
    default:
        fprintf(stderr, "embcc: internal: bad cmp predicate %d\n", pred);
        exit(1);
    }
}

static void gen_func(struct ir_func *fn, struct code *text,
                     struct callsite **sites, int *nsites, int *capsites,
                     struct extcall **ext, int *next, int *capext)
{
    struct func *f = fn->src;

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

    x86_prologue(text, frame_size(fn->nvregs));
    for (int i = 0; i < f->nparams; i++)
        x86_store_arg(text, i, slot_disp(i));

    for (int n = 0; n < fn->nins; n++) {
        struct ir_ins *i = &fn->ins[n];
        switch (i->op) {
        case IR_CONST:
            x86_mov_eax_imm32(text, i->imm);
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_MOV:
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_alu_eax_mem(text,
                            i->op == IR_ADD ? '+' :
                            i->op == IR_SUB ? '-' :
                            i->op == IR_MUL ? '*' :
                            i->op == IR_AND ? '&' :
                            i->op == IR_OR ? '|' : '^',
                            slot_disp(i->b));
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_DIV:
        case IR_MOD:
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_cdq(text);
            x86_idiv_mem(text, slot_disp(i->b));
            if (i->op == IR_MOD)
                x86_mov_eax_edx(text); /* remainder lives in edx */
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_SHL:
        case IR_SHR:
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_mov_ecx_mem(text, slot_disp(i->b));
            if (i->op == IR_SHL)
                x86_shl_eax_cl(text);
            else
                x86_sar_eax_cl(text);
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_NEG:
        case IR_BNOT:
            x86_mov_eax_mem(text, slot_disp(i->a));
            if (i->op == IR_NEG)
                x86_neg_eax(text);
            else
                x86_not_eax(text);
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_CMP:
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_cmp_eax_mem(text, slot_disp(i->b));
            x86_setcc_eax(text, cc_for(i->pred));
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_LABEL:
            label_off[i->label] = text->len;
            break;
        case IR_JMP:
        case IR_BRZ: {
            int patch;
            if (i->op == IR_BRZ) {
                x86_mov_eax_mem(text, slot_disp(i->a));
                x86_test_eax(text);
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
                x86_load_arg(text, k, slot_disp(i->args[k]));
            int patch = x86_call_rel32(text);
            if (i->callee->has_defn) {
                if (*nsites == *capsites) {
                    *capsites = *capsites ? *capsites * 2 : 16;
                    *sites = xrealloc(*sites,
                                      (size_t)*capsites * sizeof **sites);
                }
                (*sites)[*nsites].patch_off = patch;
                (*sites)[*nsites].target = i->callee;
                (*nsites)++;
            } else {
                if (*next == *capext) {
                    *capext = *capext ? *capext * 2 : 16;
                    *ext = xrealloc(*ext, (size_t)*capext * sizeof **ext);
                }
                (*ext)[*next].patch_off = patch;
                (*ext)[*next].callee = i->callee;
                (*next)++;
            }
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        }
        case IR_RET:
            x86_mov_eax_mem(text, slot_disp(i->a));
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

    f->code_len = text->len - f->code_off;
}

void codegen_unit(struct ir_unit *iu, struct code *text,
                  struct extcall **ext, int *next)
{
    struct callsite *sites = NULL;
    int nsites = 0, capsites = 0;
    int capext = 0;

    *ext = NULL;
    *next = 0;
    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &sites, &nsites, &capsites,
                 ext, next, &capext);

    /* All targets are placed now; resolve the intra-unit calls.
     * rel32 is relative to the end of the call instruction. */
    for (int n = 0; n < nsites; n++) {
        int from = sites[n].patch_off + 4;
        long rel = (long)sites[n].target->code_off - from;
        code_patch32(text, sites[n].patch_off,
                     (unsigned long)(unsigned int)rel);
    }
    free(sites);
}
