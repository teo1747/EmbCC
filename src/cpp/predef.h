/* The predefined-macro table for the x86_64-elf LP64 target.
 *
 * ARCHITECTURE.md §5: real newlib headers select their fixed-width types
 * from the full GCC predefined-macro family and hard-#error when it is
 * absent (the break that cost TCC patch 0002). The table is generated from
 * `x86_64-elf-gcc -dM -E` by tools/gen-predef.sh — never hand-derived.
 *
 * Deliberately NOT in the table (THE RULE — claim only what is present):
 *   __GNUC__ family  — EmbCC is not gcc; claiming it would route real
 *                      headers onto gcc-extension paths EmbCC cannot honor.
 *   __STDC__ family  — describes the compiler, not the target; the
 *                      preprocessor will define these itself when there is
 *                      a preprocessor to do so (pre-M2 there is not).
 */
#ifndef EMBCC_CPP_PREDEF_H
#define EMBCC_CPP_PREDEF_H

struct predef_macro {
    /* For function-like macros the name carries its parameter list
     * verbatim, e.g. "__INT64_C(c)" — the M2 preprocessor splits it. */
    const char *name;
    const char *value; /* replacement text, verbatim from the reference gcc */
};

extern const struct predef_macro predef_macros[];
extern const int predef_macro_count;

#endif
