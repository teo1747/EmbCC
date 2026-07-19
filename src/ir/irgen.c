#include "ir.h"

#include "../driver/util.h"

static struct ir_ins *emit(struct ir_func *fn)
{
    if (fn->nins == fn->cap) {
        fn->cap = fn->cap ? fn->cap * 2 : 16;
        fn->ins = xrealloc(fn->ins, (size_t)fn->cap * sizeof *fn->ins);
    }
    struct ir_ins *i = &fn->ins[fn->nins++];
    i->op = IR_CONST;
    i->dst = i->a = i->b = -1;
    i->imm = 0;
    i->callee = 0;
    i->nargs = 0;
    return i;
}

static int new_temp(struct ir_func *fn) { return fn->nvregs++; }

static int gen_expr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM: {
        struct ir_ins *i = emit(fn);
        i->op = IR_CONST;
        i->imm = e->num;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_VAR:
        return e->var_index;
    case EXPR_BINOP: {
        int a = gen_expr(fn, e->lhs);
        int b = gen_expr(fn, e->rhs);
        struct ir_ins *i = emit(fn);
        i->op = e->op == '+' ? IR_ADD : e->op == '-' ? IR_SUB : IR_MUL;
        i->a = a;
        i->b = b;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_CALL: {
        int args[MAX_PARAMS];
        for (int k = 0; k < e->nargs; k++)
            args[k] = gen_expr(fn, e->args[k]);
        struct ir_ins *i = emit(fn);
        i->op = IR_CALL;
        i->callee = e->callee;
        i->nargs = e->nargs;
        for (int k = 0; k < e->nargs; k++)
            i->args[k] = args[k];
        i->dst = new_temp(fn);
        return i->dst;
    }
    }
    return -1; /* unreachable; every kind returns above */
}

static void gen_func(struct ir_func *fn, struct func *f)
{
    fn->src = f;
    fn->nvregs = f->nvars; /* params + locals occupy [0, nvars) */

    for (struct stmt *s = f->body; s; s = s->next) {
        switch (s->kind) {
        case STMT_DECL:
            if (s->expr) {
                int v = gen_expr(fn, s->expr);
                struct ir_ins *i = emit(fn);
                i->op = IR_MOV;
                i->a = v;
                i->dst = s->var_index;
            }
            break;
        case STMT_RETURN: {
            int v = gen_expr(fn, s->expr);
            struct ir_ins *i = emit(fn);
            i->op = IR_RET;
            i->a = v;
            break;
        }
        }
    }
}

struct ir_unit *irgen(struct unit *u)
{
    struct ir_unit *iu = xcalloc(1, sizeof *iu);
    iu->src = u;

    for (struct func *f = u->funcs; f; f = f->next)
        iu->nfuncs++;
    iu->funcs = xcalloc((size_t)iu->nfuncs, sizeof *iu->funcs);

    int n = 0;
    for (struct func *f = u->funcs; f; f = f->next)
        gen_func(&iu->funcs[n++], f);
    return iu;
}
