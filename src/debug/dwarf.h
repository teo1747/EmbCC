/* Minimal DWARF line-info emitter (D-010 step 1: "where am I?" —
 * address <-> source file:line, the cheapest debug layer that buys the
 * largest fraction of a debugger's usefulness).
 *
 * Emits three sections from the codegen line table (ir_func.lines):
 *   .debug_abbrev  one compile_unit abbreviation
 *   .debug_info    one CU DIE (producer/name/comp_dir/low_pc/high_pc/stmt_list)
 *   .debug_line    a DWARF-4 line-number program, one row where the line
 *                  changes, bracketed per function by set_address/end_sequence
 * DWARF version 4, 32-bit format, address_size 8. No .debug_info children
 * (subprogram/local DIEs are step 2); backtraces still name functions from
 * the ELF .symtab, and line breakpoints need only the line table + CU.
 *
 * The addresses are RELOCATED: the object is ET_REL, so every field holding
 * a .text address (line-program set_address, CU low/high_pc) and every field
 * holding a debug-section offset (CU abbrev_offset, stmt_list) is written as
 * zero and paired with a relocation the driver hands to the ELF writer. The
 * emitter has no ELF dependency — it returns the reloc list; the driver maps
 * targets to section symbols. */
#ifndef EMBCC_DEBUG_DWARF_H
#define EMBCC_DEBUG_DWARF_H

#include "../ir/ir.h"

/* The debug sections the emitter fills, indexing dwarf_out.sec[]. The count
 * is a macro, not an enum terminator: EmbCC's own subset (which must compile
 * this file for self-hosting) folds only integer literals, not enum
 * constants, in an array bound. */
#define DWARF_NSEC 3
enum { DWSEC_ABBREV, DWSEC_INFO, DWSEC_LINE };

/* What a relocation binds against — the driver resolves each to the matching
 * section symbol (STT_SECTION). DWTGT_TEXT is the code the addresses point
 * into; the rest are self-references between debug sections. */
enum { DWTGT_TEXT, DWTGT_ABBREV, DWTGT_LINE };

struct dwarf_reloc {
    int in_sec;      /* DWSEC_* the field lives in */
    int off;         /* byte offset of the field within that section */
    int width;       /* 4 or 8 — selects R_X86_64_32 vs R_X86_64_64 */
    int target;      /* DWTGT_* — which section symbol to bind against */
    long addend;
};

struct dwarf_out {
    unsigned char *sec[DWARF_NSEC];
    int seclen[DWARF_NSEC];
    struct dwarf_reloc *relocs;
    int nrelocs, reloccap;
};

/* Build the debug sections for iu into out (caller zero-inits out). filename
 * is written verbatim as DW_AT_name — it is what a debugger uses to locate
 * source, so it is the path as the user gave it on the command line.
 * Deterministic: no timestamps, no getcwd (comp_dir is a fixed "."), per the
 * reproducibility rule debug output must not break. */
void dwarf_emit(struct ir_unit *iu, const char *filename,
                struct dwarf_out *out);
void dwarf_free(struct dwarf_out *out);

#endif
