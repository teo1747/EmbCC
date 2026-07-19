#ifndef EMBCC_SEMA_SEMA_H
#define EMBCC_SEMA_SEMA_H

#include "../parse/ast.h"

/* Resolves names and checks the unit; exits with a diagnostic on the
 * first error. On success every EXPR_VAR has a var_index, every
 * EXPR_CALL a callee, and every func its nvars. */
void sema_check(struct unit *u);

#endif
