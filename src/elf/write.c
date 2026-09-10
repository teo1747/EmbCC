#include "write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Growable byte buffer, used for section payloads and string tables. */
struct buf {
    unsigned char *p;
    size_t len, cap;
};

static void buf_append(struct buf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n)
            cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p) {
            fprintf(stderr, "embcc: out of memory\n");
            exit(1);
        }
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

/* Appends s (with its NUL) and returns its offset — the strtab primitive. */
static Elf64_Word strtab_add(struct buf *b, const char *s)
{
    Elf64_Word off = (Elf64_Word)b->len;
    buf_append(b, s, strlen(s) + 1);
    return off;
}

struct section {
    Elf64_Shdr hdr;
    struct buf data;
};

#define ELFW_MAX_SECTIONS 32
#define ELFW_MAX_RELA 6      /* .text, .data, and -g's .debug_info/.debug_line */

/* Relocations are grouped by the section they apply to; each group emits
 * its own .rela.<name> section. One group (.text) is the common case; a
 * global initializer pointing into .rodata adds a .data group. */
struct rela_group {
    int target;          /* section index the relocations apply to */
    struct buf rela;     /* array of Elf64_Rela */
    int nrela;
    int sec_ndx;         /* the .rela.<name> section, set at write time */
};

struct elfw {
    struct section sec[ELFW_MAX_SECTIONS];
    int nsec;
    struct buf symtab;   /* array of Elf64_Sym */
    int nsym;
    int nlocal;          /* symbols [0, nlocal) are STB_LOCAL */
    int globals_started; /* set once a non-local is added */
    struct buf strtab;   /* symbol names */
    struct buf shstrtab; /* section names */
    struct rela_group relagrp[ELFW_MAX_RELA];
    int nrelagrp;
    int machine;         /* e_machine, fixed at elfw_new */
};

struct elfw *elfw_new(int machine)
{
    struct elfw *w = calloc(1, sizeof *w);
    if (!w) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    w->machine = machine;
    /* Index 0 is reserved in every table it manages. */
    w->nsec = 1; /* SHT_NULL section */
    strtab_add(&w->strtab, "");
    strtab_add(&w->shstrtab, "");
    Elf64_Sym null_sym;
    memset(&null_sym, 0, sizeof null_sym);
    buf_append(&w->symtab, &null_sym, sizeof null_sym);
    w->nsym = 1;
    w->nlocal = 1;
    return w;
}

void elfw_free(struct elfw *w)
{
    if (!w)
        return;
    for (int i = 0; i < w->nsec; i++)
        free(w->sec[i].data.p);
    free(w->symtab.p);
    free(w->strtab.p);
    free(w->shstrtab.p);
    for (int i = 0; i < w->nrelagrp; i++)
        free(w->relagrp[i].rela.p);
    free(w);
}

int elfw_add_section(struct elfw *w, const char *name, Elf64_Word type,
                     Elf64_Xword flags, const void *data, Elf64_Xword size,
                     Elf64_Xword addralign)
{
    if (w->nsec >= ELFW_MAX_SECTIONS) {
        fprintf(stderr, "embcc: elf writer: section limit (%d) reached\n",
                ELFW_MAX_SECTIONS);
        exit(1);
    }
    struct section *s = &w->sec[w->nsec];
    memset(s, 0, sizeof *s);
    s->hdr.sh_name = strtab_add(&w->shstrtab, name);
    s->hdr.sh_type = type;
    s->hdr.sh_flags = flags;
    s->hdr.sh_size = size;
    s->hdr.sh_addralign = addralign;
    if (size && type != SHT_NOBITS)
        buf_append(&s->data, data, (size_t)size);
    return w->nsec++;
}

int elfw_add_symbol(struct elfw *w, const char *name, Elf64_Addr value,
                    Elf64_Xword size, Elf64_Uchar info, Elf64_Half shndx)
{
    int is_local = (info >> 4) == STB_LOCAL;
    if (is_local && w->globals_started) {
        /* The gABI requires locals before globals (sh_info is the split
         * point). Refuse loudly instead of reordering behind the caller. */
        fprintf(stderr,
                "embcc: elf writer: local symbol '%s' added after globals\n",
                name);
        exit(1);
    }
    if (!is_local)
        w->globals_started = 1;
    else
        w->nlocal++;

    Elf64_Sym sym;
    memset(&sym, 0, sizeof sym);
    sym.st_name = name && *name ? strtab_add(&w->strtab, name) : 0;
    sym.st_info = info;
    sym.st_shndx = shndx;
    sym.st_value = value;
    sym.st_size = size;
    buf_append(&w->symtab, &sym, sizeof sym);
    return w->nsym++;
}

static Elf64_Off align_up(Elf64_Off off, Elf64_Xword align)
{
    if (align < 2)
        return off;
    return (off + align - 1) & ~(align - 1);
}

