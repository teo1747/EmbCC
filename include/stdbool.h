/* EmbCC's stdbool.h (C99 7.16). bool is the real _Bool type — sizeof is 1
 * and any nonzero value stored through it normalizes to 1. */
#ifndef _STDBOOL_H
#define _STDBOOL_H
#define bool _Bool
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif
