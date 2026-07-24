/* Allocation that cannot fail quietly, and fatal diagnostics.
 *
 * Diagnostics carry file:line from M1 on (ROADMAP M2 raises the bar
 * further). A diagnostic here is always fatal: the M1 compiler stops at
 * the first error rather than guessing its way past it — recovery is
 * sema/M2 territory.
 */
#ifndef EMBCC_DRIVER_UTIL_H
#define EMBCC_DRIVER_UTIL_H

#include <stddef.h>

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
void *xcalloc(size_t n, size_t size);
char *xstrndup(const char *s, size_t n);

/* "embcc: FILE:LINE: error: ..." then exit(1). line 0 omits the line. */
void diag_fatal(const char *file, int line, const char *fmt, ...);

#endif
