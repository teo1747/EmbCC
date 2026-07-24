#include "dwarf.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

/* --- DWARF constants (only the handful this emitter uses) --- */
#define DW_TAG_compile_unit   0x11
#define DW_CHILDREN_no        0x00
#define DW_AT_name            0x03
#define DW_AT_stmt_list       0x10
#define DW_AT_low_pc          0x11
#define DW_AT_high_pc         0x12
#define DW_AT_language        0x13
#define DW_AT_producer        0x25
#define DW_AT_comp_dir        0x1b
#define DW_FORM_addr          0x01
#define DW_FORM_data2         0x05
#define DW_FORM_data4         0x06
#define DW_FORM_string        0x08
#define DW_FORM_sec_offset    0x17
#define DW_LANG_C99           0x000c

/* Line-program standard opcodes */
#define DW_LNS_copy           0x01
#define DW_LNS_advance_pc     0x02
#define DW_LNS_advance_line   0x03
/* Extended opcodes */
#define DW_LNE_end_sequence   0x01
#define DW_LNE_set_address    0x02

/* line-program special-opcode parameters (we do NOT use special opcodes —
 * advance_line + advance_pc + copy is enough and always correct — but the
 * header must still declare them consistently for a reader). */
#define LINE_BASE   (-5)
#define LINE_RANGE  14
#define OPCODE_BASE 13

/* --- a growable byte buffer, one per section --- */
struct dbuf { unsigned char *p; int len, cap; };

static void db_need(struct dbuf *b, int n)
{
    if (b->len + n > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        while (b->len + n > b->cap) b->cap *= 2;
        b->p = xrealloc(b->p, (size_t)b->cap);
    }
}
static void db_u8(struct dbuf *b, unsigned v)
{
    db_need(b, 1);
    b->p[b->len++] = (unsigned char)(v & 0xff);
}
static void db_u16(struct dbuf *b, unsigned v)
{
    db_u8(b, v); db_u8(b, v >> 8);
}
static void db_u32(struct dbuf *b, unsigned long v)
{
    db_u8(b, (unsigned)v); db_u8(b, (unsigned)(v >> 8));
    db_u8(b, (unsigned)(v >> 16)); db_u8(b, (unsigned)(v >> 24));
}
static void db_u64(struct dbuf *b, unsigned long v)
{
    /* `long` is 64-bit on x86_64-elf (and on the host), so a single unsigned
     * long spans the field — EmbCC's subset has no `long long`. */
    db_u32(b, v & 0xffffffffUL);
    db_u32(b, (v >> 32) & 0xffffffffUL);
}
static void db_str(struct dbuf *b, const char *s)
{
    do { db_u8(b, (unsigned char)*s); } while (*s++);
}
static void db_uleb(struct dbuf *b, unsigned long v)
{
    do {
        unsigned char byte = v & 0x7f;
        v >>= 7;
        if (v) byte |= 0x80;
        db_u8(b, byte);
    } while (v);
}
static void db_sleb(struct dbuf *b, long v)
{
    int more = 1;
    while (more) {
        unsigned char byte = (unsigned char)(v & 0x7f);
        v >>= 7;                       /* arithmetic shift (sign-propagating) */
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
            more = 0;
        else
            byte |= 0x80;
        db_u8(b, byte);
    }
}

/* Record a relocation against a to-be-written zero field at the CURRENT end
 * of section `in_sec` (call immediately before writing the field's zeros). */
static void reloc(struct dwarf_out *out, int in_sec, int at_off, int width,
                  int target, long addend)
{
    if (out->nrelocs == out->reloccap) {
        out->reloccap = out->reloccap ? out->reloccap * 2 : 8;
        out->relocs = xrealloc(out->relocs,
                               (size_t)out->reloccap * sizeof *out->relocs);
    }
    struct dwarf_reloc *r = &out->relocs[out->nrelocs++];
    r->in_sec = in_sec;
    r->off = at_off;
    r->width = width;
    r->target = target;
    r->addend = addend;
}

