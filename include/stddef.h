/* EmbCC's stddef.h — compiler-owned (ARCHITECTURE §5: the predefined
 * macro table supplies the underlying types). */
#ifndef _STDDEF_H
#define _STDDEF_H
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__ wchar_t;
typedef __WINT_TYPE__ wint_t;
#define NULL ((void *)0)
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#endif
