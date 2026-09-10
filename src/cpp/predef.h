/* The predefined-macro tables, one per target.
 *
 * ARCHITECTURE.md §5: real newlib headers select their fixed-width types
 * from the full GCC predefined-macro family and hard-#error when it is
 * absent (the break that cost TCC patch 0002). Each table is generated
 * from `<triple>-gcc -dM -E` by tools/gen-predef.sh — never hand-derived.
 * That is also why the two tables come from different gcc versions: each
 * is taken from the compiler EmbLinkOS actually builds that architecture
 * with, and the aarch64 side is pinned to 16.2.0 (myos ARM64.md §2.8).
 *
 * Deliberately NOT in the tables (THE RULE — claim only what is present):
 *   __GNUC__ family  — EmbCC is not gcc; claiming it would route real
 *                      headers onto gcc-extension paths EmbCC cannot honor.
 *   __STDC__ family  — describes the compiler, not the target; the
 *                      preprocessor defines these itself.
 */
#ifndef EMBCC_CPP_PREDEF_H
#define EMBCC_CPP_PREDEF_H

struct predef_macro {
    /* For function-like macros the name carries its parameter list
     * verbatim, e.g. "__INT64_C(c)" — the M2 preprocessor splits it. */
    const char *name;
    const char *value; /* replacement text, verbatim from the reference gcc */
};

extern const struct predef_macro predef_macros_x86_64[];
extern const int predef_macro_count_x86_64;
extern const struct predef_macro predef_macros_aarch64[];
extern const int predef_macro_count_aarch64;

/* The table for the target selected by --target=. Every consumer goes
 * through this rather than naming a table, so adding a third machine
 * touches predef_select.c and nothing else. */
const struct predef_macro *predef_table(int *count);

#endif
