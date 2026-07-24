/* EmbLD — the integrated linker. See link.h.
 *
 * Structure, in load-sequence order (the read is the algorithm, the way
 * embread mirrors the EMBX §6 load): parse each object → collect its
 * allocated sections → resolve the global symbol table → lay the
 * sections out into the text and data segments at fixed vaddrs → apply
 * relocations → write the ET_EXEC.
 *
 * Correct-and-slow first (ARCHITECTURE §3): the symbol table is a linear
 * scan, which is fine for the bounded symbol set B1 pulls from libc.a; a
 * hash lands when a measured corpus makes it slow, not before.
 */
#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../elf/elf.h"

/* EmbLink app image (TARGET_ABI §4a, newlib.ld): text at 0x400000
 * (R+X), then a page boundary (W^X), then data (R+W). */
#define DEFAULT_BASE 0x400000ULL
#define PAGE 0x1000ULL

/* ---- input objects ---- */

struct object {
    const char *name;         /* for diagnostics: "libc.a(printf.o)" etc */
    unsigned char *buf;
    long len;
    Elf64_Ehdr *eh;
    Elf64_Shdr *shdrs;
    int nsh;
    const char *shstr;
    Elf64_Sym *syms;
    int nsym;
    const char *symstr;
    int local_syms;           /* sh_info of the symtab: [0,local) are LOCAL */
    /* per input section: index into insecs[], or -1 if not laid out */
    int *sec_out;
};

/* An allocated input section placed into the output. */
enum seg { SEG_TEXT, SEG_DATA };

struct insec {
    struct object *obj;
    int shndx;
    const unsigned char *data; /* NULL for NOBITS (.bss) */
    Elf64_Xword size;
    Elf64_Xword align;
    int is_bss;
    enum seg seg;
    Elf64_Addr vaddr;          /* assigned in layout */
};

struct symbol {
    const char *name;
    struct object *obj;        /* defining object, or NULL if undefined */
    int insec;                 /* insecs index of its section, or -1 (ABS) */
    Elf64_Addr value;          /* section-relative until layout, then absolute */
    int defined;
    int weak;
    int common;                /* a tentative (COMMON) definition */
    Elf64_Xword size;          /* for COMMON: the size to reserve */
    Elf64_Xword align;         /* for COMMON */
};

struct linker {
    struct object **objs;
    int nobj, capobj;
    struct insec *insecs;
    int nsec, capsec;
    struct symbol *syms;
    int nsym, capsym;
    Elf64_Addr base;
    const char *entry;
};

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embld: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* ---- symbol table (linear; §3 correct-and-slow) ---- */

static struct symbol *sym_find(struct linker *l, const char *name)
{
    for (int i = 0; i < l->nsym; i++)
        if (strcmp(l->syms[i].name, name) == 0)
            return &l->syms[i];
    return NULL;
}

static struct symbol *sym_intern(struct linker *l, const char *name)
{
    struct symbol *s = sym_find(l, name);
    if (s)
        return s;
    if (l->nsym == l->capsym) {
        l->capsym = l->capsym ? l->capsym * 2 : 64;
        l->syms = xrealloc(l->syms, (size_t)l->capsym * sizeof *l->syms);
    }
    s = &l->syms[l->nsym++];
    memset(s, 0, sizeof *s);
    s->name = name;
    s->insec = -1;
    return s;
}

/* ---- object parsing ---- */

static Elf64_Shdr *sh_at(struct object *o, int i) { return &o->shdrs[i]; }

static struct object *parse_object(const char *name, unsigned char *buf,
                                   long len)
{
    if (len < (long)sizeof(Elf64_Ehdr))
        die("%s: too small to be an object", name);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        die("%s: not an ELF file", name);
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB)
        die("%s: not 64-bit little-endian", name);
    if (eh->e_type != ET_REL)
        die("%s: not a relocatable object (ET_REL)", name);
    if (eh->e_machine != EM_X86_64)
        die("%s: not x86-64", name);

    struct object *o = xcalloc(1, sizeof *o);
    o->name = name;
    o->buf = buf;
    o->len = len;
    o->eh = eh;
    o->nsh = eh->e_shnum;
    o->shdrs = (Elf64_Shdr *)(buf + eh->e_shoff);
    if (eh->e_shoff + (Elf64_Off)o->nsh * sizeof(Elf64_Shdr) > (Elf64_Off)len)
        die("%s: section headers run past end of file", name);
    o->shstr = (const char *)(buf + o->shdrs[eh->e_shstrndx].sh_offset);
    o->sec_out = xmalloc((size_t)o->nsh * sizeof(int));
    for (int i = 0; i < o->nsh; i++)
        o->sec_out[i] = -1;

    /* find the symbol table */
    for (int i = 0; i < o->nsh; i++) {
        if (o->shdrs[i].sh_type == SHT_SYMTAB) {
            Elf64_Shdr *sh = &o->shdrs[i];
            o->syms = (Elf64_Sym *)(buf + sh->sh_offset);
            o->nsym = (int)(sh->sh_size / sizeof(Elf64_Sym));
            o->local_syms = (int)sh->sh_info;
            o->symstr = (const char *)(buf + o->shdrs[sh->sh_link].sh_offset);
            break;
        }
    }
    return o;
}

