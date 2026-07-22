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

#define EMBCC_VERSION "0.6.0-m2.structs"

static void print_version(void)
{
    /* Honest: names what exists and what does not. */
    printf("EmbCC %s — C compiler for EmbLinkOS, target x86_64-elf\n",
           EMBCC_VERSION);
    printf("C subset: the integer types, pointers, arrays, "
           "structs/unions/enums, typedef, string literals, globals, "
           "sizeof, casts, full control flow and operators, prototypes "
           "incl. variadic externals; compile with -c.\n");
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
    struct extcall *ext;
    struct strsite *strs;
    struct gsite *gs;
    int next, nstrs, ngs;
    codegen_unit(iu, &text, &ext, &next, &strs, &nstrs, &gs, &ngs);

    /* Lay out the defined globals: initialized -> .data, zero -> .bss,
     * each aligned to its (element) size. */
    int data_len = 0, bss_len = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined)
            continue;
        int align = ty_align(g->ty);
        g->in_bss = !g->has_init;
        int *len = g->in_bss ? &bss_len : &data_len;
        *len = (*len + align - 1) & ~(align - 1);
        g->off = *len;
        *len += ty_size(g->ty);
    }
    char *data = NULL;
    if (data_len) {
        data = xcalloc(1, (size_t)data_len);
        for (struct global *g = u->globals; g; g = g->next) {
            if (g->absorbed || !g->defined || g->in_bss)
                continue;
            unsigned long v = (unsigned long)g->init;
            for (int b = 0; b < ty_size(g->ty); b++)
                data[g->off + b] = (char)((v >> (8 * b)) & 0xff);
        }
    }

    /* .rodata: the string literals, at the offsets irgen assigned. */
    char *rodata = NULL;
    if (iu->rodata_len) {
        rodata = xmalloc((size_t)iu->rodata_len);
        for (int i = 0; i < iu->nstrs; i++)
            memcpy(rodata + iu->strs[i].off, iu->strs[i].bytes,
                   (size_t)iu->strs[i].len);
    }

    struct elfw *w = elfw_new();
    int text_ndx = elfw_add_section(w, ".text", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_EXECINSTR,
                                    text.p, (Elf64_Xword)text.len, 16);
    int rodata_ndx = 0;
    if (rodata)
        rodata_ndx = elfw_add_section(w, ".rodata", SHT_PROGBITS,
                                      SHF_ALLOC, rodata,
                                      (Elf64_Xword)iu->rodata_len, 1);
    int data_ndx = 0, bss_ndx = 0;
    if (data_len)
        data_ndx = elfw_add_section(w, ".data", SHT_PROGBITS,
                                    SHF_ALLOC | SHF_WRITE, data,
                                    (Elf64_Xword)data_len, 8);
    if (bss_len)
        bss_ndx = elfw_add_section(w, ".bss", SHT_NOBITS,
                                   SHF_ALLOC | SHF_WRITE, NULL,
                                   (Elf64_Xword)bss_len, 8);
    elfw_add_symbol(w, in, 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_FILE), SHN_ABS);
    elfw_add_symbol(w, "", 0, 0,
                    ELF64_ST_INFO(STB_LOCAL, STT_SECTION),
                    (Elf64_Half)text_ndx);
    int rodata_sym = 0;
    if (rodata)
        rodata_sym = elfw_add_symbol(w, "", 0, 0,
                                     ELF64_ST_INFO(STB_LOCAL,
                                                   STT_SECTION),
                                     (Elf64_Half)rodata_ndx);
    /* Locals before globals — the writer enforces the gABI ordering.
     * Only canonical, defined functions own code. */
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && f->is_static)
            elfw_add_symbol(w, f->name, (Elf64_Addr)f->code_off,
                            (Elf64_Xword)f->code_len,
                            ELF64_ST_INFO(STB_LOCAL, STT_FUNC),
                            (Elf64_Half)text_ndx);
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && g->is_static)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, (Elf64_Addr)g->off,
                (Elf64_Xword)ty_size(g->ty),
                ELF64_ST_INFO(STB_LOCAL, STT_OBJECT),
                (Elf64_Half)(g->in_bss ? bss_ndx : data_ndx));
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->has_defn && !f->is_static)
            elfw_add_symbol(w, f->name, (Elf64_Addr)f->code_off,
                            (Elf64_Xword)f->code_len,
                            ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),
                            (Elf64_Half)text_ndx);
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && !g->is_static)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, (Elf64_Addr)g->off,
                (Elf64_Xword)ty_size(g->ty),
                ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT),
                (Elf64_Half)(g->in_bss ? bss_ndx : data_ndx));
    /* extern-declared, used, never defined: the linker's problem */
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && !g->defined && g->used)
            g->sym_ndx = elfw_add_symbol(
                w, g->name, 0, 0,
                ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE), SHN_UNDEF);

    /* Every called external gets one UNDEF symbol, and every call site
     * a PLT32 relocation against it. addend -4: rel32 is relative to
     * the END of the call instruction, four bytes past r_offset. */
    for (int i = 0; i < next; i++) {
        struct func *callee = ext[i].callee;
        if (!callee->sym_ndx)
            callee->sym_ndx =
                elfw_add_symbol(w, callee->name, 0, 0,
                                ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE),
                                SHN_UNDEF);
        elfw_add_rela(w, text_ndx, (Elf64_Addr)ext[i].patch_off,
                      callee->sym_ndx, R_X86_64_PLT32, -4);
    }
    free(ext);

    /* String addresses: PC32 against the .rodata section symbol.
     * addend = target offset - 4, because rel32 is measured from the
     * end of the instruction, four bytes past r_offset. */
    for (int i = 0; i < nstrs; i++)
        elfw_add_rela(w, text_ndx, (Elf64_Addr)strs[i].patch_off,
                      rodata_sym, R_X86_64_PC32, strs[i].str_off - 4);
    free(strs);

    /* Global-variable addresses: PC32 against the global's own symbol
     * (defined or UNDEF alike — the linker fills in either way). */
    for (int i = 0; i < ngs; i++)
        elfw_add_rela(w, text_ndx, (Elf64_Addr)gs[i].patch_off,
                      gs[i].glob->sym_ndx, R_X86_64_PC32, -4);
    free(gs);

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
