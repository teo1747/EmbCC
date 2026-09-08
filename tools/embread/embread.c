/* embread — dump and VERIFY an EMBX image.
 *
 * The one tool EMBX_Specification_v2 §9 sanctions today, for the reason
 * it gives: "a format that cannot be dumped cannot be debugged." It is
 * also the toolchain's verifier — it runs the §6 load sequence and the
 * §8 parse-time guards over a candidate image, on the host, so a
 * producer bug is found here instead of as a refusal from the kernel
 * with one error code and no detail.
 *
 * It reads the image the way the LOADER does, in §6 order, and refuses
 * in the same places: nothing later is trusted until the header
 * checksum passes, because that is the bootstrap step that makes every
 * offset in the header meaningful.
 *
 * What it does NOT do, stated plainly (THE RULE):
 *   - it does not verify build_id. That needs SHA-256, which this tool
 *     does not implement; the value is DISPLAYED and labelled unverified
 *     rather than silently blessed. Byte-identical output (EmbCC's M3
 *     fixed-point check) is a whole-file comparison anyway.
 *   - it does not check capabilities against a grantor. That is §6 step
 *     9 and it needs a live process tree; the shape of the table is
 *     checked, the authority is not.
 *
 * usage: embread [-q] FILE.embx
 *        -q  verify only, print nothing unless something is wrong
 * exit:  0 = every guard passed, 1 = a guard failed, 2 = usage/IO error
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/embx/embx.h"

static int problems;
static int quiet;

/* A guard failure names the spec section, because the next question is
 * always "says who". */
static void bad(const char *section, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embread: %s: ", section);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    problems++;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    if (quiet)
        return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static int is_pow2(embx_u64 v) { return v && (v & (v - 1)) == 0; }

static void hex32(const embx_u8 *b)
{
    for (int i = 0; i < 32; i++)
        printf("%02x", b[i]);
}

static unsigned char *read_file(const char *path, unsigned long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "embread: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        fprintf(stderr, "embread: cannot size '%s'\n", path);
        return NULL;
    }
    unsigned char *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        fprintf(stderr, "embread: cannot read '%s'\n", path);
        return NULL;
    }
    fclose(f);
    *len_out = (unsigned long)n;
    return buf;
}

/* §6 steps 1-8 over the header. Returns 0 when the image is too broken
 * to keep walking (the header checksum is the bootstrap: past it, no
 * offset means anything). */
static int check_header(const struct embx_header *h, unsigned long fsize)
{
    static const embx_u8 want[8] = EMBX_MAGIC_BYTES;

    if (fsize < EMBX_HDR_SIZE) {
        bad("§6.1 ETRUNC", "file is %lu bytes, shorter than a header",
            fsize);
        return 0;
    }
    if (memcmp(h->magic, want, 8) != 0) {
        bad("§6.2 EMAGIC", "not an EMBX image");
        return 0;
    }
    embx_u32 csum = embx_crc32c(h, EMBX_HDR_BODY_SIZE);
    if (csum != h->header_checksum) {
        bad("§6.3 ECHECKSUM",
            "header CRC32C is %08x, header says %08x — nothing in the "
            "header can be trusted", csum, h->header_checksum);
        return 0;
    }
    if (h->version_major != 1)
        bad("§6.4 EVERSION", "version_major %u is not understood",
            h->version_major);
    if (h->header_size != EMBX_HDR_SIZE)
        bad("§6.4 EVERSION", "header_size %u, must be %d in v1",
            h->header_size, EMBX_HDR_SIZE);
    if (h->feature_incompat & ~EMBX_KNOWN_INCOMPAT)
        bad("§6.5 EFEATURE", "unknown incompat bits %#llx set — a v1 "
            "loader refuses this image",
            (unsigned long long)(h->feature_incompat &
                                 ~EMBX_KNOWN_INCOMPAT));
    if (h->machine != EMBX_MACHINE_X86_64)
        bad("§6.7 EMACHINE", "machine %u is not x86-64", h->machine);
    if (h->abi_version > EMBX_ABI_VERSION)
        bad("§6.7 EABI", "image wants ABI %u, this toolchain knows %d",
            h->abi_version, EMBX_ABI_VERSION);
    if (h->image_size != fsize)
        bad("§6.8 ETRUNC", "image_size is %llu, file is %lu bytes",
            (unsigned long long)h->image_size, fsize);

    /* §8 header guards */
    if (h->segment_entry_size != EMBX_SEG_SIZE)
        bad("§8 EMALFORMED", "segment_entry_size %u, must be %d",
            h->segment_entry_size, EMBX_SEG_SIZE);
    if (h->capability_entry_size != EMBX_CAP_SIZE)
        bad("§8 EMALFORMED", "capability_entry_size %u, must be %d",
            h->capability_entry_size, EMBX_CAP_SIZE);
    if (h->grantor != 0)
        bad("§5.4 EMALFORMED", "grantor must be 0 in v1, is %u",
            h->grantor);
    if (h->reserved0 || h->reserved1)
        bad("§8 EMALFORMED", "a reserved header field is nonzero");
    if (h->capability_count > 0 && h->capability_table_offset == 0)
        bad("§8 EMALFORMED",
            "capability_count is %u but the table offset is 0",
            h->capability_count);
    if (h->flags & EMBX_F_PIE)
        bad("§3.5 EMALFORMED", "EMBX_F_PIE is reserved; v1 producers "
            "must not set it");
    /* §4.5: the contiguous-front guarantee a stage-2 parser depends on */
    if (h->binary_type == EMBX_TYPE_BOOT &&
        h->segment_table_offset != EMBX_HDR_SIZE)
        bad("§8 EMALFORMED", "BOOT image must put its segment table at "
            "offset %d, has %u", EMBX_HDR_SIZE, h->segment_table_offset);
    return 1;
}

