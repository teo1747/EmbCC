/* embcc driver: argv, flags, orchestration (ARCHITECTURE.md §2).
 *
 * M1 surface: -c compiles one file of the M1 subset to a relocatable
 * object; linking stays with the existing toolchain until the integrated
 * linker (M3). Everything the compiler cannot do fails loudly with a
 * diagnostic naming the milestone that brings it (THE RULE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../codegen/codegen.h"
#include "../cpp/predef.h"
#include "../elf/write.h"
#include "../ir/ir.h"
#include "../parse/parse.h"
#include "../sema/sema.h"
#include "util.h"

#define EMBCC_VERSION "0.2.0-m2.control-flow"

static void print_version(void)
{
    /* Honest: names what exists and what does not. */
    printf("EmbCC %s — C compiler for EmbLinkOS, target x86_64-elf\n",
           EMBCC_VERSION);
    printf("C subset: int functions, if/else, while, for, comparisons, "
           "&&/||/!, + - *, assignment, calls within one file; "
           "compile with -c.\n");
    printf("No preprocessor yet (M2), no linker yet (M3) — "
           "link objects with the existing toolchain.\n");
}

static void print_usage(FILE *out)
{
    fprintf(out,
            "usage: embcc -c FILE.c [-o FILE.o]\n"
            "       embcc --version | --dump-predef"
            " | --emit-empty-object FILE\n");
}

static void dump_predef(void)
{
    for (int i = 0; i < predef_macro_count; i++)
        printf("#define %s %s\n",
               predef_macros[i].name, predef_macros[i].value);
}

/* An empty but genuine relocatable object: the smallest output readelf,
 * objdump and the cross ld all accept. Kept from M0 so the writer stays
 * testable independently of the compiler. */
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

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        diag_fatal(path, 0, "cannot open file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n)
        diag_fatal(path, 0, "read error");
    buf[n] = 0;
    fclose(f);
    return buf;
}

static const char *default_output(const char *in)
{
    size_t n = strlen(in);
    char *out = xstrndup(in, n);
    out[n - 1] = 'o'; /* caller verified the .c suffix */
    return out;
}

static int compile(const char *in, const char *out)
{
    char *src = read_file(in);
    struct unit *u = parse_unit(in, src);
    sema_check(u);
    struct ir_unit *iu = irgen(u);

    struct code text = { 0, 0, 0 };
    codegen_unit(iu, &text);

    struct elfw *w = elfw_new();
    int text_ndx = elfw_add_section(w, ".text", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_EXECINSTR,
                                    text.p, (Elf64_Xword)text.len, 16);
    elfw_add_symbol(w, in, 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_FILE), SHN_ABS);
    elfw_add_symbol(w, "", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                    (Elf64_Half)text_ndx);
    /* Locals before globals — the writer enforces the gABI ordering. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (f->is_static)
            elfw_add_symbol(w, f->name, (Elf64_Addr)f->code_off,
                            (Elf64_Xword)f->code_len,
                            ELF64_ST_INFO(STB_LOCAL, STT_FUNC),
                            (Elf64_Half)text_ndx);
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->is_static)
            elfw_add_symbol(w, f->name, (Elf64_Addr)f->code_off,
                            (Elf64_Xword)f->code_len,
                            ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                            (Elf64_Half)text_ndx);

    int rc = elfw_write(w, out);
    elfw_free(w);
    return rc == 0 ? 0 : 1;
}

static int has_c_suffix(const char *s)
{
    size_t n = strlen(s);
    return n > 2 && strcmp(s + n - 2, ".c") == 0;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    int compile_mode = 0;

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

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            compile_mode = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 == argc) {
                fprintf(stderr, "embcc: -o needs a FILE\n");
                return 1;
            }
            output = argv[++i];
        } else if (has_c_suffix(argv[i])) {
            if (input) {
                fprintf(stderr, "embcc: error: more than one input file "
                                "(M1: one file at a time)\n");
                return 1;
            }
            input = argv[i];
        } else {
            fprintf(stderr, "embcc: error: unknown argument '%s'\n",
                    argv[i]);
            print_usage(stderr);
            return 1;
        }
    }

    if (!input) {
        fprintf(stderr, "embcc: error: no input file\n");
        return 1;
    }
    if (!compile_mode) {
        fprintf(stderr,
                "embcc: error: cannot link '%s': the integrated linker is "
                "M3 (see docs/ROADMAP.md) — compile with -c and link with "
                "the existing toolchain\n", input);
        return 1;
    }
    return compile(input, output ? output : default_output(input));
}
