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
 * va_arg walks the SysV __va_list_tag that va_start built: it reads the
 * next INTEGER-class argument (integers and pointers) from the register
 * save area or the overflow area and advances the tag. Floating and
 * struct-by-value varargs are a seam — refused, not miscompiled. */
#ifndef _STDARG_H
#define _STDARG_H
typedef char *va_list;
typedef char *__gnuc_va_list;
#define va_start(ap, last) __builtin_va_start((ap), (last))
#define va_arg(ap, type)   __builtin_va_arg((ap), type)
#define va_end(ap)         __builtin_va_end((ap))
#endif
