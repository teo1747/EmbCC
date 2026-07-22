/* The preprocessor (ARCHITECTURE §2, §5): a text -> text pass run
 * before the lexer. Output carries line markers (# LINE "FILE") that
 * the lexer consumes, so every diagnostic still lands on the line the
 * human wrote, not the line the expansion produced.
 *
 * Implemented: #include ("..." and <...> via -I paths), #define
 * (object- and function-like, # stringize, ## paste), #undef,
 * #if/#ifdef/#ifndef/#elif/#else/#endif with a constant-expression
 * evaluator (incl. defined()), #error, #pragma (ignored), __FILE__,
 * __LINE__, and the predefined x86_64-elf macro table from M0.
 */
#ifndef EMBCC_CPP_CPP_H
#define EMBCC_CPP_CPP_H

/* Preprocesses the file at `path` (its content in `src`), returning
 * malloc'd expanded text. incdirs/nincdirs are the -I search paths.
 * Exits with a diagnostic on any preprocessing error. */
char *cpp_process(const char *path, const char *src,
                  const char **incdirs, int nincdirs);

#endif
