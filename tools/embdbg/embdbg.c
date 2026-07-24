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
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

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
#define DW_AT_encoding          0x3e
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

/* Added to every code address the DWARF readers decode. 0 for normal tool use
 * (a .o yields .text-relative addresses, a linked image absolute ones). EmbLD
 * sets it to a debug object's FINAL .text vaddr, so a relocatable object's
 * .text-relative addresses come out absolute — the link-time producer that
 * gives .embdbg the absolute vaddrs the spec wants (§2 "producer finding"). */
static long g_addr_bias = 0;

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
struct dtype { unsigned long off; int is_ptr; const char *name; int size; int enc; unsigned long pointee; };
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
        m->fn[m->nfn].addr = sym[i].st_value + (unsigned long)g_addr_bias;
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
                    addr = found ? (unsigned long)((long)a + g_addr_bias) : u64(p + i);
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
        long fb = 0; int hasloc = 0, size = 0, enc = 0;
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
                else if (at == DW_AT_encoding) enc = v;
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
                if (found) v = (unsigned long)((long)v + g_addr_bias);
                else v = u64(p + i);
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
            t->enc = enc;
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

static void print_source(const char *path, int line, int ctx);

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
    const struct row *r = line_at(m, addr);
    if (r) print_source(file_name(m, r->file), r->line, 2);
    const struct dfunc *d = dfunc_at(m, addr);
    if (!d) { printf("    (no scope info here)\n"); return; }
    printf("  in %s — %d variable(s) in scope:\n", d->name, d->nvars);
    list_vars(m, d);
}

/* Print a window of source around `line`, the current line marked. The path
 * is DW_AT_name as EmbCC recorded it (the path given on its command line);
 * open it relative to the cwd, and say so plainly if it is not reachable. */
static void print_source(const char *path, int line, int ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("    (source '%s' not reachable from here)\n", path); return; }
    char buf[1024];
    int ln = 0;
    while (fgets(buf, sizeof buf, f)) {
        ln++;
        if (ln > line + ctx) break;
        if (ln >= line - ctx) {
            buf[strcspn(buf, "\n")] = 0;
            printf("  %s %4d | %s\n", ln == line ? "->" : "  ", ln, buf);
        }
    }
    fclose(f);
}

/* Source-context view: symbolize the address, then show the source lines
 * around it with the current line marked — the modern "you are here". */
