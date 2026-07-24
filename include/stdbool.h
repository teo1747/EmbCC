/* EmbCC's stdbool.h. No _Bool type yet: bool is int, which changes
 * sizeof(bool) from 1 to 4 — code depending on that will misbehave,
 * so this divergence is documented here and only here. */
#ifndef _STDBOOL_H
#define _STDBOOL_H
#define bool int
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
