/* ELF64 structures and constants shared by asm and link (ARCHITECTURE.md §2).
 *
 * Defined here rather than pulled from the host's <elf.h>: EmbCC must
 * eventually compile itself (ARCHITECTURE.md §7), so its own source cannot
 * lean on headers the target does not ship. Only what EmbCC actually emits
 * is defined — this file grows with the writer, it is not a mirror of the
 * spec.
 *
 * Layouts follow the System V gABI, ELF64, little-endian (the only target,
 * TARGET_ABI.md).
 */
#ifndef EMBCC_ELF_ELF_H
#define EMBCC_ELF_ELF_H

typedef unsigned char      Elf64_Uchar;
typedef unsigned short     Elf64_Half;
typedef unsigned int       Elf64_Word;
typedef unsigned long long Elf64_Xword;
typedef unsigned long long Elf64_Addr;
typedef unsigned long long Elf64_Off;

#define EI_NIDENT 16

typedef struct {
    Elf64_Uchar e_ident[EI_NIDENT];
    Elf64_Half  e_type;
    Elf64_Half  e_machine;
    Elf64_Word  e_version;
    Elf64_Addr  e_entry;
    Elf64_Off   e_phoff;
    Elf64_Off   e_shoff;
    Elf64_Word  e_flags;
    Elf64_Half  e_ehsize;
    Elf64_Half  e_phentsize;
    Elf64_Half  e_phnum;
    Elf64_Half  e_shentsize;
    Elf64_Half  e_shnum;
    Elf64_Half  e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Word  st_name;
    Elf64_Uchar st_info;
    Elf64_Uchar st_other;
    Elf64_Half  st_shndx;
    Elf64_Addr  st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr  r_offset;
    Elf64_Xword r_info;
    long long   r_addend;
} Elf64_Rela;

/* Program header — the linker (M3) writes these; the compiler's ET_REL
 * output has none. */
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

#define ELF64_R_INFO(sym, type) \
    (((Elf64_Xword)(sym) << 32) | ((Elf64_Xword)(type) & 0xffffffff))
#define ELF64_R_SYM(info)  ((Elf64_Word)((info) >> 32))
#define ELF64_R_TYPE(info) ((Elf64_Word)((info) & 0xffffffff))
#define ELF64_ST_BIND(info) ((info) >> 4)
#define ELF64_ST_TYPE(info) ((info) & 0xf)

/* Relocation types. R_X86_64_PLT32 is what gas/gcc emit for a call to a
 * global; TARGET_ABI §4a records the expensive fact that a static link
 * must treat it as a plain PC32 — the EmbLinkOS linker (TCC patch 0001)
 * and the cross ld both do. GOTPCREL/its relaxable forms reach data
 * through the GOT (newlib's _impure_ptr — TCC patch 0003, the static
 * GOT must be built AND filled). */
#define R_X86_64_64            1
#define R_X86_64_PC32          2
#define R_X86_64_PLT32         4
#define R_X86_64_GOTPCREL      9
#define R_X86_64_32           10
#define R_X86_64_32S          11
#define R_X86_64_PC64         24
#define R_X86_64_GOTPCRELX    41
#define R_X86_64_REX_GOTPCRELX 42

/* aarch64 (ELF for the Arm 64-bit Architecture, §4.6.3). Only the four
 * EmbCC emits are named: a `bl`'s 26-bit branch, the adrp/add pair that
 * materialises a symbol's address, and an absolute 64-bit data slot. */
#define R_AARCH64_ABS64              257
#define R_AARCH64_ADR_PREL_PG_HI21   275
#define R_AARCH64_ADD_ABS_LO12_NC    277
#define R_AARCH64_CALL26             283

/* e_ident indices and values */
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define ELFMAG0       0x7f
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

/* e_type — ET_REL until the integrated linker lands (ROADMAP M3);
 * ET_EXEC is listed because it is the only executable type the kernel
 * loader accepts (TARGET_ABI.md §4b: never ET_DYN/PIE). */
#define ET_REL        1
#define ET_EXEC       2

#define EM_X86_64     62
#define EM_AARCH64   183

/* p_type */
#define PT_NULL       0
#define PT_LOAD       1

/* p_flags */
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

/* sh_type */
#define SHT_NULL      0
#define SHT_PROGBITS  1
#define SHT_SYMTAB    2
#define SHT_STRTAB    3
#define SHT_RELA      4
#define SHT_NOBITS    8

/* sh_flags */
#define SHF_WRITE     0x1
#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_INFO_LINK 0x40

/* special section indices */
#define SHN_UNDEF     0
#define SHN_ABS       0xfff1
#define SHN_COMMON    0xfff2

/* symbol binding/type, packed into st_info */
#define STB_LOCAL     0
#define STB_GLOBAL    1
#define STB_WEAK      2
#define STT_NOTYPE    0
#define STT_OBJECT    1
#define STT_FUNC      2
#define STT_SECTION   3
#define STT_FILE      4
#define ELF64_ST_INFO(bind, type) ((Elf64_Uchar)(((bind) << 4) | ((type) & 0xf)))

#endif