static void cmd_list(struct img *m, int argc, char **argv)
{
    if (argc < 1) die("list needs an address");
    unsigned long addr = strtoul(argv[0], NULL, 0);
    printf("0x%lx  ", addr);
    print_frame(m, addr);
    printf("\n");
    const struct row *r = line_at(m, addr);
    if (r) print_source(file_name(m, r->file), r->line, 2);
    else   printf("    (no line info)\n");
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

/* ===================================================================== *
 * Native .embdbg — myos/docs/EMBDBG_Specification.md v1.
 *
 * The owned debug format (D-010: DWARF is the bridge, .embdbg the destination).
 * EmbDBG is the consumer the spec's byte layout was derived to serve, so the
 * honest proof is a round trip: this tool WRITES .embdbg from the DWARF it
 * parsed, then READS it back into the same model, and every command produces
 * identical output either way. Byte-exact, little-endian, deterministic; the
 * header carries a SHA-256 build_id and CRC32C checksums exactly as the spec
 * (and EMBX) require. Addresses are whatever the input holds — absolute for a
 * linked image, .text-relative for a .o; the link-time producer yields the
 * absolute form the spec's kernel symbolizer wants.
 * ===================================================================== */

static const unsigned char EMBDBG_MAGIC[8] = { 0x7F,0x45,0x4D,0x44,0x42,0x47,0x0A,0x1A };
#define DBG_KIND_STRTAB 1
#define DBG_KIND_FILES  2
#define DBG_KIND_LINE   3
#define DBG_KIND_FUNCS  4
#define DBG_KIND_FRAME  5
#define DBG_KIND_VARS   6
#define DBG_KIND_TYPES  7
#define LN_STMT         0x1000
#define LOC_FBREG       1
#define VAR_PARAM       0x1

/* CRC32C (Castagnoli) — the header/section integrity check the spec mandates. */
static unsigned long crc32c(const unsigned char *p, long n)
{
    unsigned long crc = 0xFFFFFFFFUL;
    for (long i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78UL & (unsigned long)(-(long)(crc & 1)));
        crc &= 0xFFFFFFFFUL;
    }
    return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

/* SHA-256 (FIPS 180-4) — the 32-byte build_id that binds .embdbg to its image. */
struct sha { unsigned int h[8]; unsigned long long total; unsigned char buf[64]; int n; };
static unsigned int rotr(unsigned int x, int c) { return (x >> c) | (x << (32 - c)); }
static void sha_block(struct sha *s, const unsigned char *p)
{
    static const unsigned int K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        unsigned int s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        unsigned int s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    unsigned int a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],hh=s->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned int S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = hh + S1 + ch + K[i] + w[i];
        unsigned int S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=hh;
}
static void sha256(const unsigned char *data, long n, unsigned char out[32])
{
    struct sha s;
    s.h[0]=0x6a09e667; s.h[1]=0xbb67ae85; s.h[2]=0x3c6ef372; s.h[3]=0xa54ff53a;
    s.h[4]=0x510e527f; s.h[5]=0x9b05688c; s.h[6]=0x1f83d9ab; s.h[7]=0x5be0cd19;
    long i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, data + i);
    unsigned char tail[128]; int t = (int)(n - i);
    memcpy(tail, data + i, (size_t)t);
    tail[t++] = 0x80;
    int pad = (t <= 56) ? 56 - t : 120 - t;
    memset(tail + t, 0, (size_t)pad); t += pad;
    unsigned long long bits = (unsigned long long)n * 8;
    for (int k = 0; k < 8; k++) tail[t + k] = (unsigned char)(bits >> (56 - 8*k));
    t += 8;
    for (int j = 0; j < t; j += 64) sha_block(&s, tail + j);
    for (int k = 0; k < 8; k++) {
        out[k*4]   = (unsigned char)(s.h[k] >> 24);
        out[k*4+1] = (unsigned char)(s.h[k] >> 16);
        out[k*4+2] = (unsigned char)(s.h[k] >> 8);
        out[k*4+3] = (unsigned char)(s.h[k]);
    }
}

/* --- little-endian output buffer --- */
struct ob { unsigned char *p; long n, cap; };
static void ob_need(struct ob *b, long k) {
    if (b->n + k > b->cap) {
        b->cap = b->cap ? b->cap*2 : 256;
        while (b->n + k > b->cap) b->cap *= 2;
        b->p = realloc(b->p, (size_t)b->cap);
    }
}
static void ob_u8(struct ob *b, unsigned v){ ob_need(b,1); b->p[b->n++]=(unsigned char)v; }
static void ob_u16(struct ob *b, unsigned v){ ob_u8(b,v); ob_u8(b,v>>8); }
static void ob_u32(struct ob *b, unsigned long v){ ob_u16(b,(unsigned)v); ob_u16(b,(unsigned)(v>>16)); }
static void ob_u64(struct ob *b, unsigned long v){ ob_u32(b,v & 0xffffffffUL); ob_u32(b,(v>>32)&0xffffffffUL); }
static void ob_bytes(struct ob *b, const void *p, long k){ ob_need(b,k); memcpy(b->p+b->n,p,(size_t)k); b->n+=k; }

/* STRTAB builder with dedup (deterministic: first-referenced order). */
struct strtab { struct ob b; };
static unsigned long str_intern(struct strtab *s, const char *str)
{
    if (!str) str = "";
    if (s->b.n == 0) ob_u8(&s->b, 0);          /* offset 0 = "" */
    for (long i = 0; i < s->b.n; ) {
        const char *e = (const char *)(s->b.p + i);
        if (strcmp(e, str) == 0) return (unsigned long)i;
        i += (long)strlen(e) + 1;
    }
    unsigned long off = (unsigned long)s->b.n;
    ob_bytes(&s->b, str, (long)strlen(str) + 1);
    return off;
}

/* Flatten a DWARF-parsed type into the TYPES blob, returning its byte offset
 * (0 = void). Pointee first, so a T_POINTER references an existing entry. */
