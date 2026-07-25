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
#include "../embx/embx.h"
#include "../../tools/embdbg/embdbg_core.h"

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

/* Output sections, in layout order. Input sections are grouped by name
 * into these, so — the reason this exists — all .init_array inputs land
 * contiguously and the bracket symbols __init_array_start/_end are just
 * the group's bounds, the way a linker script's `*(.init_array)` places
 * them. Order matters: constructors precede ordinary data; .bss is last
 * (NOBITS, the memsz tail). */
enum osec {
    OSEC_TEXT, OSEC_RODATA,               /* text segment (R+X) */
    OSEC_INIT_ARRAY, OSEC_FINI_ARRAY,     /* data segment (R+W) */
    OSEC_CTORS, OSEC_DTORS,
    OSEC_DATA, OSEC_BSS,
    OSEC_COUNT
};

struct insec {
    struct object *obj;
    int shndx;
    const char *name;
    const unsigned char *data; /* NULL for NOBITS (.bss) */
    Elf64_Xword size;
    Elf64_Xword align;
    int is_bss;
    enum seg seg;
    int osec;
    Elf64_Addr vaddr;          /* assigned in layout */
};

/* The final [start,end) vaddr span of each output section — the source
 * of the bracket symbols. */
struct osec_bound { Elf64_Addr start, end; };

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

/* A static archive (.a) is a pool of member objects; a member is pulled
 * into the link only if it defines a symbol something still needs
 * (classic archive semantics, TARGET_ABI §4b). */
struct member {
    const char *name;         /* "libc.a(malloc.o)" for diagnostics */
    unsigned char *buf;
    long len;
    int pulled;
    struct object *obj;       /* parsed lazily on first inspection */
};

struct archive {
    const char *name;
    struct member *members;
    int nmembers;
};

struct linker {
    struct object **objs;
    int nobj, capobj;
    struct insec *insecs;
    int nsec, capsec;
    struct symbol *syms;
    int nsym, capsym;
    struct archive **archives;
    int narch, caparch;
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

/* Which output section a named input section joins. Matched by prefix so
 * .text.foo joins .text, .data.rel joins .data, and so on — exactly the
 * grouping a linker script's wildcards express. */
static int classify_osec(const char *name, int writable, int is_bss)
{
    static const struct { const char *pfx; int osec; } map[] = {
        { ".init_array", OSEC_INIT_ARRAY },
        { ".fini_array", OSEC_FINI_ARRAY },
        { ".ctors", OSEC_CTORS },
        { ".dtors", OSEC_DTORS },
        { ".text", OSEC_TEXT },
        { ".rodata", OSEC_RODATA },
        { ".data", OSEC_DATA },
        { ".bss", OSEC_BSS },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t n = strlen(map[i].pfx);
        if (strncmp(name, map[i].pfx, n) == 0 &&
            (name[n] == 0 || name[n] == '.'))
            return map[i].osec;
    }
    /* an unrecognized allocated section: place by its flags */
    if (is_bss)
        return OSEC_BSS;
    return writable ? OSEC_DATA : OSEC_RODATA;
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
        s->name = o->shstr + sh->sh_name;
        s->size = sh->sh_size;
        s->align = sh->sh_addralign ? sh->sh_addralign : 1;
        s->is_bss = sh->sh_type == SHT_NOBITS;
        s->data = s->is_bss ? NULL : o->buf + sh->sh_offset;
        s->osec = classify_osec(s->name, sh->sh_flags & SHF_WRITE,
                                s->is_bss);
        /* text segment: executable OR read-only allocatable (.rodata);
         * data segment: writable and the constructor arrays. W^X by
         * construction — the constructor arrays are read-only data that
         * the ABI keeps in the writable segment (they hold relocated
         * pointers), never executable. */
        s->seg = (s->osec == OSEC_TEXT || s->osec == OSEC_RODATA)
                     ? SEG_TEXT : SEG_DATA;
        o->sec_out[i] = l->nsec;
        l->nsec++;
    }
}