/* Collect the object's SHF_ALLOC sections into the global insec list and
 * record where each landed (sec_out), so relocations and symbols can map
 * a (object, section) back to its output placement. */
static void collect_sections(struct linker *l, struct object *o)
{
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *sh = sh_at(o, i);
        if (!(sh->sh_flags & SHF_ALLOC))
            continue;
        if (l->nsec == l->capsec) {
            l->capsec = l->capsec ? l->capsec * 2 : 64;
            l->insecs = xrealloc(l->insecs,
                                 (size_t)l->capsec * sizeof *l->insecs);
        }
        struct insec *s = &l->insecs[l->nsec];
        memset(s, 0, sizeof *s);
        s->obj = o;
        s->shndx = i;
        s->size = sh->sh_size;
        s->align = sh->sh_addralign ? sh->sh_addralign : 1;
        s->is_bss = sh->sh_type == SHT_NOBITS;
        s->data = s->is_bss ? NULL : o->buf + sh->sh_offset;
        /* text segment: executable OR read-only allocatable (.rodata);
         * data segment: writable (.data, .bss). W^X by construction. */
        s->seg = (sh->sh_flags & SHF_WRITE) ? SEG_DATA : SEG_TEXT;
        o->sec_out[i] = l->nsec;
        l->nsec++;
    }
}

/* Add this object's global definitions and note its undefined references.
 * The resolution rule (static link): a strong definition wins; a second
 * strong definition of the same name is an error; a weak definition
 * yields to a strong one; COMMON (tentative) is superseded by any real
 * definition and merged with other COMMONs at the largest size. */
static void add_symbols(struct linker *l, struct object *o)
{
    for (int i = o->local_syms; i < o->nsym; i++) {
        Elf64_Sym *sy = &o->syms[i];
        const char *name = o->symstr + sy->st_name;
        if (!*name)
            continue;
        int bind = ELF64_ST_BIND(sy->st_info);
        int weak = bind == STB_WEAK;
        struct symbol *g = sym_intern(l, name);

        if (sy->st_shndx == SHN_UNDEF)
            continue; /* a reference; may be satisfied by a later object */

        if (sy->st_shndx == SHN_COMMON) {
            /* tentative definition: reserve space unless something real
             * defines it. Largest size, strongest alignment win. */
            if (g->defined && !g->common)
                continue; /* a real definition already supersedes it */
            g->common = 1;
            g->defined = 1;
            g->obj = o;
            if (sy->st_size > g->size)
                g->size = sy->st_size;
            if (sy->st_value > g->align) /* COMMON: st_value is alignment */
                g->align = sy->st_value;
            continue;
        }

        /* a real (allocated or ABS) definition */
        if (g->defined && !g->common && !g->weak && !weak)
            die("multiple definition of '%s' (in %s and %s)", name,
                g->obj ? g->obj->name : "?", o->name);
        if (g->defined && !g->weak && weak)
            continue; /* keep the strong one already present */

        g->defined = 1;
        g->common = 0;
        g->weak = weak;
        g->obj = o;
        g->value = sy->st_value;
        g->insec = (sy->st_shndx == SHN_ABS) ? -1
                                             : o->sec_out[sy->st_shndx];
    }
}

/* ---- layout ---- */

static Elf64_Addr align_up(Elf64_Addr v, Elf64_Xword a)
{
    if (a < 2)
        return v;
    return (v + a - 1) & ~(a - 1);
}

/* Assign every allocated section a vaddr: all of the text segment first
 * (non-writable), a page break for W^X, then data, then bss (NOBITS,
 * which the loader zero-fills). Returns via out-params the segment
 * geometry the ET_EXEC writer needs. */