void elfw_add_rela(struct elfw *w, int target_ndx, Elf64_Addr offset,
                   int sym, int type, long addend)
{
    /* Route to the group for this target section, creating it on first
     * use. Grouping keeps each .rela.<name> pointing at exactly one
     * section, as the gABI requires. */
    struct rela_group *gp = NULL;
    for (int i = 0; i < w->nrelagrp; i++)
        if (w->relagrp[i].target == target_ndx) {
            gp = &w->relagrp[i];
            break;
        }
    if (!gp) {
        if (w->nrelagrp >= ELFW_MAX_RELA) {
            fprintf(stderr, "embcc: elf writer: too many relocation "
                            "target sections (max %d)\n", ELFW_MAX_RELA);
            exit(1);
        }
        gp = &w->relagrp[w->nrelagrp++];
        memset(gp, 0, sizeof *gp);
        gp->target = target_ndx;
    }
    Elf64_Rela r;
    r.r_offset = offset;
    r.r_info = ELF64_R_INFO((Elf64_Xword)sym, (Elf64_Xword)type);
    r.r_addend = addend;
    buf_append(&gp->rela, &r, sizeof r);
    gp->nrela++;
}

int elfw_write(struct elfw *w, const char *path)
{
    /* Materialize the bookkeeping sections after the user's: one
     * .rela.<target> per relocation group (sh_link/sh_info patched below
     * once the symtab index exists), then .symtab/.strtab/.shstrtab. The
     * name is ".rela" + the target's own name (".rela.text", ".rela.data"). */
    for (int i = 0; i < w->nrelagrp; i++) {
        struct rela_group *gp = &w->relagrp[i];
        const char *tname = (const char *)w->shstrtab.p +
                            w->sec[gp->target].hdr.sh_name;
        char rname[64];
        snprintf(rname, sizeof rname, ".rela%s", tname);
        gp->sec_ndx = elfw_add_section(w, rname, SHT_RELA, SHF_INFO_LINK,
                                       gp->rela.p, gp->rela.len, 8);
    }
    int symtab_ndx = elfw_add_section(w, ".symtab", SHT_SYMTAB, 0,
                                      w->symtab.p, w->symtab.len, 8);
    int strtab_ndx = elfw_add_section(w, ".strtab", SHT_STRTAB, 0,
                                      w->strtab.p, w->strtab.len, 1);
    /* .shstrtab names itself, so its payload is copied only after
     * strtab_add has run for it. */
    int shstr_ndx = w->nsec;
    Elf64_Word shstr_name = strtab_add(&w->shstrtab, ".shstrtab");
    struct section *shstr = &w->sec[w->nsec++];
    memset(shstr, 0, sizeof *shstr);
    shstr->hdr.sh_name = shstr_name;
    shstr->hdr.sh_type = SHT_STRTAB;
    shstr->hdr.sh_size = w->shstrtab.len;
    shstr->hdr.sh_addralign = 1;
    buf_append(&shstr->data, w->shstrtab.p, w->shstrtab.len);

    w->sec[symtab_ndx].hdr.sh_link = (Elf64_Word)strtab_ndx;
    w->sec[symtab_ndx].hdr.sh_info = (Elf64_Word)w->nlocal;
    w->sec[symtab_ndx].hdr.sh_entsize = sizeof(Elf64_Sym);
    for (int i = 0; i < w->nrelagrp; i++) {
        struct rela_group *gp = &w->relagrp[i];
        w->sec[gp->sec_ndx].hdr.sh_link = (Elf64_Word)symtab_ndx;
        w->sec[gp->sec_ndx].hdr.sh_info = (Elf64_Word)gp->target;
        w->sec[gp->sec_ndx].hdr.sh_entsize = sizeof(Elf64_Rela);
    }

    /* Lay out: ehdr, section payloads, then the section header table. */
    Elf64_Off off = sizeof(Elf64_Ehdr);
    for (int i = 1; i < w->nsec; i++) {
        struct section *s = &w->sec[i];
        off = align_up(off, s->hdr.sh_addralign);
        s->hdr.sh_offset = off;
        if (s->hdr.sh_type != SHT_NOBITS)
            off += s->hdr.sh_size;
    }
    Elf64_Off shoff = align_up(off, 8);

    Elf64_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    eh.e_ident[EI_MAG0] = ELFMAG0;
    eh.e_ident[EI_MAG1] = ELFMAG1;
    eh.e_ident[EI_MAG2] = ELFMAG2;
    eh.e_ident[EI_MAG3] = ELFMAG3;
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_REL;
    eh.e_machine = (Elf64_Half)w->machine;
    eh.e_version = EV_CURRENT;
    eh.e_shoff = shoff;
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = (Elf64_Half)w->nsec;
    eh.e_shstrndx = (Elf64_Half)shstr_ndx;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "embcc: cannot open '%s' for writing\n", path);
        return -1;
    }
    fwrite(&eh, sizeof eh, 1, f);
    Elf64_Off pos = sizeof eh;
    for (int i = 1; i < w->nsec; i++) {
        struct section *s = &w->sec[i];
        if (s->hdr.sh_type == SHT_NOBITS)
            continue;
        while (pos < s->hdr.sh_offset) {
            fputc(0, f);
            pos++;
        }
        fwrite(s->data.p, 1, s->data.len, f);
        pos += s->data.len;
    }
    while (pos < shoff) {
        fputc(0, f);
        pos++;
    }
    for (int i = 0; i < w->nsec; i++)
        fwrite(&w->sec[i].hdr, sizeof(Elf64_Shdr), 1, f);
    if (fclose(f) != 0) {
        fprintf(stderr, "embcc: write error on '%s'\n", path);
        return -1;
    }
    return 0;
}
