#include "target.h"

#include <string.h>

#include "../elf/elf.h"

static enum target_arch g_arch = TARGET_X86_64;

enum target_arch target_get(void) { return g_arch; }
void target_set(enum target_arch a) { g_arch = a; }

int target_from_triple(const char *triple, enum target_arch *out)
{
    static const struct { const char *name; enum target_arch arch; } tab[] = {
        { "x86_64-elf",       TARGET_X86_64 },
        { "x86_64",           TARGET_X86_64 },
        { "x86_64-none-elf",  TARGET_X86_64 },
        { "aarch64-elf",      TARGET_AARCH64 },
        { "aarch64",          TARGET_AARCH64 },
        { "arm64",            TARGET_AARCH64 },
        { "aarch64-none-elf", TARGET_AARCH64 },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(triple, tab[i].name) == 0) {
            *out = tab[i].arch;
            return 1;
        }
    return 0;
}

const char *target_triple(enum target_arch a)
{
    return a == TARGET_AARCH64 ? "aarch64-elf" : "x86_64-elf";
}

int target_elf_machine(enum target_arch a)
{
    return a == TARGET_AARCH64 ? EM_AARCH64 : EM_X86_64;
}

int target_reloc_type(enum target_arch a, enum reloc_kind k)
{
    if (a == TARGET_AARCH64) {
        switch (k) {
        /* CALL26, not JUMP26: the field is the same, but CALL26 is what a
         * `bl` carries, and a linker may only insert a veneer for the
         * range-exceeding case on the call form. */
        case RK_CALL:     return R_AARCH64_CALL26;
        case RK_ADR_HI21: return R_AARCH64_ADR_PREL_PG_HI21;
        case RK_ADD_LO12: return R_AARCH64_ADD_ABS_LO12_NC;
        case RK_ABS64:    return R_AARCH64_ABS64;
        default:          return -1;
        }
    }
    switch (k) {
    /* PLT32 rather than PC32 for calls: it lets the linker route through
     * a PLT entry when the callee turns out to be far away or interposed,
     * and degrades to PC32 when it does not. */
    case RK_CALL:     return R_X86_64_PLT32;
    case RK_PCREL32:  return R_X86_64_PC32;
    case RK_ABS64:    return R_X86_64_64;
    default:          return -1;
    }
}

long target_reloc_addend(enum target_arch a, enum reloc_kind k, long bias)
{
    if (a == TARGET_AARCH64)
        return bias;              /* aarch64 fields are relative to the
                                   * instruction, so no end-of-insn bias */
    switch (k) {
    case RK_CALL:
    case RK_PCREL32:
        return bias - 4;          /* x86-64 rel32 is measured from the END
                                   * of the instruction, four bytes on */
    default:
        return bias;
    }
}
