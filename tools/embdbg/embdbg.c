/* EmbDBG — the EmbLinkOS debugger, v0: the debug-info reader and symbolizer.
 *
 * WHY THIS FIRST, AND WHY IT NEEDS NO KERNEL. A debugger answers three
 * questions in ascending cost (EMBDBG_Requirements §1): "where am I?"
 * (address <-> file:line + function), "what can I see?" (the locals in
 * scope), "what is it?" (their types). The FIRST is pure debug-info reading —
 * no live process, no ptrace, no breakpoints — so it can be built and proven
 * today, on the host, against the DWARF EmbCC already emits (steps 1-2). The
 * live-control half (breakpoints, single-step, register/memory inspect) needs
 * the kernel debug contract — CAP_DEBUG, syscalls 69-75, exception routing —
 * which myos/docs/EMBDBG_Specification.md SPECIFIES but is reserved, not yet
 * built (a kernel design, D-007). This tool is the consumer that half will sit
 * on top of, and the consumer whose real needs the native .embdbg format is
 * meant to be derived from (D-010) — DWARF is the bridge until then.
 *
 * v0 reads .symtab (function ranges) and .debug_line (address -> file:line),
 * applying an object's .rela.debug_line so a relocatable .o works too, and
 * answers:
 *     embdbg FILE funcs                 list functions with address ranges
 *     embdbg FILE lines                 dump the decoded line table
 *     embdbg FILE symbolize ADDR...     addr -> FUNC+off  FILE:LINE  (a crash
 *                                       backtrace's addresses, symbolized)
 * It is a host tool for now (full C, gcc-built, like embread); the on-OS port
 * (in-subset + EmbLinkOS syscalls, reading the native .embdbg) is the seam.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/elf/elf.h"

/* DWARF line-program opcodes we decode. */
#define DW_LNS_copy             0x01
#define DW_LNS_advance_pc       0x02
#define DW_LNS_advance_line     0x03
#define DW_LNS_set_file         0x04
#define DW_LNS_set_column       0x05
#define DW_LNS_negate_stmt      0x06
#define DW_LNS_const_add_pc     0x08
#define DW_LNS_fixed_advance_pc 0x09
#define DW_LNE_end_sequence     0x01
#define DW_LNE_set_address      0x02

/* DIE tags / attributes / forms we read back (a subset — exactly what the
 * EmbCC emitter writes; unknown forms are skipped by their fixed size). */
#define DW_TAG_subprogram       0x2e
#define DW_TAG_formal_parameter 0x05
#define DW_TAG_variable         0x34
#define DW_TAG_base_type        0x24
#define DW_TAG_pointer_type     0x0f
#define DW_AT_name              0x03
#define DW_AT_byte_size         0x0b
#define DW_AT_low_pc            0x11
#define DW_AT_high_pc           0x12
#define DW_AT_type              0x49
#define DW_AT_location          0x02
#define DW_FORM_addr            0x01
#define DW_FORM_block1          0x0a
#define DW_FORM_data1           0x0b
#define DW_FORM_data2           0x05
#define DW_FORM_data4           0x06
#define DW_FORM_string          0x08
#define DW_FORM_ref4            0x13
#define DW_FORM_sec_offset      0x17
#define DW_FORM_exprloc         0x18
#define DW_OP_fbreg             0x91

static void die(const char *msg) { fprintf(stderr, "embdbg: %s\n", msg); exit(1); }

static unsigned char *slurp(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open file");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) die("read error");
    fclose(f);
    *len_out = n;
    return b;
}

