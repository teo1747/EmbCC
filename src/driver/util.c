#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    return p;
}

/* strndup is POSIX, not C99, and the source must stay strict C99
 * (ARCHITECTURE §7) — so EmbCC carries its own. */
char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void diag_fatal(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    if (line > 0)
        fprintf(stderr, "embcc: %s:%d: error: ", file, line);
    else
        fprintf(stderr, "embcc: %s: error: ", file);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}
