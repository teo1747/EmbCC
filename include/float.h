/* EmbCC's float.h — macro values from the reference gcc's table.
 * There is no floating-point TYPE support yet; these exist so headers
 * that include <float.h> parse. */
#ifndef _FLOAT_H
#define _FLOAT_H
#define FLT_MANT_DIG __FLT_MANT_DIG__
#define DBL_MANT_DIG __DBL_MANT_DIG__
#define FLT_MAX_EXP __FLT_MAX_EXP__
#define DBL_MAX_EXP __DBL_MAX_EXP__
#define FLT_RADIX 2
#endif