static void add_symbols(struct linker *l, struct object *o);

/* Bring a parsed object fully into the link: register it, lay out its
 * allocated sections, and merge its symbols. */
static void add_object(struct linker *l, struct object *o)
{
    if (l->nobj == l->capobj) {
        l->capobj = l->capobj ? l->capobj * 2 : 8;
        l->objs = xrealloc(l->objs, (size_t)l->capobj * sizeof *l->objs);
    }
    l->objs[l->nobj++] = o;
    collect_sections(l, o);
    add_symbols(l, o);
}

/* ---- static archives (ar format) ---- */

/* The System V / GNU ar header before each member — 60 bytes, ASCII
 * decimal fields. The member name is what makes it fiddly: a short name
 * is "name/" (trailing slash); a long name is "/offset" into the "//"
 * string-table member. The "/" and "//" members are the symbol index
 * and the string table — skipped, because members are scanned directly. */
struct ar_hdr {
    char name[16];
    char mtime[12];
    char uid[6];
    char gid[6];
    char mode[8];
    char size[10];
    char end[2];              /* "`\n" */
};

static long ar_num(const char *p, int n)
{
    long v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++)
        v = v * 10 + (p[i] - '0');
    return v;
}

static int is_archive(const unsigned char *buf, long len)
{
    return len >= 8 && memcmp(buf, "!<arch>\n", 8) == 0;
}

/* Parses an archive into its member list. Long names resolve through the
 * "//" string table; the "/"/"" symbol-index member is skipped (members
 * are inspected directly at pull time). */
static void parse_archive(struct linker *l, const char *name,
                          unsigned char *buf, long len)
{
    struct archive *ar = xcalloc(1, sizeof *ar);
    ar->name = name;
    const char *longnames = NULL;

    long off = 8; /* past "!<arch>\n" */
    while (off + (long)sizeof(struct ar_hdr) <= len) {
        struct ar_hdr *h = (struct ar_hdr *)(buf + off);
        if (h->end[0] != '`' || h->end[1] != '\n')
            die("%s: corrupt archive header at offset %ld", name, off);
        long msize = ar_num(h->size, 10);
        long data = off + sizeof(struct ar_hdr);

        /* member name */
        char mname[256];
        if (h->name[0] == '/' && h->name[1] == '/') {
            /* the long-name string table */
            longnames = (const char *)(buf + data);
            mname[0] = 0;
        } else if (h->name[0] == '/' &&
                   (h->name[1] == ' ' || h->name[1] == 0)) {
            mname[0] = 0; /* the symbol index — skipped */
        } else if (h->name[0] == '/') {
            long noff = ar_num(h->name + 1, 15);
            const char *s = longnames ? longnames + noff : "?";
            int k = 0;
            while (s[k] && s[k] != '/' && s[k] != '\n' && k < 255) {
                mname[k] = s[k];
                k++;
            }
            mname[k] = 0;
        } else {
            int k = 0;
            while (k < 16 && h->name[k] && h->name[k] != '/' &&
                   h->name[k] != ' ') {
                mname[k] = h->name[k];
                k++;
            }
            mname[k] = 0;
        }

        if (mname[0]) { /* a real object member */
            if (ar->nmembers % 64 == 0)
                ar->members = xrealloc(ar->members,
                    (size_t)(ar->nmembers + 64) * sizeof *ar->members);
            struct member *m = &ar->members[ar->nmembers++];
            memset(m, 0, sizeof *m);
            size_t nlen = strlen(name) + strlen(mname) + 4;
            char *full = xmalloc(nlen);
            snprintf(full, nlen, "%s(%s)", name, mname);
            m->name = full;
            m->buf = buf + data;
            m->len = msize;
        }
        off = data + msize;
        if (off & 1)
            off++; /* members are 2-byte aligned */
    }

    if (l->narch == l->caparch) {
        l->caparch = l->caparch ? l->caparch * 2 : 8;
        l->archives = xrealloc(l->archives,
                               (size_t)l->caparch * sizeof *l->archives);
    }
    l->archives[l->narch++] = ar;
}

