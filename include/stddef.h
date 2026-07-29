/* EmbCC's stddef.h — compiler-owned (ARCHITECTURE §5: the predefined
 * macro table supplies the underlying types). */
#ifndef _STDDEF_H
#define _STDDEF_H
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef __WINT_TYPE__ wint_t;
#define NULL ((void *)0)
/* Fold to a size_t constant via the builtin, so offsetof works in an
 * integer-constant-expression (the `&((T*)0)->m` form does not). */
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
