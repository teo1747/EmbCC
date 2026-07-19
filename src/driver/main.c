/* embcc driver: argv, flags, orchestration (ARCHITECTURE.md §2).
 *
 * At M0 there is nothing to orchestrate — the honest surface is exactly:
 * report what this is, expose the two artifacts M0 does contain (the
 * predefined-macro table and the ELF writer skeleton) so tests can exercise
 * them, and refuse everything else loudly (THE RULE: a missing capability
 * fails, it is never faked).
 */
#include <stdio.h>
#include <string.h>

#include "../cpp/predef.h"
#include "../elf/write.h"

#define EMBCC_VERSION "0.0.0-m0"

static void print_version(void)
{
    /* Honest: names the milestone and what does not exist yet. */
    printf("EmbCC %s — C compiler for EmbLinkOS, target x86_64-elf\n",
           EMBCC_VERSION);
    printf("M0 scaffolding: no parser, no codegen; nothing compiles yet.\n");
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: embcc [--version] [--dump-predef]"
            " [--emit-empty-object FILE]\n");
}

static void dump_predef(void)
{
    for (int i = 0; i < predef_macro_count; i++)
        printf("#define %s %s\n",
               predef_macros[i].name, predef_macros[i].value);
}

/* An empty but genuine relocatable object: the smallest output readelf,
 * objdump and the cross ld all accept. Exists so the writer is testable
 * before there is a compiler to feed it (ROADMAP M0). */
static int emit_empty_object(const char *path)
{
    struct elfw *w = elfw_new();
    int text = elfw_add_section(w, ".text", SHT_PROGBITS,
                                SHF_ALLOC | SHF_EXECINSTR, NULL, 0, 16);
    elfw_add_symbol(w, "empty.c", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_FILE), SHN_ABS);
    elfw_add_symbol(w, "", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_SECTION), (Elf64_Half)text);
    int rc = elfw_write(w, path);
    elfw_free(w);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 1;
    }
    if (strcmp(argv[1], "--version") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(argv[1], "--dump-predef") == 0) {
        dump_predef();
        return 0;
    }
    if (strcmp(argv[1], "--emit-empty-object") == 0) {
        if (argc != 3) {
            fprintf(stderr, "embcc: --emit-empty-object needs a FILE\n");
            return 1;
        }
        return emit_empty_object(argv[2]);
    }

    size_t n = strlen(argv[1]);
    if (n > 2 && strcmp(argv[1] + n - 2, ".c") == 0) {
        fprintf(stderr,
                "embcc: error: cannot compile '%s': "
                "no compiler exists yet (pre-M1, see docs/ROADMAP.md)\n",
                argv[1]);
        return 1;
    }
    fprintf(stderr, "embcc: error: unknown argument '%s'\n", argv[1]);
    print_usage(stderr);
    return 1;
}
