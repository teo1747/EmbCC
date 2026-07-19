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

static void gen_func(struct ir_func *fn, struct code *text,
                     struct callsite **sites, int *nsites, int *capsites)
{
    struct func *f = fn->src;

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
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_alu_eax_mem(text,
                            i->op == IR_ADD ? '+' :
                            i->op == IR_SUB ? '-' : '*',
                            slot_disp(i->b));
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        case IR_CALL: {
            for (int k = 0; k < i->nargs; k++)
                x86_load_arg(text, k, slot_disp(i->args[k]));
            int patch = x86_call_rel32(text);
            if (*nsites == *capsites) {
                *capsites = *capsites ? *capsites * 2 : 16;
                *sites = xrealloc(*sites,
                                  (size_t)*capsites * sizeof **sites);
            }
            (*sites)[*nsites].patch_off = patch;
            (*sites)[*nsites].target = i->callee;
            (*nsites)++;
            x86_mov_mem_eax(text, slot_disp(i->dst));
            break;
        }
        case IR_RET:
            x86_mov_eax_mem(text, slot_disp(i->a));
            x86_epilogue(text);
            break;
        }
    }

    f->code_len = text->len - f->code_off;
}

void codegen_unit(struct ir_unit *iu, struct code *text)
{
    struct callsite *sites = NULL;
    int nsites = 0, capsites = 0;

    for (int n = 0; n < iu->nfuncs; n++)
        gen_func(&iu->funcs[n], text, &sites, &nsites, &capsites);

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
