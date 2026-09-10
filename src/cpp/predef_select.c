/* Which predefined-macro table the selected target uses. Hand-written:
 * the tables themselves are generated, so the choice between them cannot
 * live in either file. */
#include "predef.h"

#include "../target/target.h"

const struct predef_macro *predef_table(int *count)
{
    if (target_get() == TARGET_AARCH64) {
        *count = predef_macro_count_aarch64;
        return predef_macros_aarch64;
    }
    *count = predef_macro_count_x86_64;
    return predef_macros_x86_64;
}