static void dump_header(const struct embx_header *h)
{
    say("EMBX %u.%u  %s  machine %u  abi %u\n",
        h->version_major, h->version_minor,
        embx_type_name(h->binary_type), h->machine, h->abi_version);
    say("  entry_point        0x%llx\n",
        (unsigned long long)h->entry_point);
    say("  image_size         %llu bytes\n",
        (unsigned long long)h->image_size);
    say("  flags              %#llx%s\n", (unsigned long long)h->flags,
        (h->flags & EMBX_F_STRIPPED) ? "  (STRIPPED)" : "");
    say("  feature_incompat   %#llx\n",
        (unsigned long long)h->feature_incompat);
    say("  feature_compat     %#llx%s\n",
        (unsigned long long)h->feature_compat,
        (h->feature_compat & EMBX_COMPAT_DEBUG_SIDECAR)
            ? "  (.embdbg sidecar)" : "");
    say("  segments           %u at +%u\n", h->segment_count,
        h->segment_table_offset);
    say("  capabilities       %u at +%u\n", h->capability_count,
        h->capability_table_offset);
    say("  header_checksum    %08x  OK\n", h->header_checksum);
    if (!quiet) {
        printf("  build_id           ");
        hex32(h->build_id);
        printf("  (displayed, NOT verified — no SHA-256 here)\n");
    }
}