/* --- LEB128 --- */
static unsigned long uleb(const unsigned char *p, int *i)
{
    unsigned long v = 0; int s = 0; unsigned char b;
    do { b = p[(*i)++]; v |= (unsigned long)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    return v;
}
static long sleb(const unsigned char *p, int *i)
{
    long v = 0; int s = 0; unsigned char b;
    do { b = p[(*i)++]; v |= (long)(b & 0x7f) << s; s += 7; } while (b & 0x80);
    if (s < 64 && (b & 0x40)) v |= -(1L << s);
    return v;
}
static unsigned long u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long u32(const unsigned char *p)
{ return (unsigned long)p[0] | (p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24); }
static unsigned long u64(const unsigned char *p)
{ return u32(p) | ((unsigned long)u32(p + 4) << 32); }

/* --- the loaded image --- */
struct sec { const char *name; unsigned char *data; unsigned long size, off; unsigned type, link, info, entsize; };
struct func { const char *name; unsigned long addr, size; };
struct row  { unsigned long addr; int file, line; int end; };

/* .debug_info DIEs, decoded to what an inspector needs. */
struct dtype { unsigned long off; int is_ptr; const char *name; int size; unsigned long pointee; };
struct dvar  { const char *name; int is_param; unsigned long type_off; long fbreg; int has_loc; };
struct dfunc { const char *name; unsigned long lo, hi; struct dvar *vars; int nvars; };

struct img {
    unsigned char *b; long len;
    struct sec *sec; int nsec;
    struct func *fn; int nfn;
    struct row *rows; int nrows;
    char **files; int nfiles;   /* file_names[1..], index 1-based in DWARF */
    struct dtype *types; int ntypes;
    struct dfunc *dfn; int ndfn;
};

static struct sec *find_sec(struct img *m, const char *name)
{
    for (int i = 0; i < m->nsec; i++)
        if (strcmp(m->sec[i].name, name) == 0) return &m->sec[i];
    return NULL;
}

static void load_sections(struct img *m)
{
    Elf64_Ehdr *e = (Elf64_Ehdr *)m->b;
    if (m->len < (long)sizeof *e || memcmp(e->e_ident, "\177ELF", 4) != 0)
        die("not an ELF file");
    Elf64_Shdr *sh = (Elf64_Shdr *)(m->b + e->e_shoff);
    int n = e->e_shnum;
    const char *shstr = (const char *)(m->b + sh[e->e_shstrndx].sh_offset);
    m->nsec = n;
    m->sec = calloc((size_t)n, sizeof *m->sec);
    for (int i = 0; i < n; i++) {
        m->sec[i].name = shstr + sh[i].sh_name;
        m->sec[i].data = m->b + sh[i].sh_offset;
        m->sec[i].size = sh[i].sh_size;
        m->sec[i].off = sh[i].sh_offset;
        m->sec[i].type = sh[i].sh_type;
        m->sec[i].link = sh[i].sh_link;
        m->sec[i].info = sh[i].sh_info;
        m->sec[i].entsize = sh[i].sh_entsize;
    }
}

static void load_funcs(struct img *m)
{
    struct sec *st = find_sec(m, ".symtab");
    if (!st) return;
    struct sec *strt = &m->sec[st->link];
    Elf64_Sym *sym = (Elf64_Sym *)st->data;
    int n = (int)(st->size / sizeof *sym);
    m->fn = calloc((size_t)n, sizeof *m->fn);
    for (int i = 0; i < n; i++) {
        if (ELF64_ST_TYPE(sym[i].st_info) != STT_FUNC || sym[i].st_size == 0)
            continue;
        m->fn[m->nfn].name = (const char *)strt->data + sym[i].st_name;
        m->fn[m->nfn].addr = sym[i].st_value;
        m->fn[m->nfn].size = sym[i].st_size;
        m->nfn++;
    }
}

/* Relocations against a debug section: a relocatable .o writes an address
 * field as 0 and carries the real .text offset in the addend. Map a field's
 * section-offset -> addend so the reader recovers the address. Used for
 * .debug_line's set_address and .debug_info's low_pc/high_pc. */
static unsigned long reloc_lookup(struct img *m, const char *rela, unsigned long off, int *found)
{
    struct sec *r = find_sec(m, rela);
    *found = 0;
    if (!r) return 0;
    Elf64_Rela *rel = (Elf64_Rela *)r->data;
    int n = (int)(r->size / sizeof *rel);
    for (int i = 0; i < n; i++)
        if (rel[i].r_offset == off) { *found = 1; return (unsigned long)rel[i].r_addend; }
    return 0;
}

static void add_row(struct img *m, unsigned long addr, int file, int line, int end)
{
    m->rows = realloc(m->rows, (size_t)(m->nrows + 1) * sizeof *m->rows);
    m->rows[m->nrows].addr = addr;
    m->rows[m->nrows].file = file;
    m->rows[m->nrows].line = line;
    m->rows[m->nrows].end = end;
    m->nrows++;
}

static void decode_lines(struct img *m)
{
    struct sec *ls = find_sec(m, ".debug_line");
    if (!ls) return;
    const unsigned char *p = ls->data;
    int i = 0, n = (int)ls->size;

    while (i < n) {
        int unit_start = i;
        unsigned long ulen = u32(p + i); i += 4;
        int unit_end = unit_start + 4 + (int)ulen;
        unsigned ver = (unsigned)u16(p + i); i += 2;
        unsigned long hlen = u32(p + i); i += 4;
        int prog = i + (int)hlen;
        unsigned min_inst = p[i++];
        if (ver >= 4) i++;                 /* maximum_operations_per_instruction */
        i++;                               /* default_is_stmt */
        int line_base = (signed char)p[i++];
        unsigned line_range = p[i++];
        unsigned opcode_base = p[i++];
        const unsigned char *std_len = p + i;
        i += (int)opcode_base - 1;
        while (p[i]) {                     /* include_directories */
            while (p[i]) i++;
            i++;
        }
        i++;
        m->nfiles = 1; m->files = calloc(1, sizeof(char *));  /* index 0 unused */
        while (p[i]) {
            const char *name = (const char *)(p + i);
            while (p[i]) i++;
            i++;
            (void)uleb(p, &i); (void)uleb(p, &i); (void)uleb(p, &i); /* dir,mtime,size */
            m->files = realloc(m->files, (size_t)(m->nfiles + 1) * sizeof(char *));
            m->files[m->nfiles++] = (char *)name;
        }
        i = prog;

        unsigned long addr = 0; int file = 1, line = 1;
        while (i < unit_end) {
            unsigned op = p[i++];
            if (op == 0) {                 /* extended */
                int len = (int)uleb(p, &i);
                int nexti = i + len;
                unsigned sub = p[i++];
                if (sub == DW_LNE_set_address) {
                    int found;
                    unsigned long a = reloc_lookup(m, ".rela.debug_line", (unsigned long)i, &found);
                    addr = found ? a : u64(p + i);
                } else if (sub == DW_LNE_end_sequence) {
                    add_row(m, addr, file, line, 1);
                    addr = 0; file = 1; line = 1;
                }
                i = nexti;
            } else if (op < opcode_base) {
                switch (op) {
                case DW_LNS_copy:            add_row(m, addr, file, line, 0); break;
                case DW_LNS_advance_pc:      addr += uleb(p, &i) * min_inst; break;
                case DW_LNS_advance_line:    line += (int)sleb(p, &i); break;
                case DW_LNS_set_file:        file = (int)uleb(p, &i); break;
                case DW_LNS_set_column:      (void)uleb(p, &i); break;
                case DW_LNS_negate_stmt:     break;
                case DW_LNS_const_add_pc:    addr += ((255 - opcode_base) / line_range) * min_inst; break;
                case DW_LNS_fixed_advance_pc: addr += u16(p + i); i += 2; break;
                default:                     /* skip unknown, per std_len */
                    for (int a = 0; a < std_len[op - 1]; a++) (void)uleb(p, &i);
                }
            } else {                         /* special opcode */
                unsigned adj = op - opcode_base;
                addr += (adj / line_range) * min_inst;
                line += line_base + (int)(adj % line_range);
                add_row(m, addr, file, line, 0);
            }
        }
        i = unit_end;
    }
}

/* --- .debug_info + .debug_abbrev: functions with their params/locals and the
 * scalar types EmbCC describes. Scoped to the forms the emitter writes. --- */
struct abbrev { int tag, children; int at[24], form[24]; int n; };

static void decode_info(struct img *m)
{
    struct sec *is = find_sec(m, ".debug_info");
    struct sec *as = find_sec(m, ".debug_abbrev");
    if (!is || !as) return;

    struct abbrev ab[64];
    memset(ab, 0, sizeof ab);
    { const unsigned char *p = as->data; int i = 0, n = (int)as->size;
      while (i < n) {
          int code = (int)uleb(p, &i);
          if (code == 0) break;            /* end of this (single) CU's table */
          if (code >= 64) return;
          ab[code].tag = (int)uleb(p, &i);
          ab[code].children = p[i++];
          for (;;) {
              int at = (int)uleb(p, &i), form = (int)uleb(p, &i);
              if (at == 0 && form == 0) break;
              if (ab[code].n < 24) {
                  ab[code].at[ab[code].n] = at;
                  ab[code].form[ab[code].n] = form;
                  ab[code].n++;
              }
          }
      }
    }

    const unsigned char *p = is->data; int i = 0, n = (int)is->size;
    i += 4;                                /* unit_length */
    i += 2;                                /* version */
    i += 4;                                /* debug_abbrev_offset */
    i += 1;                                /* address_size */
    struct dfunc *cur = NULL;

    while (i < n) {
        int die_off = i;                   /* ref4 targets this (CU starts at 0) */
        int code = (int)uleb(p, &i);
        if (code == 0) continue;           /* end-of-children marker */
        if (code >= 64 || ab[code].tag == 0) break;
        struct abbrev *a = &ab[code];

        const char *name = NULL;
        unsigned long low = 0, high = 0, tref = 0;
        long fb = 0; int hasloc = 0, size = 0;
        for (int k = 0; k < a->n; k++) {
            int at = a->at[k], form = a->form[k];
            unsigned long secoff = (unsigned long)i;
            switch (form) {
            case DW_FORM_string: {
                const char *s = (const char *)(p + i);
                while (p[i]) i++;
                i++;
                if (at == DW_AT_name) name = s;
                break; }
            case DW_FORM_data1: {
                int v = p[i++];
                if (at == DW_AT_byte_size) size = v;
                break; }
            case DW_FORM_data2:                    i += 2; break;
            case DW_FORM_data4:
            case DW_FORM_sec_offset:               i += 4; break;
            case DW_FORM_ref4: {
                unsigned long v = u32(p + i); i += 4;
                if (at == DW_AT_type) tref = v;
                break; }
            case DW_FORM_addr: {
                int found;
                unsigned long v = reloc_lookup(m, ".rela.debug_info", secoff, &found);
                if (!found) v = u64(p + i);
                i += 8;
                if (at == DW_AT_low_pc) low = v;
                else if (at == DW_AT_high_pc) high = v;
                break; }
            case DW_FORM_exprloc: {
                int len = (int)uleb(p, &i), st = i;
                if (at == DW_AT_location && len >= 1 && p[st] == DW_OP_fbreg) {
                    int j = st + 1; fb = sleb(p, &j); hasloc = 1;
                }
                i = st + len;
                break; }
            case DW_FORM_block1: { int len = p[i++]; i += len; break; }
            default: i = n; break;         /* unknown form: cannot size safely */
            }
        }

        if (a->tag == DW_TAG_base_type || a->tag == DW_TAG_pointer_type) {
            m->types = realloc(m->types, (size_t)(m->ntypes + 1) * sizeof *m->types);
            struct dtype *t = &m->types[m->ntypes++];
            t->off = (unsigned long)die_off;
            t->is_ptr = (a->tag == DW_TAG_pointer_type);
            t->name = name;
            t->size = t->is_ptr ? 8 : size;
            t->pointee = tref;
        } else if (a->tag == DW_TAG_subprogram) {
            m->dfn = realloc(m->dfn, (size_t)(m->ndfn + 1) * sizeof *m->dfn);
            cur = &m->dfn[m->ndfn++];
            cur->name = name; cur->lo = low; cur->hi = high;
            cur->vars = NULL; cur->nvars = 0;
        } else if ((a->tag == DW_TAG_formal_parameter || a->tag == DW_TAG_variable)
                   && cur && hasloc) {
            cur->vars = realloc(cur->vars, (size_t)(cur->nvars + 1) * sizeof *cur->vars);
            struct dvar *v = &cur->vars[cur->nvars++];
            v->name = name;
            v->is_param = (a->tag == DW_TAG_formal_parameter);
            v->type_off = tref;
            v->fbreg = fb;
            v->has_loc = hasloc;
        }
    }
}

/* A type's spelling, following a pointer one level to its pointee's name. */
static const char *type_name(struct img *m, unsigned long off)
{
    static char buf[128];
    if (off == 0) return "?";
    for (int i = 0; i < m->ntypes; i++) {
        if (m->types[i].off != off) continue;
        struct dtype *t = &m->types[i];
        if (!t->is_ptr) return t->name ? t->name : "?";
        const char *pn = t->pointee ? type_name(m, t->pointee) : "void";
        snprintf(buf, sizeof buf, "%s *", pn);
        return buf;
    }
    return "?";
}

static const struct dfunc *dfunc_at(struct img *m, unsigned long addr)
{
    for (int i = 0; i < m->ndfn; i++)
        if (addr >= m->dfn[i].lo && addr < m->dfn[i].hi) return &m->dfn[i];
    return NULL;
}

static const struct func *func_at(struct img *m, unsigned long addr)
{
    for (int i = 0; i < m->nfn; i++)
        if (addr >= m->fn[i].addr && addr < m->fn[i].addr + m->fn[i].size)
            return &m->fn[i];
    return NULL;
}

/* The last row at or before addr within a sequence (rows are in address order
 * per sequence; end-markers bound a sequence and never match). */
static const struct row *line_at(struct img *m, unsigned long addr)
{
    const struct row *best = NULL;
    for (int i = 0; i < m->nrows; i++) {
        if (m->rows[i].end) { continue; }
        if (m->rows[i].addr <= addr &&
            (i + 1 >= m->nrows || addr < m->rows[i + 1].addr))
            best = &m->rows[i];
    }
    return best;
}

static const char *file_name(struct img *m, int idx)
{
    if (idx >= 1 && idx < m->nfiles) return m->files[idx];
    return "?";
}

static void cmd_funcs(struct img *m)
{
    printf("%-24s %-10s %s\n", "FUNCTION", "ADDR", "SIZE");
    for (int i = 0; i < m->nfn; i++)
        printf("%-24s 0x%-8lx %lu\n", m->fn[i].name, m->fn[i].addr, m->fn[i].size);
}

static void cmd_lines(struct img *m)
{
    printf("%-12s %-20s %s\n", "ADDRESS", "FILE", "LINE");
    for (int i = 0; i < m->nrows; i++) {
        if (m->rows[i].end) { printf("0x%-10lx %-20s (end)\n", m->rows[i].addr, ""); continue; }
        printf("0x%-10lx %-20s %d\n", m->rows[i].addr,
               file_name(m, m->rows[i].file), m->rows[i].line);
    }
}

static void print_frame(struct img *m, unsigned long addr)
{
    const struct func *f = func_at(m, addr);
    const struct row *r = line_at(m, addr);
    if (f) printf("%s+0x%lx", f->name, addr - f->addr);
    else   printf("??");
    if (r) printf("  %s:%d", file_name(m, r->file), r->line);
    else   printf("  (no line info)");
}

static void cmd_symbolize(struct img *m, int argc, char **argv)
{
    for (int a = 0; a < argc; a++) {
        unsigned long addr = strtoul(argv[a], NULL, 0);
        printf("0x%lx  ", addr);
        print_frame(m, addr);
        printf("\n");
    }
}

/* A symbolized backtrace: the caller chain a kernel fault handler collects by
 * walking the rbp links is just a list of return addresses — name each. */
static void cmd_backtrace(struct img *m, int argc, char **argv)
{
    for (int a = 0; a < argc; a++) {
        unsigned long addr = strtoul(argv[a], NULL, 0);
        printf("#%-2d 0x%lx  ", a, addr);
        print_frame(m, addr);
        printf("\n");
    }
}

static void list_vars(struct img *m, const struct dfunc *d)
{
    for (int i = 0; i < d->nvars; i++) {
        struct dvar *v = &d->vars[i];
        printf("    %-5s %-14s %-8s @ rbp%+ld\n",
               v->is_param ? "param" : "local",
               type_name(m, v->type_off), v->name, v->fbreg);
    }
}

/* "where am I + what can I see": symbolize the address, then the params and
 * locals in scope at that function, each with its type and frame slot. */
static void cmd_where(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("where needs an address");
    unsigned long addr = strtoul(argv[0], NULL, 0);
    printf("0x%lx  ", addr);
    print_frame(m, addr);
    printf("\n");
    const struct dfunc *d = dfunc_at(m, addr);
    if (!d) { printf("    (no scope info here)\n"); return; }
    printf("  in %s — %d variable(s) in scope:\n", d->name, d->nvars);
    list_vars(m, d);
}

static void cmd_info(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("info needs a function name");
    for (int i = 0; i < m->ndfn; i++)
        if (m->dfn[i].name && strcmp(m->dfn[i].name, argv[0]) == 0) {
            struct dfunc *d = &m->dfn[i];
            printf("%s  [0x%lx, 0x%lx)  %d variable(s):\n",
                   d->name, d->lo, d->hi, d->nvars);
            list_vars(m, d);
            return;
        }
    printf("embdbg: no function '%s' with debug info\n", argv[0]);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "EmbDBG v0 — EmbLinkOS debug-info reader\n"
            "usage: embdbg FILE funcs               list functions + ranges\n"
            "       embdbg FILE lines               the address->file:line table\n"
            "       embdbg FILE symbolize ADDR...   addr -> func+off  file:line\n"
            "       embdbg FILE backtrace ADDR...   symbolize a caller chain\n"
            "       embdbg FILE where ADDR          addr + the locals in scope\n"
            "       embdbg FILE info FUNC           a function's params/locals\n");
        return 1;
    }
    struct img m; memset(&m, 0, sizeof m);
    m.b = slurp(argv[1], &m.len);
    load_sections(&m);
    load_funcs(&m);
    decode_lines(&m);
    decode_info(&m);

    const char *cmd = argv[2];
    if (strcmp(cmd, "funcs") == 0)          cmd_funcs(&m);
    else if (strcmp(cmd, "lines") == 0)     cmd_lines(&m);
    else if (strcmp(cmd, "symbolize") == 0) cmd_symbolize(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "backtrace") == 0) cmd_backtrace(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "where") == 0)     cmd_where(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "info") == 0)      cmd_info(&m, argc - 3, argv + 3);
    else { fprintf(stderr, "embdbg: unknown command '%s'\n", cmd); return 1; }
    return 0;
}
