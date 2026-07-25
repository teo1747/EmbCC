#include "embx.h"

/* CRC32C (Castagnoli), reflected form, init and xorout 0xFFFFFFFF.
 *
 * NOT zlib's CRC-32: that uses the ISO polynomial and gives different
 * values for the same bytes. The kernel computes CRC32C (the x86 crc32
 * instruction is Castagnoli), so a producer using the wrong polynomial
 * would have every checksum rejected at load. The table is built on
 * first use rather than pasted in, so the polynomial is stated once and
 * is the only thing that could be wrong — and the selftest below checks
 * it against the canonical vector.
 */
#define CRC32C_POLY 0x82F63B78u

static embx_u32 crc_table[256];
static int crc_table_ready;

static void crc_init(void)
{
    for (int n = 0; n < 256; n++) {
        embx_u32 c = (embx_u32)n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ CRC32C_POLY : (c >> 1);
        crc_table[n] = c;
    }
    crc_table_ready = 1;
}

embx_u32 embx_crc32c(const void *data, unsigned long len)
{
    const embx_u8 *p = (const embx_u8 *)data;
    embx_u32 crc = 0xFFFFFFFFu;

    if (!crc_table_ready)
        crc_init();
    for (unsigned long i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* The canonical CRC32C check value. If this fails, every checksum this
 * toolchain writes would be rejected by the kernel — so it is checked
 * before any image is produced or verified, not documented and hoped. */
int embx_crc32c_selftest(void)
{
    return embx_crc32c("123456789", 9) == 0xE3069283u;
}

int embx_layout_ok(void)
{
    return sizeof(struct embx_header) == EMBX_HDR_SIZE &&
           sizeof(struct embx_segment) == EMBX_SEG_SIZE &&
           sizeof(struct embx_capability) == EMBX_CAP_SIZE;
}

const char *embx_cap_name(embx_u32 cap_id)
{
    switch (cap_id) {
    case EMBX_CAP_FILESYSTEM: return "FILESYSTEM";
    case EMBX_CAP_NETWORK:    return "NETWORK";
    case EMBX_CAP_GPU:        return "GPU";
    case EMBX_CAP_AUDIO:      return "AUDIO";
    case EMBX_CAP_CAMERA:     return "CAMERA";
    case EMBX_CAP_USB:        return "USB";
    case EMBX_CAP_SERIAL:     return "SERIAL";
    case EMBX_CAP_RAWDISK:    return "RAWDISK";
    case EMBX_CAP_KERNEL_EXT: return "KERNEL_EXT";
    default:                  return "<unknown>";
    }
}

/* Name -> cap_id (case-insensitive), the reverse of embx_cap_name. Used by
 * EmbLD's `--cap NAME` flag to fill an EMBX capability table. Returns 0 for an
 * unknown name (0 is never a valid cap_id). Mirrors mkembx.py's CAPS dict. */
embx_u32 embx_cap_id(const char *name)
{
    static const struct { const char *n; embx_u32 id; } tab[] = {
        { "filesystem", EMBX_CAP_FILESYSTEM }, { "network", EMBX_CAP_NETWORK },
        { "gpu", EMBX_CAP_GPU },               { "audio", EMBX_CAP_AUDIO },
        { "camera", EMBX_CAP_CAMERA },         { "usb", EMBX_CAP_USB },
        { "serial", EMBX_CAP_SERIAL },         { "rawdisk", EMBX_CAP_RAWDISK },
        { "kernel_ext", EMBX_CAP_KERNEL_EXT },
    };
    for (unsigned k = 0; k < sizeof tab / sizeof tab[0]; k++) {
        const char *a = name, *b = tab[k].n;
        for (;; a++, b++) {
            int ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;   /* fold to lower */
            if (ca != cb) break;
            if (ca == 0) return tab[k].id;
        }
    }
    return 0;
}

const char *embx_type_name(embx_u16 binary_type)
{
    switch (binary_type) {
    case EMBX_TYPE_APP:  return "APP (.embx)";
    case EMBX_TYPE_DLL:  return "DLL (.embdll)";
    case EMBX_TYPE_MOD:  return "MOD (.embmod)";
    case EMBX_TYPE_DRV:  return "DRV (.embdrv)";
    case EMBX_TYPE_FW:   return "FW (.embfw)";
    case EMBX_TYPE_BOOT: return "BOOT (.embboot)";
    default:             return "<unknown>";
    }
}

const char *embx_seg_type_name(embx_u32 type)
{
    switch (type) {
    case EMBX_SEG_NULL:     return "NULL";
    case EMBX_SEG_LOAD:     return "LOAD";
    case EMBX_SEG_DYNAMIC:  return "DYNAMIC";
    case EMBX_SEG_TLS:      return "TLS";
    case EMBX_SEG_NOTE:     return "NOTE";
    case EMBX_SEG_DEBUG:    return "DEBUG";
    case EMBX_SEG_FIRMWARE: return "FIRMWARE";
    default:                return "<unknown>";
    }
}