/* --- .debug_abbrev: a single compile_unit abbreviation (code 1) --- */
static void emit_abbrev(struct dbuf *b)
{
    db_uleb(b, 1);                       /* abbrev code */
    db_uleb(b, DW_TAG_compile_unit);
    db_u8(b, DW_CHILDREN_no);
    db_uleb(b, DW_AT_producer);  db_uleb(b, DW_FORM_string);
    db_uleb(b, DW_AT_language);  db_uleb(b, DW_FORM_data2);
    db_uleb(b, DW_AT_name);      db_uleb(b, DW_FORM_string);
    db_uleb(b, DW_AT_comp_dir);  db_uleb(b, DW_FORM_string);
    db_uleb(b, DW_AT_low_pc);    db_uleb(b, DW_FORM_addr);
    db_uleb(b, DW_AT_high_pc);   db_uleb(b, DW_FORM_addr);
    db_uleb(b, DW_AT_stmt_list); db_uleb(b, DW_FORM_sec_offset);
    db_uleb(b, 0); db_uleb(b, 0);        /* end of this abbrev's attrs */
    db_uleb(b, 0);                       /* end of the abbrev table */
}

/* --- .debug_info: one CU DIE. low_pc/high_pc bound the whole code range. --- */
static void emit_info(struct dwarf_out *out, struct dbuf *b,
                      const char *filename, long text_lo, long text_hi)
{
    int len_at = b->len;
    db_u32(b, 0);                        /* unit_length — backpatched */
    int after_len = b->len;
    db_u16(b, 4);                        /* version */
    reloc(out, DWSEC_INFO, b->len, 4, DWTGT_ABBREV, 0);
    db_u32(b, 0);                        /* debug_abbrev_offset (reloc) */
    db_u8(b, 8);                         /* address_size */

    db_uleb(b, 1);                       /* abbrev code -> compile_unit */
    db_str(b, "EmbCC");                  /* DW_AT_producer */
    db_u16(b, DW_LANG_C99);              /* DW_AT_language */
    db_str(b, filename);                 /* DW_AT_name */
    db_str(b, ".");                      /* DW_AT_comp_dir (deterministic) */
    reloc(out, DWSEC_INFO, b->len, 8, DWTGT_TEXT, text_lo);
    db_u64(b, 0);                        /* DW_AT_low_pc (reloc) */
    reloc(out, DWSEC_INFO, b->len, 8, DWTGT_TEXT, text_hi);
    db_u64(b, 0);                        /* DW_AT_high_pc (reloc) */
    reloc(out, DWSEC_INFO, b->len, 4, DWTGT_LINE, 0);
    db_u32(b, 0);                        /* DW_AT_stmt_list (reloc) */

    unsigned long ulen = (unsigned long)(b->len - after_len);
    b->p[len_at + 0] = (unsigned char)ulen;
    b->p[len_at + 1] = (unsigned char)(ulen >> 8);
    b->p[len_at + 2] = (unsigned char)(ulen >> 16);
    b->p[len_at + 3] = (unsigned char)(ulen >> 24);
}

/* One function's rows, bracketed by set_address .. end_sequence. Offsets are
 * .text-relative (row.off and code_off share that space), so pc deltas need
 * no knowledge of the final load address. */
static void emit_line_func(struct dwarf_out *out, struct dbuf *b,
                           struct ir_func *fn)
{
    long lo = fn->src->code_off;
    long hi = fn->src->code_off + fn->src->code_len;

    /* DW_LNE_set_address <8-byte .text address, relocated> */
    db_u8(b, 0); db_uleb(b, 9); db_u8(b, DW_LNE_set_address);
    reloc(out, DWSEC_LINE, b->len, 8, DWTGT_TEXT, lo);
    db_u64(b, 0);

    long cur_addr = lo, cur_line = 1;
    for (int r = 0; r < fn->nlines; r++) {
        long off = fn->lines[r].off, line = fn->lines[r].line;
        if (line != cur_line) {
            db_u8(b, DW_LNS_advance_line);
            db_sleb(b, line - cur_line);
            cur_line = line;
        }
        if (off != cur_addr) {
            db_u8(b, DW_LNS_advance_pc);
            db_uleb(b, (unsigned long)(off - cur_addr));
            cur_addr = off;
        }
        db_u8(b, DW_LNS_copy);           /* append a row at (cur_addr,cur_line) */
    }
    /* advance to the function end and close the sequence */
    if (hi != cur_addr) {
        db_u8(b, DW_LNS_advance_pc);
        db_uleb(b, (unsigned long)(hi - cur_addr));
    }
    db_u8(b, 0); db_uleb(b, 1); db_u8(b, DW_LNE_end_sequence);
}