/* §8's segment guards plus the per-segment payload checksum (§6.10). */
static void check_segments(const struct embx_header *h,
                           const unsigned char *img, unsigned long fsize)
{
    unsigned long tab = h->segment_table_offset;
    unsigned long need = (unsigned long)h->segment_count * EMBX_SEG_SIZE;

    if (tab > fsize || need > fsize - tab) {
        bad("§8 ETRUNC", "segment table runs past the end of the image");
        return;
    }
    say("\nsegments:\n");

    int have_prev_load = 0;
    embx_u64 prev_vaddr = 0, prev_end = 0;

    for (int i = 0; i < h->segment_count; i++) {
        struct embx_segment s;
        memcpy(&s, img + tab + (unsigned long)i * EMBX_SEG_SIZE,
               sizeof s);

        char perm[4];
        perm[0] = (s.flags & EMBX_SEG_R) ? 'r' : '-';
        perm[1] = (s.flags & EMBX_SEG_W) ? 'w' : '-';
        perm[2] = (s.flags & EMBX_SEG_X) ? 'x' : '-';
        perm[3] = 0;

        say("  [%d] %-8s %s vaddr 0x%llx file +%llu/%llu mem %llu "
            "align %llu\n",
            i, embx_seg_type_name(s.type), perm,
            (unsigned long long)s.vaddr,
            (unsigned long long)s.file_offset,
            (unsigned long long)s.file_size,
            (unsigned long long)s.mem_size,
            (unsigned long long)s.align);

        if (s.reserved0)
            bad("§8 EMALFORMED", "segment %d: reserved0 is nonzero", i);
        if (s.paddr != 0 && h->binary_type != EMBX_TYPE_BOOT)
            bad("§8 EMALFORMED",
                "segment %d: paddr must be 0 outside a BOOT image", i);
        if (s.mem_size < s.file_size)
            bad("§8 EMALFORMED",
                "segment %d: mem_size %llu < file_size %llu", i,
                (unsigned long long)s.mem_size,
                (unsigned long long)s.file_size);
        if (s.file_offset > fsize || s.file_size > fsize - s.file_offset)
            bad("§8 ETRUNC",
                "segment %d: payload runs past the end of the image", i);

        if (s.type != EMBX_SEG_LOAD) {
            /* Unknown/unmapped types are IGNORED by a loader (§3.3) —
             * the additive half of the extension mechanism. */
            continue;
        }

        if (!is_pow2(s.align) || s.align < 4096)
            bad("§8 EMALFORMED",
                "segment %d: align %llu must be a power of two >= 4096",
                i, (unsigned long long)s.align);
        else if (((s.vaddr - s.file_offset) % s.align) != 0)
            bad("§8 EMALFORMED",
                "segment %d: (vaddr - file_offset) mod align != 0 — the "
                "congruence rule a demand-pager needs", i);
        if ((s.flags & EMBX_SEG_W) && (s.flags & EMBX_SEG_X))
            bad("§8 EMALFORMED",
                "segment %d: W and X both set — W^X is enforced at parse "
                "time", i);
        if (h->binary_type == EMBX_TYPE_APP &&
            (s.vaddr >= EMBX_USER_LIMIT ||
             s.mem_size > EMBX_USER_LIMIT - s.vaddr))
            bad("§8 EMALFORMED",
                "segment %d: reaches the non-canonical half", i);
        if (have_prev_load) {
            if (s.vaddr < prev_vaddr)
                bad("§8 EMALFORMED",
                    "segment %d: LOAD segments must be sorted ascending "
                    "by vaddr", i);
            else if (s.vaddr < prev_end)
                bad("§8 EMALFORMED",
                    "segment %d: overlaps the previous LOAD segment", i);
        }
        have_prev_load = 1;
        prev_vaddr = s.vaddr;
        prev_end = s.vaddr + s.mem_size;

        /* §6.10: the payload's own checksum, the thing that localizes
         * corruption to one segment instead of "the file is bad". */
        if (s.file_size &&
            s.file_offset + s.file_size <= fsize) {
            embx_u32 got = embx_crc32c(img + s.file_offset,
                                       (unsigned long)s.file_size);
            if (got != s.checksum)
                bad("§6.10 ECHECKSUM",
                    "segment %d: payload CRC32C is %08x, table says %08x",
                    i, got, s.checksum);
            else
                say("      checksum %08x OK\n", s.checksum);
        } else if (s.file_size == 0) {
            say("      (no file payload — %llu bytes of BSS)\n",
                (unsigned long long)s.mem_size);
        }
        if (s.mem_size > s.file_size)
            say("      bss tail %llu bytes zero-filled at load\n",
                (unsigned long long)(s.mem_size - s.file_size));
    }
}

/* §5.5 / §8: the table must be sorted, unique, and name capabilities
 * this kernel knows — a grantor cannot honestly grant what it cannot
 * name, so an unknown id is refused rather than ignored. */