static void layout(struct linker *l,
                   Elf64_Addr *text_start, Elf64_Xword *text_size,
                   Elf64_Addr *data_start, Elf64_Xword *data_filesz,
                   Elf64_Xword *data_memsz)
{
    Elf64_Addr va = l->base;

    /* text: SEG_TEXT, non-bss (there is no bss in text) */
    *text_start = va;
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->seg != SEG_TEXT)
            continue;
        va = align_up(va, s->align);
        s->vaddr = va;
        va += s->size;
    }
    *text_size = va - *text_start;

    /* page break, then data: file-backed sections first, then bss */
    va = align_up(va, PAGE);
    *data_start = va;
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->seg != SEG_DATA || s->is_bss)
            continue;
        va = align_up(va, s->align);
        s->vaddr = va;
        va += s->size;
    }
    *data_filesz = va - *data_start;
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->seg != SEG_DATA || !s->is_bss)
            continue;
        va = align_up(va, s->align);
        s->vaddr = va;
        va += s->size;
    }
    *data_memsz = va - *data_start;
}

/* Turn every section-relative symbol value into a final absolute vaddr,
 * now that every section has one. */
static void finalize_symbols(struct linker *l)
{
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!g->defined)
            continue;
        if (g->common)
            continue; /* placed with the synthetic .bss below (B2) */
        if (g->insec >= 0)
            g->value += l->insecs[g->insec].vaddr;
        /* insec == -1 is ABS: value is already absolute */
    }
}

/* The absolute vaddr of a symbol referenced by a relocation. Undefined
 * weak binds to 0 (TARGET_ABI §4a). A strong undefined is a hard error:
 * the static link has no resolver to defer to. */
static Elf64_Addr reloc_symval(struct linker *l, struct object *o,
                               Elf64_Word symidx, int *is_undef_weak)
{
    Elf64_Sym *sy = &o->syms[symidx];
    const char *name = o->symstr + sy->st_name;
    *is_undef_weak = 0;

    if (ELF64_ST_BIND(sy->st_info) == STB_LOCAL) {
        /* a local symbol: resolve within this object directly */
        if (sy->st_shndx == SHN_ABS)
            return sy->st_value;
        int out = o->sec_out[sy->st_shndx];
        if (out < 0)
            die("%s: local symbol in a non-allocated section", o->name);
        return l->insecs[out].vaddr + sy->st_value;
    }

    struct symbol *g = *name ? sym_find(l, name) : NULL;
    if (g && g->defined)
        return g->value;
    if (ELF64_ST_BIND(sy->st_info) == STB_WEAK) {
        *is_undef_weak = 1;
        return 0;
    }
    die("undefined symbol '%s' (referenced by %s)", name, o->name);
    return 0;
}

/* ---- relocation ---- */

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put64(unsigned char *p, unsigned long long v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void apply_relocs(struct linker *l, struct object *o)
{
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *rsh = sh_at(o, i);
        if (rsh->sh_type != SHT_RELA)
            continue;
        int target = (int)rsh->sh_info;         /* section being relocated */
        if (o->sec_out[target] < 0)
            continue; /* relocations for a non-allocated section (debug) */
        struct insec *ts = &l->insecs[o->sec_out[target]];
        if (ts->is_bss)
            continue;
        /* the output bytes to patch live in the object's own buffer; we
         * patch there, then copy the section into the image at write. */
        unsigned char *base = o->buf + sh_at(o, target)->sh_offset;
        Elf64_Rela *r = (Elf64_Rela *)(o->buf + rsh->sh_offset);
        int n = (int)(rsh->sh_size / sizeof(Elf64_Rela));

        for (int j = 0; j < n; j++) {
            Elf64_Word type = ELF64_R_TYPE(r[j].r_info);
            Elf64_Word symi = ELF64_R_SYM(r[j].r_info);
            int uw;
            Elf64_Addr S = reloc_symval(l, o, symi, &uw);
            long long A = r[j].r_addend;
            Elf64_Addr P = ts->vaddr + r[j].r_offset; /* patch site vaddr */
            unsigned char *loc = base + r[j].r_offset;

            switch (type) {
            case R_X86_64_64:
                put64(loc, (unsigned long long)(S + A));
                break;
            case R_X86_64_32:
            case R_X86_64_32S:
                put32(loc, (unsigned int)(S + A));
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                /* TARGET_ABI §4a: PLT32 is a plain PC32 in a static
                 * link — no PLT slot is minted. */
                put32(loc, (unsigned int)(long)((long long)S + A -
                                                (long long)P));
                break;
            case R_X86_64_PC64:
                put64(loc, (unsigned long long)((long long)S + A -
                                                (long long)P));
                break;
            default:
                die("%s: unsupported relocation type %u (this is the "
                    "next linker increment, not a bug in your program)",
                    o->name, type);
            }
        }
    }
}

