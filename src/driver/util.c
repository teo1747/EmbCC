#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* isatty — auto colour when stderr is a terminal */

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

/* ---- source registry -------------------------------------------------------
 *
 * Diagnostics render the offending source line, so the text of every file a
 * diagnostic can name is registered here under the exact name the lexer reports
 * (the driver registers the main file; the preprocessor each #include). A
 * diagnostic with an unregistered file (a synthetic location) still prints its
 * message and location — just no source line. */
struct src_ent { const char *file; const char *text; };
static struct src_ent *g_srcs;
static int g_nsrc, g_capsrc;

void diag_register_source(const char *file, const char *text)
{
    for (int i = 0; i < g_nsrc; i++)
        if (!strcmp(g_srcs[i].file, file)) { g_srcs[i].text = text; return; }
    if (g_nsrc == g_capsrc) {
        g_capsrc = g_capsrc ? g_capsrc * 2 : 8;
        g_srcs = xrealloc(g_srcs, (size_t)g_capsrc * sizeof *g_srcs);
    }
    g_srcs[g_nsrc].file = file;
    g_srcs[g_nsrc].text = text;
    g_nsrc++;
}

/* Start of 1-based `line` in `file` (NUL/newline-terminated run), or NULL. */
static const char *src_line(const char *file, int line, int *len_out)
{
    const char *text = NULL;
    for (int i = 0; i < g_nsrc; i++)
        if (!strcmp(g_srcs[i].file, file)) { text = g_srcs[i].text; break; }
    if (!text || line < 1)
        return NULL;
    const char *p = text;
    for (int n = 1; n < line; n++) {
        p = strchr(p, '\n');
        if (!p) return NULL;
        p++;
    }
    const char *e = p;
    while (*e && *e != '\n') e++;
    *len_out = (int)(e - p);
    return p;
}

/* ---- colour ---- */
static const char *cc(const char *code)
{
    static int on = -1;
    if (on < 0)
        on = isatty(2) ? 1 : 0;
    return on ? code : "";
}

/* Render one diagnostic: the located, coloured heading, then the source line
 * with a caret under column `col` (a tab in the source stays a tab under the
 * caret, so it lines up in any tab width). col/line <= 0 drop that detail. */
static void diag_render(const char *file, int line, int col,
                        const char *sgr, const char *level,
                        const char *fmt, va_list ap)
{
    fprintf(stderr, "%s%s", cc("\033[1m"), file);
    if (line > 0) fprintf(stderr, ":%d", line);
    if (col > 0)  fprintf(stderr, ":%d", col);
    fprintf(stderr, ":%s %s%s:%s %s", cc("\033[0m"), cc(sgr), level,
            cc("\033[0m"), cc("\033[1m"));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "%s\n", cc("\033[0m"));

    int len;
    const char *ln = line > 0 ? src_line(file, line, &len) : NULL;
    if (!ln)
        return;
    fprintf(stderr, "  %.*s\n", len, ln);
    if (col > 0 && col <= len + 1) {   /* a caret only when the column is known */
        fputs("  ", stderr);
        for (int i = 1; i < col; i++)
            fputc(ln[i - 1] == '\t' ? '\t' : ' ', stderr);
        fprintf(stderr, "%s^", cc("\033[1;32m"));
        /* Underline the rest of the token under the caret — an identifier or
         * number spans its whole run, so `^~~~~~~` marks the offending name.
         * Inferred from the source, so no length has to be threaded here. */
        int c = col - 1;   /* 0-based index of the caret character */
        if (c < len && (isalnum((unsigned char)ln[c]) || ln[c] == '_'))
            for (int j = c + 1;
                 j < len && (isalnum((unsigned char)ln[j]) || ln[j] == '_'); j++)
                fputc('~', stderr);
        fprintf(stderr, "%s\n", cc("\033[0m"));
    }
}

