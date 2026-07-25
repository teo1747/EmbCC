/* EmbCC's stdint.h — freestanding, for the fixed x86-64 SysV target EmbCC
 * emits (char 1, short 2, int 4, long 8, pointer 8). Exact-width, least,
 * fast, pointer, and max integer types plus their limits, so real vendored
 * C (fdlibm, and anything that wants int64_t/uint32_t) parses and compiles.
 * The widths are hard-wired to the ABI rather than derived from predefined
 * macros: EmbCC targets exactly one machine, so there is nothing to vary. */
#ifndef _STDINT_H
#define _STDINT_H

/* exact-width */
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

/* minimum-width (same as exact here) */
typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

/* fastest-minimum-width (the natural register width for the small ones) */
typedef int      int_fast8_t;
typedef int      int_fast16_t;
typedef int      int_fast32_t;
typedef long     int_fast64_t;
typedef unsigned int  uint_fast8_t;
typedef unsigned int  uint_fast16_t;
typedef unsigned int  uint_fast32_t;
typedef unsigned long uint_fast64_t;

/* pointer-sized and maximum-width */
typedef long          intptr_t;
typedef unsigned long uintptr_t;
typedef long          intmax_t;
typedef unsigned long uintmax_t;

/* limits */
#define INT8_MIN    (-128)
#define INT16_MIN   (-32768)
#define INT32_MIN   (-2147483647 - 1)
#define INT64_MIN   (-9223372036854775807L - 1)
#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define INT64_MAX   9223372036854775807L
#define UINT8_MAX   255
#define UINT16_MAX  65535
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615UL

#define INT_LEAST8_MIN   INT8_MIN
#define INT_LEAST16_MIN  INT16_MIN
#define INT_LEAST32_MIN  INT32_MIN
#define INT_LEAST64_MIN  INT64_MIN
#define INT_LEAST8_MAX   INT8_MAX
#define INT_LEAST16_MAX  INT16_MAX
#define INT_LEAST32_MAX  INT32_MAX
#define INT_LEAST64_MAX  INT64_MAX
#define UINT_LEAST8_MAX  UINT8_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INTPTR_MIN   INT64_MIN
#define INTPTR_MAX   INT64_MAX
#define UINTPTR_MAX  UINT64_MAX
#define INTMAX_MIN   INT64_MIN
#define INTMAX_MAX   INT64_MAX
#define UINTMAX_MAX  UINT64_MAX
#define PTRDIFF_MIN  INT64_MIN
#define PTRDIFF_MAX  INT64_MAX
#define SIZE_MAX     UINT64_MAX

/* constant-builder macros */
#define INT8_C(x)    (x)
#define INT16_C(x)   (x)
#define INT32_C(x)   (x)
#define INT64_C(x)   (x ## L)
#define UINT8_C(x)   (x)
#define UINT16_C(x)  (x)
#define UINT32_C(x)  (x ## U)
#define UINT64_C(x)  (x ## UL)
#define INTMAX_C(x)  (x ## L)
#define UINTMAX_C(x) (x ## UL)

#endif /* _STDINT_H */
