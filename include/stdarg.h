/* EmbCC's stdarg.h. The va_list TYPE exists so prototypes taking one
 * (vprintf and friends) parse; there is no va_start/va_arg/va_end yet,
 * so USING one fails honestly at compile time — EmbCC cannot define
 * variadic functions until the SysV register-save machinery exists. */
#ifndef _STDARG_H
#define _STDARG_H
typedef char *va_list;
typedef char *__gnuc_va_list;
#endif
