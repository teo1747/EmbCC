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
    i->pred = B_ADD;
    i->label = -1;
    i->callee = 0;
    i->nargs = 0;
    return i;
}

static int new_temp(struct ir_func *fn) { return fn->nvregs++; }
static int new_label(struct ir_func *fn) { return fn->nlabels++; }

static void emit_label(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_LABEL;
    i->label = label;
}

static void emit_jmp(struct ir_func *fn, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_JMP;
    i->label = label;
}

static void emit_brz(struct ir_func *fn, int v, int label)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_BRZ;
    i->a = v;
    i->label = label;
}

static void emit_const(struct ir_func *fn, int dst, long imm)
{
    struct ir_ins *i = emit(fn);
    i->op = IR_CONST;
    i->imm = imm;
    i->dst = dst;
}

static int gen_expr(struct ir_func *fn, struct expr *e)
{
    switch (e->kind) {
    case EXPR_NUM: {
        int dst = new_temp(fn);
        emit_const(fn, dst, e->num);
        return dst;
    }
    case EXPR_VAR:
        return e->var_index;
    case EXPR_ASSIGN: {
        int v = gen_expr(fn, e->rhs);
        struct ir_ins *i = emit(fn);
        i->op = IR_MOV;
        i->a = v;
        i->dst = e->var_index;
        return e->var_index; /* the value of (a = b) is a */
    }
    case EXPR_NOT: {
        /* !x is x == 0 */
        int v = gen_expr(fn, e->rhs);
        int zero = new_temp(fn);
        emit_const(fn, zero, 0);
        struct ir_ins *i = emit(fn);
        i->op = IR_CMP;
        i->pred = B_EQ;
        i->a = v;
        i->b = zero;
        i->dst = new_temp(fn);
        return i->dst;
    }
    case EXPR_BINOP:
        switch (e->op) {
        case B_ADD:
        case B_SUB:
        case B_MUL: {
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            struct ir_ins *i = emit(fn);
            i->op = e->op == B_ADD ? IR_ADD :
                    e->op == B_SUB ? IR_SUB : IR_MUL;
            i->a = a;
            i->b = b;
            i->dst = new_temp(fn);
            return i->dst;
        }
        case B_EQ:
        case B_NE:
        case B_LT:
        case B_LE:
        case B_GT:
        case B_GE: {
            int a = gen_expr(fn, e->lhs);
            int b = gen_expr(fn, e->rhs);
            struct ir_ins *i = emit(fn);
            i->op = IR_CMP;
            i->pred = e->op;
            i->a = a;
            i->b = b;
            i->dst = new_temp(fn);
            return i->dst;
        }
        case B_LAND:
        case B_LOR: {
            /* Short-circuit, so the right side must not run when the
             * left decides — C semantics, and once side effects exist
             * (calls do) it is observable. */
            int dst = new_temp(fn);
            int l_short = new_label(fn);
            int l_end = new_label(fn);
            int a = gen_expr(fn, e->lhs);
            if (e->op == B_LAND) {
                emit_brz(fn, a, l_short);          /* 0 && _  -> 0 */
                int b = gen_expr(fn, e->rhs);
                int zero = new_temp(fn);
                emit_const(fn, zero, 0);
                struct ir_ins *i = emit(fn);       /* dst = (b != 0) */
                i->op = IR_CMP;
                i->pred = B_NE;
                i->a = b;
                i->b = zero;
                i->dst = dst;
                emit_jmp(fn, l_end);
                emit_label(fn, l_short);
                emit_const(fn, dst, 0);
            } else {
                int l_rhs = new_label(fn);
                emit_brz(fn, a, l_rhs);            /* 0 || b  -> test b */
                emit_label(fn, l_short);           /* nonzero -> 1 */
                emit_const(fn, dst, 1);
                emit_jmp(fn, l_end);
                emit_label(fn, l_rhs);
                int b = gen_expr(fn, e->rhs);
                int zero = new_temp(fn);
                emit_const(fn, zero, 0);
                struct ir_ins *i = emit(fn);       /* dst = (b != 0) */
                i->op = IR_CMP;
                i->pred = B_NE;
                i->a = b;
                i->b = zero;
                i->dst = dst;
            }
            emit_label(fn, l_end);
            return dst;
        }
        }
        return -1; /* unreachable: all binops handled above */
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

static void gen_stmt(struct ir_func *fn, struct stmt *s)
{
    for (; s; s = s->next) {
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
        case STMT_EXPR:
            gen_expr(fn, s->expr); /* value discarded */
            break;
        case STMT_RETURN: {
            int v = gen_expr(fn, s->expr);
            struct ir_ins *i = emit(fn);
            i->op = IR_RET;
            i->a = v;
            break;
        }
        case STMT_IF: {
            int l_else = new_label(fn);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, l_else);
            gen_stmt(fn, s->thn);
            if (s->els) {
                int l_end = new_label(fn);
                emit_jmp(fn, l_end);
                emit_label(fn, l_else);
                gen_stmt(fn, s->els);
                emit_label(fn, l_end);
            } else {
                emit_label(fn, l_else);
            }
            break;
        }
        case STMT_WHILE: {
            int l_cond = new_label(fn);
            int l_end = new_label(fn);
            emit_label(fn, l_cond);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, l_end);
            gen_stmt(fn, s->body);
            emit_jmp(fn, l_cond);
            emit_label(fn, l_end);
            break;
        }
        case STMT_FOR: {
            int l_cond = new_label(fn);
            int l_end = new_label(fn);
            if (s->init)
                gen_expr(fn, s->init);
            emit_label(fn, l_cond);
            int c = gen_expr(fn, s->cond);
            emit_brz(fn, c, l_end);
            gen_stmt(fn, s->body);
            if (s->step)
                gen_expr(fn, s->step);
            emit_jmp(fn, l_cond);
            emit_label(fn, l_end);
            break;
        }
        case STMT_BLOCK:
            gen_stmt(fn, s->body);
            break;
        }
    }
}

static void gen_func(struct ir_func *fn, struct func *f)
{
    fn->src = f;
    fn->nvregs = f->nvars; /* params + locals occupy [0, nvars) */
    gen_stmt(fn, f->body);
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