/* Does this parsed object define a symbol that is currently referenced
 * but undefined? That is exactly the condition to pull an archive
 * member. */
static int defines_needed(struct linker *l, struct object *o)
{
    for (int i = o->local_syms; i < o->nsym; i++) {
        Elf64_Sym *sy = &o->syms[i];
        if (sy->st_shndx == SHN_UNDEF)
            continue;
        const char *nm = o->symstr + sy->st_name;
        if (!*nm)
            continue;
        struct symbol *g = sym_find(l, nm);
        if (g && !g->defined)
            return 1;
    }
    return 0;
}

/* Pull members to a fixed point: repeatedly, any not-yet-pulled member
 * that satisfies a still-undefined symbol is linked in — which may
 * create new undefined symbols an earlier member then satisfies, so the
 * scan repeats until a whole pass pulls nothing. This handles libc.a's
 * two-way dependencies (malloc↔sbrk, printf→malloc) without caring about
 * member order. */
static void pull_archives(struct linker *l)
{
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int a = 0; a < l->narch; a++) {
            struct archive *ar = l->archives[a];
            for (int m = 0; m < ar->nmembers; m++) {
                struct member *mem = &ar->members[m];
                if (mem->pulled)
                    continue;
                if (!mem->obj)
                    mem->obj = parse_object(mem->name, mem->buf, mem->len);
                if (!defines_needed(l, mem->obj))
                    continue;
                mem->pulled = 1;
                add_object(l, mem->obj);
                progress = 1;
            }
        }
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

/* Places every insec belonging to output section `os`, recording the
 * group's [start,end) bounds. Advances *va. */
static void place_osec(struct linker *l, int os, Elf64_Addr *va,
                       struct osec_bound *b)
{
    b[os].start = *va;
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->osec != os)
            continue;
        *va = align_up(*va, s->align);
        s->vaddr = *va;
        *va += s->size;
    }
    b[os].end = *va;
}

/* Lay the output sections out in order into the two segments, recording
 * each group's bounds. COMMON (tentative) symbols are placed at the end
 * of .bss, since they have no input section of their own. */
static void layout(struct linker *l, struct osec_bound *b,
                   Elf64_Addr *text_start, Elf64_Xword *text_size,
                   Elf64_Addr *data_start, Elf64_Xword *data_filesz,
                   Elf64_Xword *data_memsz)
{
    Elf64_Addr va = l->base;

    *text_start = va;
    place_osec(l, OSEC_TEXT, &va, b);
    place_osec(l, OSEC_RODATA, &va, b);
    *text_size = va - *text_start;

    va = align_up(va, PAGE);           /* W^X boundary */
    *data_start = va;
    place_osec(l, OSEC_INIT_ARRAY, &va, b);
    place_osec(l, OSEC_FINI_ARRAY, &va, b);
    place_osec(l, OSEC_CTORS, &va, b);
    place_osec(l, OSEC_DTORS, &va, b);
    place_osec(l, OSEC_DATA, &va, b);
    *data_filesz = va - *data_start;   /* .bss is beyond the file image */

    b[OSEC_BSS].start = va;
    place_osec(l, OSEC_BSS, &va, b);   /* real .bss inputs first */
    /* then COMMON: each tentative symbol gets space, largest alignment
     * honored, and its value fixed to the reserved slot. */
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!g->common)
            continue;
        va = align_up(va, g->align ? g->align : 1);
        g->value = va;
        g->insec = -1;                 /* now an absolute address */
        g->common = 0;
        va += g->size;
    }
    b[OSEC_BSS].end = va;
    *data_memsz = va - *data_start;
}

/* Turn every section-relative symbol value into a final absolute vaddr,
 * now that every section has one. */
static void finalize_symbols(struct linker *l)
{
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!g->defined || g->insec < 0)
            continue; /* insec == -1: ABS or already-placed COMMON */
        g->value += l->insecs[g->insec].vaddr;
    }
}