static unsigned long types_emit(struct img *m, struct ob *tb, struct strtab *st,
                                unsigned long *map_from, unsigned long *map_to,
                                int *nmap, unsigned long dwoff)
{
    if (dwoff == 0) return 0;
    for (int i = 0; i < *nmap; i++) if (map_from[i] == dwoff) return map_to[i];
    struct dtype *t = NULL;
    for (int i = 0; i < m->ntypes; i++) if (m->types[i].off == dwoff) { t = &m->types[i]; break; }
    if (!t) return 0;
    unsigned long off;
    if (t->is_ptr) {
        unsigned long tgt = types_emit(m, tb, st, map_from, map_to, nmap, t->pointee);
        if (tb->n == 0) ob_u8(tb, 0);          /* reserve offset 0 for void */
        off = (unsigned long)tb->n;
        ob_u8(tb, 2);                            /* T_POINTER */
        ob_u32(tb, tgt);
    } else {
        if (tb->n == 0) ob_u8(tb, 0);
        off = (unsigned long)tb->n;
        /* DWARF DW_ATE -> spec encoding: 1 signed,2 unsigned,3 float,5 char,6 uchar */
        int enc = t->enc == 4 ? 3 : t->enc == 6 ? 5 : t->enc == 8 ? 6
                : t->enc == 7 ? 2 : 1;
        ob_u8(tb, 1);                            /* T_BASE */
        ob_u8(tb, (unsigned)enc);
        ob_u8(tb, (unsigned)t->size);
        ob_u32(tb, str_intern(st, t->name));
    }
    map_from[*nmap] = dwoff; map_to[*nmap] = off; (*nmap)++;
    return off;
}