/* ---- macro-expansion registry ----
 *
 * The parser runs on preprocessed text, so an error inside a macro expansion is
 * reported at the INVOCATION line — a line that often looks fine, because the
 * offending code is in the macro body. The preprocessor records each expansion
 * here (file, line -> macro name); an error at that line then adds a bare
 * "expanded from macro 'X'" note, explaining the caret. Line-granularity, so it
 * fires only when the macro's name actually appears on the error's line. */
struct exp_ent { const char *file; int line; const char *macro; };
static struct exp_ent *g_exps;
static int g_nexp, g_capexp;

void diag_register_expansion(const char *file, int line, const char *macro)
{
    if (g_nexp == g_capexp) {
        g_capexp = g_capexp ? g_capexp * 2 : 16;
        g_exps = xrealloc(g_exps, (size_t)g_capexp * sizeof *g_exps);
    }
    g_exps[g_nexp].file = file;
    g_exps[g_nexp].line = line;
    g_exps[g_nexp].macro = macro;
    g_nexp++;
}

/* 1-based column of `needle`'s first whole-identifier occurrence in hay[0..len),
 * or 0 if absent. */
static int word_col(const char *hay, int len, const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= len; i++) {
        if (memcmp(hay + i, needle, (size_t)nl) != 0)
            continue;
        int lok = i == 0 || !(isalnum((unsigned char)hay[i - 1]) || hay[i - 1] == '_');
        int rok = i + nl == len ||
                  !(isalnum((unsigned char)hay[i + nl]) || hay[i + nl] == '_');
        if (lok && rok)
            return i + 1;
    }
    return 0;
}

/* Note the macro whose expansion the error came from. An error inside an
 * expansion is reported at the macro's start column, so this fires only when
 * the error's column lands within the macro name on that source line — which
 * keeps an unrelated error that merely shares a line with some macro from being
 * misattributed. (col 0 = column unknown: fall back to name-on-line.) */
static void emit_macro_note(const char *file, int line, int col)
{
    for (int i = g_nexp - 1; i >= 0; i--) {
        if (g_exps[i].line != line || strcmp(g_exps[i].file, file) != 0)
            continue;
        int len;
        const char *ln = src_line(file, line, &len);
        if (!ln)
            continue;
        const char *m = g_exps[i].macro;
        int mc = word_col(ln, len, m);
        if (mc == 0)
            continue;
        if (col > 0 && !(col >= mc && col < mc + (int)strlen(m)))
            continue;
        fprintf(stderr, "embcc: %s%s:%d:%s %snote:%s expanded from macro '%s'\n",
                cc("\033[1m"), file, line, cc("\033[0m"),
                cc("\033[1;36m"), cc("\033[0m"), m);
        return;
    }
}

/* An error at a precise location: heading, source line, caret — then exit. */
void diag_at(const char *file, int line, int col, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embcc: ");
    va_start(ap, fmt);
    diag_render(file, line, col, "\033[1;31m", "error", fmt, ap);
    va_end(ap);
    emit_macro_note(file, line, col);
    exit(1);
}

/* An error that does NOT exit — for the primary of an error+note pair, so the
 * note (a previous declaration, an expansion site) prints under it before the
 * caller exits. */
void diag_error_at(const char *file, int line, int col, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embcc: ");
    va_start(ap, fmt);
    diag_render(file, line, col, "\033[1;31m", "error", fmt, ap);
    va_end(ap);
    emit_macro_note(file, line, col);
}

/* A non-fatal note, tied to an earlier location (a previous declaration, a
 * macro-expansion site). Does not exit — the caller's error already will. */
void diag_note_at(const char *file, int line, int col, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embcc: ");
    va_start(ap, fmt);
    diag_render(file, line, col, "\033[1;36m", "note", fmt, ap);
    va_end(ap);
}

void diag_fatal(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embcc: ");
    va_start(ap, fmt);
    diag_render(file, line, 0, "\033[1;31m", "error", fmt, ap);
    va_end(ap);
    emit_macro_note(file, line, 0);
    exit(1);
}
