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

struct img {
    unsigned char *b; long len;
    struct sec *sec; int nsec;
    struct func *fn; int nfn;
    struct row *rows; int nrows;
    char **files; int nfiles;   /* file_names[1..], index 1-based in DWARF */
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

/* Relocations against .debug_line (a relocatable .o writes the set_address
 * operand as 0 and carries the real .text offset in the addend). Map operand
 * section-offset -> addend so the decoder can recover the address. */
static unsigned long reloc_at(struct img *m, unsigned long line_off, int *found)
{
    struct sec *r = find_sec(m, ".rela.debug_line");
    *found = 0;
    if (!r) return 0;
    Elf64_Rela *rel = (Elf64_Rela *)r->data;
    int n = (int)(r->size / sizeof *rel);
    for (int i = 0; i < n; i++)
        if (rel[i].r_offset == line_off) { *found = 1; return (unsigned long)rel[i].r_addend; }
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
                    int found; unsigned long a = reloc_at(m, (unsigned long)i, &found);
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

static void cmd_symbolize(struct img *m, int argc, char **argv)
{
    for (int a = 0; a < argc; a++) {
        unsigned long addr = strtoul(argv[a], NULL, 0);
        const struct func *f = func_at(m, addr);
        const struct row *r = line_at(m, addr);
        printf("0x%lx  ", addr);
        if (f) printf("%s+0x%lx", f->name, addr - f->addr);
        else   printf("??");
        if (r) printf("  %s:%d", file_name(m, r->file), r->line);
        else   printf("  (no line info)");
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "EmbDBG v0 — EmbLinkOS debug-info reader\n"
            "usage: embdbg FILE funcs\n"
            "       embdbg FILE lines\n"
            "       embdbg FILE symbolize ADDR...\n");
        return 1;
    }
    struct img m; memset(&m, 0, sizeof m);
    m.b = slurp(argv[1], &m.len);
    load_sections(&m);
    load_funcs(&m);
    decode_lines(&m);

    const char *cmd = argv[2];
    if (strcmp(cmd, "funcs") == 0)          cmd_funcs(&m);
    else if (strcmp(cmd, "lines") == 0)     cmd_lines(&m);
    else if (strcmp(cmd, "symbolize") == 0) cmd_symbolize(&m, argc - 3, argv + 3);
    else { fprintf(stderr, "embdbg: unknown command '%s'\n", cmd); return 1; }
    return 0;
}
