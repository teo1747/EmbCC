/* EmbCC's stdarg.h.
 *
 * va_list is `char *` — deliberately the SAME type newlib's headers pick
 * for a non-GNU compiler (__VALIST becomes `char*` because EmbCC does not
 * define __GNUC__), so a va_list flows into vfprintf and friends with no
 * type conflict. va_start makes that pointer point at a real SysV
 * __va_list_tag (gp_offset/fp_offset/overflow_arg_area/reg_save_area)
 * that EmbCC builds on the frame, which is exactly what the libc built by
 * gcc expects to read. va_end is a no-op.
 *
 * va_arg is NOT provided yet: EmbCC's own source only ever FORWARDS a
 * va_list (diag_fatal -> vfprintf), never walks one. Using va_arg without
 * it fails honestly (the identifier is undeclared) — a clean seam. */
#ifndef _STDARG_H
#define _STDARG_H
typedef char *va_list;
typedef char *__gnuc_va_list;
#define va_start(ap, last) __builtin_va_start((ap), (last))
#define va_end(ap)         __builtin_va_end((ap))
#endif