/* Define (or override a weak-undefined) linker symbol at an absolute
 * address. The __init_array_start/_end family are weak-undefined in
 * crt0 (which is why B1 linked with them at 0); defining them at the
 * real group bounds is what makes a program WITH constructors correct,
 * not just one whose .init_array happens to be empty. */
static void define_linker_symbol(struct linker *l, const char *name,
                                 Elf64_Addr value)
{
    struct symbol *g = sym_intern(l, name);
    /* only define it if something references it (it was interned) and it
     * is not already defined by a real object — a real definition wins */
    if (g->defined && g->insec >= 0)
        return;
    g->defined = 1;
    g->weak = 0;
    g->common = 0;
    g->insec = -1;
    g->value = value;
}

/* The bracket symbols crt0 walks, each pair the bounds of its group. An
 * empty group has start == end, so the walk does nothing — matching
 * cross-ld, which defines them even when empty. */
static void define_brackets(struct linker *l, const struct osec_bound *b)
{
    define_linker_symbol(l, "__init_array_start", b[OSEC_INIT_ARRAY].start);
    define_linker_symbol(l, "__init_array_end", b[OSEC_INIT_ARRAY].end);
    define_linker_symbol(l, "__fini_array_start", b[OSEC_FINI_ARRAY].start);
    define_linker_symbol(l, "__fini_array_end", b[OSEC_FINI_ARRAY].end);
    define_linker_symbol(l, "__ctors_start", b[OSEC_CTORS].start);
    define_linker_symbol(l, "__ctors_end", b[OSEC_CTORS].end);
    define_linker_symbol(l, "__dtors_start", b[OSEC_DTORS].start);
    define_linker_symbol(l, "__dtors_end", b[OSEC_DTORS].end);
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

/* Emit a native EMBX binary (EMBX_Specification_v2.md), the container ELF
 * cannot carry: it declares this program's required capabilities in a table
 * the kernel loader checks against the spawner's set (§6 step 9). Same two
 * W^X segments write_exec lays out — text R+X, data R+W — wrapped in EMBX's
 * header/segment/capability tables instead of an ELF ehdr+phdrs.
 *
 * Byte layout mirrors tools/embx/mkembx.py exactly so host and on-OS producers
 * agree: header(128) | 2 segment descriptors(64 each) | cap table(16 each) |
 * segment payloads at file offsets congruent to their vaddr mod align. */
static void emit_embx(struct linker *l, const char *out, unsigned long long caps,
                      Elf64_Addr entry,
                      Elf64_Addr text_start, Elf64_Xword text_size,
                      Elf64_Addr data_start, Elf64_Xword data_filesz,
                      Elf64_Xword data_memsz)
{
    /* Capability list: sorted ascending is free — walk cap_id low to high. */
    int ncaps = 0;
    for (int id = 1; id <= EMBX_CAP_MAX; id++)
        if (caps & (1ULL << id)) ncaps++;

    embx_u32 seg_tab_off = EMBX_HDR_SIZE;                    /* 128 */
    embx_u32 cap_tab_off = ncaps ? seg_tab_off + 2 * EMBX_SEG_SIZE : 0;
    embx_u32 tables_end  = seg_tab_off + 2 * EMBX_SEG_SIZE + (embx_u32)ncaps * EMBX_CAP_SIZE;

    /* Payload offsets: file_offset ≡ vaddr (mod PAGE), never before `cur`
     * (mkembx's congruent_offset; PAGE is a power of two). */
    embx_u64 text_fo = tables_end + (((embx_u64)text_start - tables_end) & (PAGE - 1));
    embx_u64 data_fo = (text_fo + text_size) + (((embx_u64)data_start - (text_fo + text_size)) & (PAGE - 1));
    embx_u64 image_size = data_fo + data_filesz;

    unsigned char *img = xcalloc(1, (size_t)image_size);

    /* --- section payloads: the same copy loop write_exec uses --- */
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->is_bss || !s->data) continue;
        embx_u64 base  = (s->seg == SEG_TEXT) ? text_fo : data_fo;
        Elf64_Addr segva = (s->seg == SEG_TEXT) ? text_start : data_start;
        memcpy(img + base + (s->vaddr - segva), s->data, (size_t)s->size);
    }

    /* --- header --- */
    struct embx_header *h = (struct embx_header *)img;
    const embx_u8 magic[8] = EMBX_MAGIC_BYTES;
    memcpy(h->magic, magic, 8);
    h->version_major = 1; h->version_minor = 0;
    h->header_size = EMBX_HDR_SIZE;
    h->binary_type = EMBX_TYPE_APP;
    h->machine = EMBX_MACHINE_X86_64;
    h->abi_version = EMBX_ABI_VERSION;
    h->flags = 0; h->feature_incompat = 0; h->feature_compat = 0;
    h->entry_point = entry;
    h->segment_table_offset = seg_tab_off;
    h->segment_count = 2;
    h->segment_entry_size = EMBX_SEG_SIZE;
    h->capability_table_offset = cap_tab_off;
    h->capability_count = (embx_u16)ncaps;
    h->capability_entry_size = EMBX_CAP_SIZE;
    h->grantor = 0; h->reserved0 = 0;
    h->image_size = image_size; h->reserved1 = 0;
    h->header_checksum = 0;                 /* filled last */

    /* --- segment descriptors: text R+X, data R+W (already W^X clean) --- */
    struct embx_segment *seg = (struct embx_segment *)(img + seg_tab_off);
    seg[0].type = EMBX_SEG_LOAD; seg[0].flags = EMBX_SEG_R | EMBX_SEG_X;
    seg[0].vaddr = text_start; seg[0].file_offset = text_fo;
    seg[0].file_size = text_size; seg[0].mem_size = text_size;
    seg[0].align = PAGE; seg[0].reserved0 = 0; seg[0].paddr = 0;
    seg[0].checksum = text_size ? embx_crc32c(img + text_fo, text_size) : 0;

    seg[1].type = EMBX_SEG_LOAD; seg[1].flags = EMBX_SEG_R | EMBX_SEG_W;
    seg[1].vaddr = data_start; seg[1].file_offset = data_fo;
    seg[1].file_size = data_filesz; seg[1].mem_size = data_memsz;
    seg[1].align = PAGE; seg[1].reserved0 = 0; seg[1].paddr = 0;
    seg[1].checksum = data_filesz ? embx_crc32c(img + data_fo, data_filesz) : 0;

    /* --- capability table (ascending, unique by construction) --- */
    if (ncaps) {
        struct embx_capability *ct = (struct embx_capability *)(img + cap_tab_off);
        int ci = 0;
        for (int id = 1; id <= EMBX_CAP_MAX; id++)
            if (caps & (1ULL << id)) {
                ct[ci].cap_id = (embx_u32)id;
                ct[ci].cap_flags = 0; ct[ci].reserved0 = 0;
                ci++;
            }
    }

    /* --- checksum order (§3.4): build_id over the whole image with build_id
     * and header_checksum still zero, then the header CRC32C last. --- */
    embdbg_sha256(img, (long)image_size, h->build_id);
    h->header_checksum = embx_crc32c(img, EMBX_HDR_BODY_SIZE);

    FILE *f = fopen(out, "wb");
    if (!f) die("cannot open '%s' for writing", out);
    if (fwrite(img, 1, (size_t)image_size, f) != (size_t)image_size)
        die("write error on '%s'", out);
    fclose(f);
    free(img);
    fprintf(stderr, "embld: wrote %s (EMBX, %d capabilit%s)\n",
            out, ncaps, ncaps == 1 ? "y" : "ies");
}

