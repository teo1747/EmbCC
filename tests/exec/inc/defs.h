#ifndef DEFS_H
#define DEFS_H
#define ANSWER 42
#define DOUBLE(x) ((x) + (x))
#define STR(x) #x
#define GLUE(a, b) a ## b
#define XGLUE(a, b) GLUE(a, b)
#define FIRST_OF(a, ...) (a)
#if __SIZEOF_POINTER__ == 8
#define ARCH64 1
#else
#error "wrong architecture"
#endif
typedef struct Pair { int a; int b; } Pair;
#endif
