#ifndef EMBCC_SEMA_SEMA_H
#define EMBCC_SEMA_SEMA_H

#include "../parse/ast.h"

/* Resolves names and checks the unit; exits with a diagnostic on the
 * first error. On success every EXPR_VAR has a var_index, every
 * EXPR_CALL a callee, and every func its nvars. */
void sema_check(struct unit *u);

/* The statement list a switch dispatches over: its body, unwrapped when
 * it is the usual brace block. Shared with irgen so both agree on which
 * statements carry the case markers. */
struct stmt *switch_stmts(struct stmt *body);

#endif
