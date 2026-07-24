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
static int *layout_frame(struct ir_func *fn, int *frame_out,
                         int *scratch_base_out, int *sret_slot_out)
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
        disp[i] = -running;
    }
    for (int t = f->nvars; t < fn->nvregs; t++) {
        running += 8;
        disp[t] = -running;
    }
    /* struct-return temporaries sit above the outgoing area */
    running += fn->scratch_bytes;
    *scratch_base_out = -running;
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

static void gen_func(struct ir_func *fn, struct code *text,
                     struct sites *st)
{
    struct func *f = fn->src;
    int frame;
    int scratch_base;
    int sret_slot;
    int *sd = layout_frame(fn, &frame, &scratch_base, &sret_slot);

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
                if (ty_is_float(pt))
                    x86_movs_store(text, freg++, sd[i], ty_size(pt));
                else
                    x86_store_arg(text, ireg++, sd[i]);
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
    }

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
            if (i->flt) {
                x86_movs_load(text, 0, sd[i->a], i->w);
                x86_sse_alu_mem(text,
                                i->op == IR_ADD ? '+' :
                                i->op == IR_SUB ? '-' : '*',
                                sd[i->b], i->w);
                x86_movs_store(text, 0, sd[i->dst], i->w);
                break;
            }
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
            if (i->flt) { /* only DIV is ever float; MOD is integers */
                x86_movs_load(text, 0, sd[i->a], i->w);
                x86_sse_alu_mem(text, '/', sd[i->b], i->w);
                x86_movs_store(text, 0, sd[i->dst], i->w);
                break;
            }
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
            if (i->flt) {
                /* ucomis sets the UNSIGNED flags, so >,>= use seta/setae
                 * directly and <,<= are the same test with the operands
                 * swapped — which is also what makes NaN compare false
                 * in every direction. */
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
                x86_store_slot(text, sd[i->dst], 8);
                break;
            }
            x86_load_slot(text, sd[i->a], i->w, 0, i->w);
            x86_cmp_eax_mem(text, sd[i->b], i->w);
            x86_setcc_eax(text, cc_for(i->pred, i->sign));
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_I2F:
            x86_cvtsi2s(text, sd[i->a], i->size, i->w);
            x86_movs_store(text, 0, sd[i->dst], i->w);
            break;
        case IR_F2I:
            x86_cvtts2si(text, sd[i->a], i->size, i->w);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        case IR_F2F:
            x86_cvts2s(text, sd[i->a], i->size);
            x86_movs_store(text, 0, sd[i->dst], i->w);
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
        case IR_GADDR: {
            struct gsite gs;
            gs.patch_off = x86_lea_rax_rip(text);
            gs.glob = i->glob;
            PUSH(st->g, st->ng, st->capg, gs);
            x86_store_slot(text, sd[i->dst], 8);
            break;
        }
        case IR_FADDR: {
            struct fsite fs;
            fs.patch_off = x86_lea_rax_rip(text);
            fs.target = i->callee;
            PUSH(st->f, st->nf, st->capf, fs);
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
        case IR_MEMCPY: {
            /* a struct copy: 8 bytes at a time, then the tail */
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
            label_off[i->label] = text->len;
            break;
        case IR_JMP:
        case IR_BRZ:
        case IR_BRNZ: {
            int patch;
            if (i->op == IR_JMP) {
                patch = x86_jmp_rel32(text);
            } else {
                x86_load_slot(text, sd[i->a], i->w, 0, i->w);
                x86_test_eax(text, i->w);
                patch = i->op == IR_BRZ ? x86_jz_rel32(text)
                                        : x86_jnz_rel32(text);
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
            /* SysV walks TWO register files independently: integers and
             * pointers take rdi..r9, floats take xmm0..7. */
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
                x86_store_slot(text, sd[i->dst], 8);
                break;
            }
            if (i->flt)
                x86_movs_store(text, 0, sd[i->dst], i->w);
            else
                x86_store_slot(text, sd[i->dst], 8);
            break;
        }
        case IR_RET:
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
                  struct fsite **fs, int *nfs)
{
    struct sites st = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

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