static void check_caps(const struct embx_header *h,
                       const unsigned char *img, unsigned long fsize)
{
    if (h->capability_count == 0) {
        say("\ncapabilities: none declared\n");
        return;
    }
    unsigned long tab = h->capability_table_offset;
    unsigned long need = (unsigned long)h->capability_count * EMBX_CAP_SIZE;
    if (tab > fsize || need > fsize - tab) {
        bad("§8 ETRUNC", "capability table runs past the end");
        return;
    }
    say("\ncapabilities: %u declared\n", h->capability_count);

    embx_u32 prev = 0;
    for (int i = 0; i < h->capability_count; i++) {
        struct embx_capability c;
        memcpy(&c, img + tab + (unsigned long)i * EMBX_CAP_SIZE,
               sizeof c);
        say("  [%d] %u %s\n", i, c.cap_id, embx_cap_name(c.cap_id));

        if (c.cap_id == 0 || c.cap_id > EMBX_CAP_MAX)
            bad("§8 EMALFORMED",
                "capability %d: id %u is unknown to this kernel", i,
                c.cap_id);
        if (c.cap_flags != 0)
            bad("§8 EMALFORMED",
                "capability %d: cap_flags must be 0 in v1", i);
        if (c.reserved0 != 0)
            bad("§8 EMALFORMED",
                "capability %d: reserved0 must be 0", i);
        if (i > 0 && c.cap_id == prev)
            bad("§8 EMALFORMED", "capability %d: duplicate id %u", i,
                c.cap_id);
        else if (i > 0 && c.cap_id < prev)
            bad("§8 EMALFORMED",
                "capability %d: table must be sorted ascending by "
                "cap_id", i);
        prev = c.cap_id;
    }
}

/* §4.1 / §4.3: the type-specific contracts. */
static void check_type_contract(const struct embx_header *h,
                                const unsigned char *img,
                                unsigned long fsize)
{
    unsigned long tab = h->segment_table_offset;

    if (h->binary_type == EMBX_TYPE_FW) {
        if (h->entry_point != 0)
            bad("§4.3 EMALFORMED", "firmware must have entry_point 0");
        if (h->capability_count != 0)
            bad("§4.3 EMALFORMED",
                "firmware declares capabilities; the DRIVER holds the "
                "authority, not the blob");
    }
    if (h->binary_type != EMBX_TYPE_APP)
        return;

    if (h->entry_point == 0) {
        bad("§8 EMALFORMED", "APP entry_point is 0");
        return;
    }
    /* The entry must land inside an executable LOAD segment. */
    unsigned long need = (unsigned long)h->segment_count * EMBX_SEG_SIZE;
    if (tab > fsize || need > fsize - tab)
        return; /* already reported */
    int found = 0;
    for (int i = 0; i < h->segment_count; i++) {
        struct embx_segment s;
        memcpy(&s, img + tab + (unsigned long)i * EMBX_SEG_SIZE,
               sizeof s);
        if (s.type == EMBX_SEG_LOAD && (s.flags & EMBX_SEG_X) &&
            h->entry_point >= s.vaddr &&
            h->entry_point < s.vaddr + s.mem_size)
            found = 1;
    }
    if (!found)
        bad("§8 EMALFORMED",
            "entry_point 0x%llx is not inside an executable LOAD segment",
            (unsigned long long)h->entry_point);
}

int main(int argc, char **argv)
{
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "embread: unknown option '%s'\n", argv[i]);
            return 2;
        } else if (!path) {
            path = argv[i];
        } else {
            fprintf(stderr, "embread: one file at a time\n");
            return 2;
        }
    }
    if (!path) {
        fprintf(stderr, "usage: embread [-q] FILE.embx\n");
        return 2;
    }

    /* Before reading anyone's checksums, prove ours matches the
     * kernel's. A wrong polynomial would make every image look
     * corrupt — a confusing lie this catches immediately. */
    if (!embx_crc32c_selftest()) {
        fprintf(stderr, "embread: CRC32C selftest FAILED — this build "
                        "would disagree with the kernel\n");
        return 2;
    }
    if (!embx_layout_ok()) {
        fprintf(stderr, "embread: EMBX structure sizes are wrong for "
                        "this build\n");
        return 2;
    }

    unsigned long fsize;
    unsigned char *img = read_file(path, &fsize);
    if (!img)
        return 2;

    struct embx_header h;
    if (fsize >= EMBX_HDR_SIZE)
        memcpy(&h, img, sizeof h);
    else
        memset(&h, 0, sizeof h);

    say("%s\n", path);
    if (check_header(&h, fsize)) {
        dump_header(&h);
        check_segments(&h, img, fsize);
        check_caps(&h, img, fsize);
        check_type_contract(&h, img, fsize);
    }
    free(img);

    if (problems) {
        fprintf(stderr, "embread: %d problem%s found\n", problems,
                problems == 1 ? "" : "s");
        return 1;
    }
    say("\nall guards passed\n");
    return 0;
}