static int cmp_rows(const void *a, const void *b)
{
    unsigned long x = ((const struct row *)a)->addr, y = ((const struct row *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void write_embdbg(struct img *m, const char *path,
                         const unsigned char *bid_src, long bid_len)
{
    struct strtab st; memset(&st, 0, sizeof st);
    struct ob files = {0,0,0}, line = {0,0,0}, funcs = {0,0,0},
              frame = {0,0,0}, vars = {0,0,0}, types = {0,0,0};

    /* FILES: DWARF file index f (1-based) -> row f-1. */
    for (int i = 1; i < m->nfiles; i++) {
        ob_u32(&files, str_intern(&st, m->files[i]));
        ob_u32(&files, 0);                       /* dir_str (empty) */
        for (int k = 0; k < 32; k++) ob_u8(&files, 0);  /* content_hash */
    }
    int filecount = m->nfiles > 0 ? m->nfiles - 1 : 0;

    /* LINE: 16-byte rows, ascending by addr. */
    struct row *sorted = malloc((size_t)(m->nrows ? m->nrows : 1) * sizeof *sorted);
    memcpy(sorted, m->rows, (size_t)m->nrows * sizeof *sorted);
    qsort(sorted, (size_t)m->nrows, sizeof *sorted, cmp_rows);
    for (int i = 0; i < m->nrows; i++) {
        ob_u64(&line, sorted[i].addr);
        ob_u32(&line, sorted[i].end ? 0 : (unsigned long)sorted[i].line);
        ob_u16(&line, sorted[i].end ? 0 : (unsigned)(sorted[i].file - 1));
        ob_u16(&line, sorted[i].end ? 0 : LN_STMT);
    }
    free(sorted);

    /* TYPES + VARS + FRAME + FUNCS (VARS ordered by function). */
    unsigned long mf[256], mt[256]; int nmap = 0;
    int var_run = 0;
    for (int fi = 0; fi < m->ndfn; fi++) {
        struct dfunc *d = &m->dfn[fi];
        int first = var_run;
        for (int v = 0; v < d->nvars; v++) {
            struct dvar *dv = &d->vars[v];
            unsigned long toff = nmap < 250
                ? types_emit(m, &types, &st, mf, mt, &nmap, dv->type_off) : 0;
            ob_u32(&vars, str_intern(&st, dv->name));
            ob_u32(&vars, toff);
            ob_u64(&vars, d->lo);                /* scope_lo = function bounds */
            ob_u64(&vars, d->hi);                /* scope_hi */
            ob_u8(&vars, LOC_FBREG);
            ob_u8(&vars, dv->is_param ? VAR_PARAM : 0);
            for (int k = 0; k < 6; k++) ob_u8(&vars, 0);
            ob_u64(&vars, (unsigned long)dv->fbreg);
            var_run++;
        }
        /* FRAME: one per function. prologue_end = first line row past low_pc. */
        unsigned long pe = 0;
        for (int i = 0; i < m->nrows; i++)
            if (!m->rows[i].end && m->rows[i].addr >= d->lo && m->rows[i].addr < d->hi) {
                pe = m->rows[i].addr - d->lo; break;
            }
        ob_u32(&frame, pe);
        ob_u32(&frame, (unsigned long)(d->hi - d->lo));  /* epilogue_start_off */
        ob_u8(&frame, 0);                        /* FRAME_RBP */
        ob_u8(&frame, 0);
        ob_u16(&frame, 0);
        ob_u32(&frame, 0);
        /* FUNCS */
        ob_u64(&funcs, d->lo);
        ob_u64(&funcs, d->hi);
        ob_u32(&funcs, str_intern(&st, d->name));
        ob_u32(&funcs, (unsigned long)fi);       /* frame_idx */
        ob_u32(&funcs, (unsigned long)first);    /* first_var */
        ob_u32(&funcs, (unsigned long)d->nvars); /* var_count */
    }

    /* Assemble sections (kind, entsize, count, body). */
    struct { int kind; unsigned entsize, count; struct ob *body; } S[7] = {
        { DBG_KIND_STRTAB, 0, 0, &st.b },
        { DBG_KIND_FILES, 40, (unsigned)filecount, &files },
        { DBG_KIND_LINE,  16, (unsigned)m->nrows, &line },
        { DBG_KIND_FUNCS, 32, (unsigned)m->ndfn, &funcs },
        { DBG_KIND_FRAME, 16, (unsigned)m->ndfn, &frame },
        { DBG_KIND_VARS,  40, (unsigned)var_run, &vars },
        { DBG_KIND_TYPES, 0, 0, &types },
    };
    int nsec = 7;
    long table_off = 64;
    long body_off = table_off + (long)nsec * 24;

    struct ob out = {0,0,0};
    /* header (0x00..0x3F) */
    ob_bytes(&out, EMBDBG_MAGIC, 8);
    ob_u16(&out, 1);                             /* format_version */
    ob_u16(&out, 64);                            /* header_size */
    ob_u32(&out, 0);                             /* flags */
    unsigned char bid[32]; sha256(bid_src, bid_len, bid);
    ob_bytes(&out, bid, 32);                     /* build_id = SHA-256(image) */
    ob_u16(&out, (unsigned)nsec);
    ob_u16(&out, 0);                             /* reserved */
    ob_u32(&out, (unsigned long)table_off);
    long fsize_pos = out.n; ob_u32(&out, 0);     /* file_size (patched) */
    long hcrc_pos = out.n; ob_u32(&out, 0);      /* header_checksum (patched) */

    /* section table + running body offsets */
    long cur = body_off;
    long body_start = out.n + (long)nsec * 24;   /* where bodies begin in file */
    (void)body_start;
    for (int i = 0; i < nsec; i++) {
        long sz = S[i].body->n;
        ob_u16(&out, (unsigned)S[i].kind);
        ob_u16(&out, 0);                         /* flags */
        ob_u32(&out, S[i].entsize);
        ob_u32(&out, S[i].count);
        ob_u32(&out, (unsigned long)cur);        /* offset */
        ob_u32(&out, (unsigned long)sz);         /* size */
        ob_u32(&out, crc32c(S[i].body->p, sz));  /* checksum */
        cur += sz;
    }
    for (int i = 0; i < nsec; i++) ob_bytes(&out, S[i].body->p, S[i].body->n);

    /* patch file_size, then header_checksum over 0x00..0x3B with the field 0 */
    unsigned long fsz = (unsigned long)out.n;
    out.p[fsize_pos]=(unsigned char)fsz; out.p[fsize_pos+1]=(unsigned char)(fsz>>8);
    out.p[fsize_pos+2]=(unsigned char)(fsz>>16); out.p[fsize_pos+3]=(unsigned char)(fsz>>24);
    unsigned long hc = crc32c(out.p, 60);
    out.p[hcrc_pos]=(unsigned char)hc; out.p[hcrc_pos+1]=(unsigned char)(hc>>8);
    out.p[hcrc_pos+2]=(unsigned char)(hc>>16); out.p[hcrc_pos+3]=(unsigned char)(hc>>24);

    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write .embdbg");
    fwrite(out.p, 1, (size_t)out.n, f);
    fclose(f);
    free(out.p); free(st.b.p); free(files.p); free(line.p);
    free(funcs.p); free(frame.p); free(vars.p); free(types.p);
}

/* Read a type (and its chain) out of the TYPES blob into m->types, keyed by
 * its blob offset so type_name() resolves it the same as the DWARF path. */
static void read_type(struct img *m, const unsigned char *tb, long tn,
                      const char *strtab, unsigned long off)
{
    if (off == 0 || (long)off >= tn) return;
    for (int i = 0; i < m->ntypes; i++) if (m->types[i].off == off) return;
    int tag = tb[off];
    m->types = realloc(m->types, (size_t)(m->ntypes+1) * sizeof *m->types);
    struct dtype *t = &m->types[m->ntypes++];
    memset(t, 0, sizeof *t);
    t->off = off;
    if (tag == 2) {                              /* T_POINTER */
        unsigned long tgt = u32(tb + off + 1);
        t->is_ptr = 1; t->size = 8; t->pointee = tgt;
        read_type(m, tb, tn, strtab, tgt);
    } else if (tag == 1) {                        /* T_BASE */
        int enc = tb[off+1]; t->size = tb[off+2];
        unsigned long ns = u32(tb + off + 3);
        t->name = strtab + ns;
        t->enc = enc;
    }
}

static void load_embdbg(struct img *m)
{
    const unsigned char *b = m->b;
    unsigned nsec = u16(b + 0x30);
    unsigned long tab = u32(b + 0x34);
    const char *strtab = NULL; const unsigned char *typesb = NULL; long typesn = 0;
    const unsigned char *filesb=NULL,*lineb=NULL,*funcsb=NULL,*varsb=NULL;
    unsigned filec=0, linec=0, funcc=0, varc=0;
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned char *e = b + tab + i*24;
        int kind = u16(e); unsigned count = u32(e+8);
        unsigned long off = u32(e+12), size = u32(e+16);
        const unsigned char *body = b + off;
        if (kind == DBG_KIND_STRTAB) strtab = (const char *)body;
        else if (kind == DBG_KIND_FILES) { filesb = body; filec = count; }
        else if (kind == DBG_KIND_LINE)  { lineb = body; linec = count; }
        else if (kind == DBG_KIND_FUNCS) { funcsb = body; funcc = count; }
        else if (kind == DBG_KIND_VARS)  { varsb = body; varc = count; (void)varc; }
        else if (kind == DBG_KIND_TYPES) { typesb = body; typesn = (long)size; }
    }
    /* files: FILES row i -> m->files[i+1] (1-based, matching file_name). */
    m->nfiles = (int)filec + 1;
    m->files = calloc((size_t)m->nfiles, sizeof(char *));
    for (unsigned i = 0; i < filec; i++)
        m->files[i+1] = (char *)(strtab + u32(filesb + i*40));
    /* lines */
    for (unsigned i = 0; i < linec; i++) {
        const unsigned char *r = lineb + i*16;
        unsigned long addr = u64(r); unsigned line = (unsigned)u32(r+8);
        unsigned file = u16(r+12);
        add_row(m, addr, line ? (int)file + 1 : 0, (int)line, line ? 0 : 1);
    }
    /* funcs + their vars + coarse symtab-equivalent */
    m->dfn = calloc((size_t)(funcc ? funcc : 1), sizeof *m->dfn);
    m->fn  = calloc((size_t)(funcc ? funcc : 1), sizeof *m->fn);
    for (unsigned i = 0; i < funcc; i++) {
        const unsigned char *e = funcsb + i*32;
        unsigned long lo = u64(e), hi = u64(e+8);
        const char *nm = strtab + u32(e+16);
        unsigned first = u32(e+24), vc = u32(e+28);
        struct dfunc *d = &m->dfn[m->ndfn++];
        d->name = nm; d->lo = lo; d->hi = hi; d->nvars = (int)vc; d->vars = NULL;
        if (vc) d->vars = calloc((size_t)vc, sizeof *d->vars);
        for (unsigned v = 0; v < vc; v++) {
            const unsigned char *r = varsb + (first+v)*40;
            struct dvar *dv = &d->vars[v];
            dv->name = strtab + u32(r);
            dv->type_off = u32(r+4);
            dv->fbreg = (long)u64(r+32);
            dv->is_param = (r[0x19] & VAR_PARAM) ? 1 : 0;
            dv->has_loc = 1;
            if (typesb) read_type(m, typesb, typesn, strtab, dv->type_off);
        }
        struct func *cf = &m->fn[m->nfn++];
        cf->name = nm; cf->addr = lo; cf->size = hi - lo;
    }
}

/* Validate a .embdbg's structural integrity: magic, the header CRC32C, and
 * every section's CRC32C — the stale-/corrupt-info trap the spec builds in. */
static void cmd_verify(struct img *m)
{
    const unsigned char *b = m->b;
    if (m->len < 64 || memcmp(b, EMBDBG_MAGIC, 8) != 0) die("not a .embdbg");
    int ok = 1;
    unsigned long hc = u32(b + 0x3c), calc = crc32c(b, 60);
    printf("magic            OK\n");
    printf("header_checksum  %s\n", hc == calc ? "OK" : "BAD");
    if (hc != calc) ok = 0;
    unsigned nsec = u16(b + 0x30);
    unsigned long tab = u32(b + 0x34);
    for (unsigned i = 0; i < nsec; i++) {
        const unsigned char *e = b + tab + i*24;
        int kind = u16(e);
        unsigned long off = u32(e+12), size = u32(e+16), csum = u32(e+20);
        unsigned long c = crc32c(b + off, (long)size);
        printf("section kind %-2d  %s\n", kind, c == csum ? "OK" : "BAD");
        if (c != csum) ok = 0;
    }
    printf("build_id         ");
    for (int i = 0; i < 32; i++) printf("%02x", b[0x10 + i]);
    printf("\n");
    if (!ok) { printf("VERIFY FAILED\n"); exit(1); }
    printf("verify OK\n");
}

/* ===================================================================== *
 * TUI — an interactive browser over the debug info. A function list on the
 * left, a detail pane on the right (signature, source, typed locals). It is
 * static inspection (no live process — that is the kernel-gated half), so it
 * browses what .embdbg/DWARF hold. When stdout is not a terminal it prints a
 * plain full dump instead, so it stays scriptable and testable.
 * ===================================================================== */

/* The source file and 1-based line span of a function, from its line rows. */
static int func_span(struct img *m, const struct dfunc *d,
                     const char **file, int *lo, int *hi)
{
    int found = 0; *lo = 1 << 30; *hi = 0; *file = NULL;
    for (int i = 0; i < m->nrows; i++) {
        if (m->rows[i].end) continue;
        if (m->rows[i].addr >= d->lo && m->rows[i].addr < d->hi) {
            found = 1;
            if (m->rows[i].line < *lo) *lo = m->rows[i].line;
            if (m->rows[i].line > *hi) *hi = m->rows[i].line;
            if (!*file) *file = file_name(m, m->rows[i].file);
        }
    }
    return found;
}

/* Build the right-pane detail for a function into `lines`, returning count. */
static int build_detail(struct img *m, const struct dfunc *d,
                        char lines[][256], int maxlines)
{
    int n = 0;
    if (n < maxlines)
        snprintf(lines[n++], 256, "%s   [0x%lx, 0x%lx)", d->name, d->lo, d->hi);
    const char *file; int lo, hi;
    if (func_span(m, d, &file, &lo, &hi) && file) {
        if (n < maxlines) snprintf(lines[n++], 256, "%s:%d", file, lo);
        FILE *f = fopen(file, "r");
        if (f) {
            char buf[512]; int ln = 0;
            while (fgets(buf, sizeof buf, f) && n < maxlines) {
                ln++;
                if (ln < lo || ln > hi) continue;
                buf[strcspn(buf, "\n")] = 0;
                snprintf(lines[n++], 256, "  %4d | %.240s", ln, buf);
            }
            fclose(f);
        }
    }
    if (n < maxlines) lines[n++][0] = 0;
    if (n < maxlines) snprintf(lines[n++], 256, "%d variable(s):", d->nvars);
    for (int v = 0; v < d->nvars && n < maxlines; v++) {
        struct dvar *dv = &d->vars[v];
        snprintf(lines[n++], 256, "  %-5s %-12s %-8s @ rbp%+ld",
                 dv->is_param ? "param" : "local",
                 type_name(m, dv->type_off), dv->name, dv->fbreg);
    }
    return n;
}

static void tui_plain(struct img *m)
{
    printf("EmbDBG — %d function(s)\n", m->ndfn);
    for (int i = 0; i < m->ndfn; i++) {
        char lines[256][256];
        int n = build_detail(m, &m->dfn[i], lines, 256);
        printf("\n== %s ==\n", m->dfn[i].name);
        for (int j = 1; j < n; j++) printf("%s\n", lines[j]);
    }
}

static struct termios g_oldt;
static int g_raw = 0;
static void raw_off(void)
{
    if (g_raw) { tcsetattr(0, TCSANOW, &g_oldt); g_raw = 0;
                 printf("\033[?25h\033[0m\033[2J\033[H"); fflush(stdout); }
}
static void raw_on(void)
{
    tcgetattr(0, &g_oldt);
    struct termios t = g_oldt;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    g_raw = 1; atexit(raw_off);
    printf("\033[?25l");
}

static void tui_interactive(struct img *m)
{
    struct winsize ws; int W = 80, H = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 4) { W = ws.ws_col; H = ws.ws_row; }
    int LW = 24;                              /* left list width */
    if (W < 50) { LW = W / 3; }
    int sel = 0, top = 0;
    char search[64] = {0}; int searching = 0;
    int *vis = malloc((size_t)(m->ndfn ? m->ndfn : 1) * sizeof(int));

    raw_on();
    for (;;) {
        /* visible = functions whose name contains the search string */
        int nvis = 0;
        for (int i = 0; i < m->ndfn; i++)
            if (!search[0] || (m->dfn[i].name && strstr(m->dfn[i].name, search)))
                vis[nvis++] = i;
        if (sel >= nvis) sel = nvis ? nvis - 1 : 0;
        if (sel < top) top = sel;
        int rows = H - 3;
        if (sel >= top + rows) top = sel - rows + 1;

        char det[256][256]; int ndet = 0;
        if (nvis) ndet = build_detail(m, &m->dfn[vis[sel]], det, 256);

        printf("\033[2J\033[H");
        printf("\033[1m EmbDBG \033[0m %.*s   %d fn%s   \033[2m[j/k] nav  [/] search  [q] quit\033[0m\n",
               W - 40 > 0 ? W - 40 : 8,
               "debug view", nvis, nvis == 1 ? "" : "s");
        if (searching) printf(" /\033[1m%s\033[0m\n", search);
        else printf(" \033[2m%s\033[0m\n", search[0] ? search : "");
        for (int r = 0; r < rows; r++) {
            /* left */
            int li = top + r;
            char left[128];
            if (li < nvis) snprintf(left, sizeof left, "%-*.*s",
                                    LW, LW, m->dfn[vis[li]].name);
            else snprintf(left, sizeof left, "%*s", LW, "");
            if (li == sel) printf(" \033[7m%s\033[0m \033[2m|\033[0m ", left);
            else           printf(" %s \033[2m|\033[0m ", left);
            /* right */
            int rw = W - LW - 5;
            if (r < ndet) printf("\033[2m%.*s\033[0m", rw > 0 ? rw : 0, det[r]);
            printf("\n");
        }

        unsigned char c;
        if (read(0, &c, 1) != 1) break;
        if (searching) {
            if (c == '\r' || c == '\n' || c == 27) { searching = 0; }
            else if (c == 127 || c == 8) { int l = (int)strlen(search); if (l) search[l-1] = 0; }
            else if (c >= 32 && c < 127) { int l = (int)strlen(search);
                if (l < (int)sizeof search - 1) { search[l] = (char)c; search[l+1] = 0; } }
            continue;
        }
        if (c == 'q') break;
        else if (c == 'j') { if (sel + 1 < nvis) sel++; }
        else if (c == 'k') { if (sel > 0) sel--; }
        else if (c == 'g') sel = 0;
        else if (c == 'G') sel = nvis ? nvis - 1 : 0;
        else if (c == '/') { searching = 1; search[0] = 0; }
        else if (c == 27) {                   /* arrow keys: ESC [ A/B */
            unsigned char s1, s2;
            if (read(0, &s1, 1) == 1 && s1 == '[' && read(0, &s2, 1) == 1) {
                if (s2 == 'A' && sel > 0) sel--;
                else if (s2 == 'B' && sel + 1 < nvis) sel++;
            }
        }
    }
    raw_off();
    free(vis);
}

static void cmd_tui(struct img *m)
{
    if (isatty(0) && isatty(1)) tui_interactive(m);
    else tui_plain(m);            /* piped/non-tty: a scriptable full dump */
}

/* Link-time entry point (used by EmbLD): parse one relocatable object's DWARF
 * with `addr_bias` = its final .text vaddr so addresses come out absolute,
 * and write a .embdbg whose build_id is the SHA-256 of the linked `image`. */
int embdbg_emit_object(const unsigned char *obj, long objlen, long addr_bias,
                       const unsigned char *image, long imagelen,
                       const char *out)
{
    struct img m; memset(&m, 0, sizeof m);
    m.b = (unsigned char *)obj; m.len = objlen;
    g_addr_bias = addr_bias;
    load_sections(&m);
    load_funcs(&m);
    decode_lines(&m);
    decode_info(&m);
    write_embdbg(&m, out, image, imagelen);
    g_addr_bias = 0;
    return 0;
}

#ifndef EMBDBG_NO_MAIN
int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "EmbDBG v0 — EmbLinkOS debug-info reader\n"
            "usage: embdbg FILE funcs               list functions + ranges\n"
            "       embdbg FILE lines               the address->file:line table\n"
            "       embdbg FILE symbolize ADDR...   addr -> func+off  file:line\n"
            "       embdbg FILE backtrace ADDR...   symbolize a caller chain\n"
            "       embdbg FILE where ADDR          source context + locals in scope\n"
            "       embdbg FILE list ADDR           source lines around addr\n"
            "       embdbg FILE info FUNC           a function's params/locals\n"
            "       embdbg FILE tui                 interactive browser (plain dump if piped)\n"
            "       embdbg FILE.o emit OUT.embdbg   convert DWARF -> native .embdbg\n"
            "   FILE may be an ELF (reads DWARF) or a .embdbg (reads it natively).\n");
        return 1;
    }
    struct img m; memset(&m, 0, sizeof m);
    m.b = slurp(argv[1], &m.len);
    int is_embdbg = (m.len >= 8 && memcmp(m.b, EMBDBG_MAGIC, 8) == 0);
    if (is_embdbg) {
        load_embdbg(&m);
    } else {
        load_sections(&m);
        load_funcs(&m);
        decode_lines(&m);
        decode_info(&m);
    }

    const char *cmd = argv[2];
    if (strcmp(cmd, "emit") == 0) {
        if (is_embdbg) die("input is already .embdbg");
        if (argc < 4) die("emit needs an output path");
        write_embdbg(&m, argv[3], m.b, m.len);   /* build_id = hash of this ELF */
        return 0;
    }
    if (strcmp(cmd, "verify") == 0) {
        if (!is_embdbg) die("verify needs a .embdbg file");
        cmd_verify(&m);
        return 0;
    }
    if (strcmp(cmd, "funcs") == 0)          cmd_funcs(&m);
    else if (strcmp(cmd, "lines") == 0)     cmd_lines(&m);
    else if (strcmp(cmd, "symbolize") == 0) cmd_symbolize(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "backtrace") == 0) cmd_backtrace(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "where") == 0)     cmd_where(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "list") == 0)      cmd_list(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "info") == 0)      cmd_info(&m, argc - 3, argv + 3);
    else if (strcmp(cmd, "tui") == 0)       cmd_tui(&m);
    else { fprintf(stderr, "embdbg: unknown command '%s'\n", cmd); return 1; }
    return 0;
}
#endif /* EMBDBG_NO_MAIN */
