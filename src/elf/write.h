/* Relocatable-object writer (skeleton, ROADMAP M0).
 *
 * The interface codegen/asm will feed at M1: create a writer, append
 * sections and symbols, write an ET_REL file. What it cannot do yet it
 * does not pretend to do — there is no relocation support and no
 * executable output; both are M1/M3 work and calls that would need them
 * do not exist here.
 */
#ifndef EMBCC_ELF_WRITE_H
#define EMBCC_ELF_WRITE_H

#include "elf.h"

struct elfw;

struct elfw *elfw_new(void);
void elfw_free(struct elfw *w);

/* Returns the section header index, for use as a symbol's st_shndx.
 * data may be NULL when size is 0. */
int elfw_add_section(struct elfw *w, const char *name, Elf64_Word type,
                     Elf64_Xword flags, const void *data, Elf64_Xword size,
                     Elf64_Xword addralign);

/* Returns the symbol table index. shndx is an index from elfw_add_section
 * (or SHN_UNDEF/SHN_ABS). Local symbols must be added before globals —
 * the gABI orders them that way, and the writer refuses rather than
 * silently reordering. */
int elfw_add_symbol(struct elfw *w, const char *name, Elf64_Addr value,
                    Elf64_Xword size, Elf64_Uchar info, Elf64_Half shndx);

/* Writes the ET_REL/EM_X86_64 object. Returns 0, or -1 with a message on
 * stderr. Always emits .symtab/.strtab/.shstrtab after the user sections. */
int elfw_write(struct elfw *w, const char *path);

#endif
