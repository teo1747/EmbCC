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

/* Register a file's text so diagnostics naming it can print its source lines.
 * `text` must outlive every diagnostic (the whole compile). */
void diag_register_source(const char *file, const char *text);

/* "embcc: FILE:LINE: error: ..." then the source line and a caret, then
 * exit(1). line 0 omits the line; a registered source adds the line + caret. */
void diag_fatal(const char *file, int line, const char *fmt, ...);

/* As diag_fatal, but with a column for the caret. */
void diag_at(const char *file, int line, int col, const char *fmt, ...);

/* A non-fatal "note:" tied to a location — a previous declaration, a macro
 * expansion site. Does not exit. */
void diag_note_at(const char *file, int line, int col, const char *fmt, ...);

#endif