static unsigned char *read_file(const char *path, long *len);

/* Emit a native .embdbg sidecar for a debug-carrying input, now that layout
 * has assigned final vaddrs. This is the link-time producer the format wants
 * (EMBDBG spec §2/§3: absolute vaddrs, build_id bound to the image). EmbCC
 * emits ET_REL objects with .text-relative debug addresses; the link is what
 * makes them absolute, so this belongs HERE, not in the compiler.
 *
 * Every directly-listed input object that carries .debug_line is merged into
 * one .embdbg, each biased by its own final .text vaddr. Archive members are
 * skipped — newlib's libc.a ships with debug info, but the intent of a -g link
 * is to debug YOUR objects, not incidentally-pulled libc members (this also
 * keeps a link with no -g object, like self-host, from emitting a sidecar). */
static void emit_embdbg(struct linker *l, const char *out)
{
    const unsigned char **objs = xmalloc((size_t)(l->nobj ? l->nobj : 1) * sizeof *objs);
    long *lens = xmalloc((size_t)(l->nobj ? l->nobj : 1) * sizeof *lens);
    long *biases = xmalloc((size_t)(l->nobj ? l->nobj : 1) * sizeof *biases);
    int n = 0;

    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        if (strchr(o->name, '('))       /* "libc.a(member.o)" — an archive member */
            continue;
        int has_dbg = 0, text_idx = -1;
        for (int s = 0; s < o->nsh; s++) {
            const char *nm = o->shstr + o->shdrs[s].sh_name;
            if (strcmp(nm, ".debug_line") == 0) has_dbg = 1;
            if (strcmp(nm, ".text") == 0) text_idx = s;
        }
        if (!has_dbg) continue;
        Elf64_Addr tv = 0;
        if (text_idx >= 0 && o->sec_out[text_idx] >= 0)
            tv = l->insecs[o->sec_out[text_idx]].vaddr;
        objs[n] = o->buf; lens[n] = o->len; biases[n] = (long)tv;
        n++;
    }

    if (n) {
        long ilen;
        unsigned char *img = read_file(out, &ilen);
        char emb[4096];
        snprintf(emb, sizeof emb, "%s.embdbg", out);
        embdbg_emit_objects(objs, lens, biases, n, img, ilen, emb);
        free(img);
        fprintf(stderr, "embld: wrote %s (debug info from %d object%s)\n",
                emb, n, n == 1 ? "" : "s");
    }
    free(objs); free(lens); free(biases);
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

    /* Explicit objects are always linked; archives are stashed and their
     * members pulled on demand (left-to-right, as a linker does — so an
     * archive satisfies references that appear before it on the line). */
    for (int i = 0; i < ninputs; i++) {
        long len;
        unsigned char *buf = read_file(inputs[i], &len);
        if (is_archive(buf, len)) {
            parse_archive(&l, inputs[i], buf, len);
            pull_archives(&l); /* satisfy what is undefined so far */
        } else {
            add_object(&l, parse_object(inputs[i], buf, len));
        }
    }
    /* A final fixed-point pass, so a later object's references can still
     * reach back into an earlier archive (the --start-group behaviour,
     * always on: correct over order-sensitive). */
    pull_archives(&l);

    Elf64_Addr text_start, data_start;
    Elf64_Xword text_size, data_filesz, data_memsz;
    struct osec_bound bounds[OSEC_COUNT];
    memset(bounds, 0, sizeof bounds);
    layout(&l, bounds, &text_start, &text_size, &data_start, &data_filesz,
           &data_memsz);
    finalize_symbols(&l);
    define_brackets(&l, bounds);

    struct symbol *e = sym_find(&l, l.entry);
    if (!e || !e->defined)
        die("entry symbol '%s' is undefined", l.entry);

    for (int i = 0; i < l.nobj; i++)
        apply_relocs(&l, l.objs[i]);

    if (opts && opts->emit_embx) {
        emit_embx(&l, out, opts->caps, e->value, text_start, text_size,
                  data_start, data_filesz, data_memsz);
    } else {
        write_exec(&l, out, e->value, text_start, text_size,
                   data_start, data_filesz, data_memsz);
        emit_embdbg(&l, out);   /* a .embdbg sidecar if any input carries -g info */
    }
    return 0;
}