static void emit_line(struct dwarf_out *out, struct dbuf *b,
                      struct ir_unit *iu, const char *filename)
{
    int len_at = b->len;
    db_u32(b, 0);                        /* unit_length — backpatched */
    int after_len = b->len;
    db_u16(b, 4);                        /* version */
    int hdr_len_at = b->len;
    db_u32(b, 0);                        /* header_length — backpatched */
    int after_hdr_len = b->len;
    db_u8(b, 1);                         /* minimum_instruction_length */
    db_u8(b, 1);                         /* maximum_operations_per_instruction */
    db_u8(b, 1);                         /* default_is_stmt */
    db_u8(b, (unsigned char)LINE_BASE);  /* line_base (signed) */
    db_u8(b, LINE_RANGE);                /* line_range */
    db_u8(b, OPCODE_BASE);               /* opcode_base */
    /* standard_opcode_lengths for opcodes 1..12 */
    static const unsigned char oplen[12] =
        { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    for (int i = 0; i < 12; i++) db_u8(b, oplen[i]);
    db_u8(b, 0);                         /* include_directories: none */
    /* file_names: one entry, the source file */
    db_str(b, filename);
    db_uleb(b, 0);                       /* dir index */
    db_uleb(b, 0);                       /* mtime (deterministic) */
    db_uleb(b, 0);                       /* size */
    db_u8(b, 0);                         /* end of file_names */

    /* backpatch header_length (bytes from here to end of header) */
    unsigned long hlen = (unsigned long)(b->len - after_hdr_len);
    b->p[hdr_len_at + 0] = (unsigned char)hlen;
    b->p[hdr_len_at + 1] = (unsigned char)(hlen >> 8);
    b->p[hdr_len_at + 2] = (unsigned char)(hlen >> 16);
    b->p[hdr_len_at + 3] = (unsigned char)(hlen >> 24);

    for (int n = 0; n < iu->nfuncs; n++)
        if (iu->funcs[n].src->code_len > 0)
            emit_line_func(out, b, &iu->funcs[n]);

    unsigned long ulen = (unsigned long)(b->len - after_len);
    b->p[len_at + 0] = (unsigned char)ulen;
    b->p[len_at + 1] = (unsigned char)(ulen >> 8);
    b->p[len_at + 2] = (unsigned char)(ulen >> 16);
    b->p[len_at + 3] = (unsigned char)(ulen >> 24);
}

void dwarf_emit(struct ir_unit *iu, const char *filename,
                struct dwarf_out *out)
{
    /* Bound the code: low = first function's offset (0 in practice),
     * high = the end of the last function. Covers exactly the code that can
     * carry line rows; file-scope asm (no line info) beyond it is simply not
     * attributed to this CU, which is correct. */
    long lo = 0, hi = 0;
    int seen = 0;
    for (int n = 0; n < iu->nfuncs; n++) {
        if (iu->funcs[n].src->code_len <= 0) continue;
        long f_lo = iu->funcs[n].src->code_off;
        long f_hi = f_lo + iu->funcs[n].src->code_len;
        if (!seen || f_lo < lo) lo = f_lo;
        if (!seen || f_hi > hi) hi = f_hi;
        seen = 1;
    }

    struct dbuf ab = { 0, 0, 0 }, in = { 0, 0, 0 }, ln = { 0, 0, 0 };
    emit_abbrev(&ab);
    emit_info(out, &in, filename, lo, hi);
    emit_line(out, &ln, iu, filename);

    out->sec[DWSEC_ABBREV] = ab.p; out->seclen[DWSEC_ABBREV] = ab.len;
    out->sec[DWSEC_INFO]   = in.p; out->seclen[DWSEC_INFO]   = in.len;
    out->sec[DWSEC_LINE]   = ln.p; out->seclen[DWSEC_LINE]   = ln.len;
}

void dwarf_free(struct dwarf_out *out)
{
    for (int i = 0; i < DWARF_NSEC; i++) free(out->sec[i]);
    free(out->relocs);
}