/* ---- output ---- */

static void write_exec(struct linker *l, const char *out,
                       Elf64_Addr entry,
                       Elf64_Addr text_start, Elf64_Xword text_size,
                       Elf64_Addr data_start, Elf64_Xword data_filesz,
                       Elf64_Xword data_memsz)
{
    /* File layout: ehdr, 2 phdrs, then the text bytes at a file offset
     * congruent to their vaddr mod PAGE, then the data bytes likewise.
     * The kernel loader maps PT_LOAD by (offset, vaddr, filesz, memsz);
     * keeping offset ≡ vaddr (mod PAGE) is what lets it map file pages
     * directly. */
    Elf64_Off hdrs = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);

    Elf64_Off text_off = hdrs;
    /* keep text_off ≡ text_start (mod PAGE) */
    text_off = align_up(text_off, PAGE) + (text_start & (PAGE - 1));
    if (text_off < hdrs)
        text_off += PAGE;
    Elf64_Off data_off = text_off + text_size;
    data_off = align_up(data_off, PAGE) + (data_start & (PAGE - 1));

    Elf64_Off total = data_off + data_filesz;
    unsigned char *img = xcalloc(1, (size_t)total);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_EXEC;              /* never ET_DYN — TARGET_ABI §4b */
    eh->e_machine = EM_X86_64;
    eh->e_version = EV_CURRENT;
    eh->e_entry = entry;
    eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_ehsize = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 2;

    Elf64_Phdr *ph = (Elf64_Phdr *)(img + sizeof(Elf64_Ehdr));
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_X;
    ph[0].p_offset = text_off;
    ph[0].p_vaddr = text_start;
    ph[0].p_paddr = text_start;
    ph[0].p_filesz = text_size;
    ph[0].p_memsz = text_size;
    ph[0].p_align = PAGE;
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_W;
    ph[1].p_offset = data_off;
    ph[1].p_vaddr = data_start;
    ph[1].p_paddr = data_start;
    ph[1].p_filesz = data_filesz;
    ph[1].p_memsz = data_memsz;       /* memsz > filesz = the .bss tail */
    ph[1].p_align = PAGE;

    /* copy each allocated, file-backed section to its place */
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->is_bss || !s->data)
            continue;
        Elf64_Off base = (s->seg == SEG_TEXT) ? text_off : data_off;
        Elf64_Addr segva = (s->seg == SEG_TEXT) ? text_start : data_start;
        memcpy(img + base + (s->vaddr - segva), s->data, (size_t)s->size);
    }

    FILE *f = fopen(out, "wb");
    if (!f)
        die("cannot open '%s' for writing", out);
    if (fwrite(img, 1, (size_t)total, f) != (size_t)total)
        die("write error on '%s'", out);
    fclose(f);
    free(img);
}

/* ---- driver ---- */

static unsigned char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open '%s'", path);
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = xmalloc((size_t)*len);
    if (fread(buf, 1, (size_t)*len, f) != (size_t)*len)
        die("read error on '%s'", path);
    fclose(f);
    return buf;
}

int embld_link(const char **inputs, int ninputs, const char *out,
               const struct link_opts *opts)
{
    struct linker l;
    memset(&l, 0, sizeof l);
    l.base = (opts && opts->base) ? opts->base : DEFAULT_BASE;
    l.entry = (opts && opts->entry) ? opts->entry : "_start";

    for (int i = 0; i < ninputs; i++) {
        long len;
        unsigned char *buf = read_file(inputs[i], &len);
        struct object *o = parse_object(inputs[i], buf, len);
        if (l.nobj == l.capobj) {
            l.capobj = l.capobj ? l.capobj * 2 : 8;
            l.objs = xrealloc(l.objs, (size_t)l.capobj * sizeof *l.objs);
        }
        l.objs[l.nobj++] = o;
        collect_sections(&l, o);
        add_symbols(&l, o);
    }

    Elf64_Addr text_start, data_start;
    Elf64_Xword text_size, data_filesz, data_memsz;
    layout(&l, &text_start, &text_size, &data_start, &data_filesz,
           &data_memsz);
    finalize_symbols(&l);

    struct symbol *e = sym_find(&l, l.entry);
    if (!e || !e->defined)
        die("entry symbol '%s' is undefined", l.entry);

    for (int i = 0; i < l.nobj; i++)
        apply_relocs(&l, l.objs[i]);

    write_exec(&l, out, e->value, text_start, text_size,
               data_start, data_filesz, data_memsz);
    return 0;
}
