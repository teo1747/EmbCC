/* EMBX (EmbLink Binary eXecutable) — the native container.
 *
 * Byte-exact per EMBX_Specification_v2 §3. This mirrors the kernel's
 * loader header (myos/kernel/arch/x86_64/syscall/embx.h), which is the
 * authority: if the two ever disagree the kernel wins and this file is
 * the bug. The _Static_asserts are what make that disagreement a build
 * failure instead of a silently misread image (spec §2.4).
 *
 * Shared, exactly as src/elf/elf.h is: embread READS these structures,
 * and the integrated linker (M3) will WRITE them — an EMBX APP is a
 * fully-linked image (§4.1: no relocations), so the producer of an
 * .embx is the linker, never the compiler proper. See DECISIONS D-003
 * (revised 2026-07-24).
 *
 * Hand-written, no host <stdint.h>: EmbCC must eventually compile its
 * own source (ARCHITECTURE §7), the same reason src/elf/elf.h defines
 * its own types.
 */
#ifndef EMBCC_EMBX_EMBX_H
#define EMBCC_EMBX_EMBX_H

typedef unsigned char      embx_u8;
typedef unsigned short     embx_u16;
typedef unsigned int       embx_u32;
typedef unsigned long long embx_u64;

/* ".EMBX\r\n\x1A" — the CRLF pair breaks loudly if a text-mode transfer
 * mangles line endings; the \x1A stops a naive cat (§3.1). */
#define EMBX_MAGIC_BYTES { 0x7F, 'E', 'M', 'B', 'X', 0x0D, 0x0A, 0x1A }

#define EMBX_HDR_SIZE  128
#define EMBX_SEG_SIZE  64
#define EMBX_CAP_SIZE  16

/* binary_type (§4). Only APP has a load contract in v1; the rest are
 * reserved by name and every loader refuses them. */
#define EMBX_TYPE_APP   1
#define EMBX_TYPE_DLL   2
#define EMBX_TYPE_MOD   3
#define EMBX_TYPE_DRV   4
#define EMBX_TYPE_FW    5
#define EMBX_TYPE_BOOT  6

#define EMBX_MACHINE_X86_64 1
#define EMBX_MACHINE_ARM64  2
#define EMBX_ABI_VERSION    1

/* segment types (§3.3). An unknown type is IGNORED, not an error — that
 * is the additive half of the extension mechanism. */
#define EMBX_SEG_NULL     0
#define EMBX_SEG_LOAD     1
#define EMBX_SEG_DYNAMIC  2
#define EMBX_SEG_TLS      3
#define EMBX_SEG_NOTE     4
#define EMBX_SEG_DEBUG    5
#define EMBX_SEG_FIRMWARE 6

#define EMBX_SEG_R 1
#define EMBX_SEG_W 2
#define EMBX_SEG_X 4

/* image flags (§3.5) */
#define EMBX_F_PIE      0x1ULL  /* reserved; v1 producers MUST NOT set */
#define EMBX_F_STRIPPED 0x2ULL

/* feature bits (§3.6): any incompat bit outside KNOWN means refuse. */
#define EMBX_INCOMPAT_SIGNED     0x1ULL
#define EMBX_INCOMPAT_COMPRESSED 0x2ULL
#define EMBX_INCOMPAT_ENCRYPTED  0x4ULL
#define EMBX_INCOMPAT_LINKAGE    0x8ULL
#define EMBX_KNOWN_INCOMPAT      0ULL

#define EMBX_COMPAT_DEBUG_SIDECAR 0x1ULL

/* The canonical low-half boundary access_ok() enforces (§4.1). */
#define EMBX_USER_LIMIT 0x0000800000000000ULL

struct embx_header {
    embx_u8  magic[8];                 /* 0x00 */
    embx_u16 version_major;            /* 0x08 */
    embx_u16 version_minor;            /* 0x0A */
    embx_u32 header_size;              /* 0x0C  == 128 in v1 */
    embx_u16 binary_type;              /* 0x10 */
    embx_u16 machine;                  /* 0x12 */
    embx_u32 abi_version;              /* 0x14 */
    embx_u64 flags;                    /* 0x18 */
    embx_u64 feature_incompat;         /* 0x20 */
    embx_u64 feature_compat;           /* 0x28 */
    embx_u64 entry_point;              /* 0x30 */
    embx_u32 segment_table_offset;     /* 0x38 */
    embx_u16 segment_count;            /* 0x3C */
    embx_u16 segment_entry_size;       /* 0x3E  == 64 */
    embx_u32 capability_table_offset;  /* 0x40 */
    embx_u16 capability_count;         /* 0x44 */
    embx_u16 capability_entry_size;    /* 0x46  == 16 */
    embx_u32 grantor;                  /* 0x48  reserved, MUST be 0 in v1 */
    embx_u32 reserved0;                /* 0x4C */
    embx_u8  build_id[32];             /* 0x50  SHA-256 */
    embx_u64 image_size;               /* 0x70 */
    embx_u32 reserved1;                /* 0x78 */
    embx_u32 header_checksum;          /* 0x7C  CRC32C over 0x00..0x7B */
} __attribute__((packed));

/* The checksummed span, derived from the struct so it cannot drift from
 * the layout (§3.1): 128 - 4 = 124. */
#define EMBX_HDR_BODY_SIZE (sizeof(struct embx_header) - sizeof(embx_u32))

struct embx_segment {
    embx_u32 type;          /* 0x00 */
    embx_u32 flags;         /* 0x04  R/W/X, meaningful for LOAD */
    embx_u64 vaddr;         /* 0x08 */
    embx_u64 file_offset;   /* 0x10 */
    embx_u64 file_size;     /* 0x18 */
    embx_u64 mem_size;      /* 0x20  >= file_size; the tail IS the BSS */
    embx_u64 align;         /* 0x28  power of two, >= 4096 for LOAD */
    embx_u32 checksum;      /* 0x30  CRC32C over file_size bytes */
    embx_u32 reserved0;     /* 0x34 */
    embx_u64 paddr;         /* 0x38  BOOT-only; MUST be 0 elsewhere */
} __attribute__((packed));

struct embx_capability {
    embx_u32 cap_id;        /* 0x00 */
    embx_u32 cap_flags;     /* 0x04  reserved, MUST be 0 in v1 */
    embx_u64 reserved0;     /* 0x08  reserved for a v2 parameter */
} __attribute__((packed));

/* Spec §2.4: off by one byte and the BUILD fails, not the loader.
 *
 * Guarded because EmbCC parses neither _Static_assert nor __attribute__
 * yet, and this file must survive self-hosting (ARCHITECTURE §7). That
 * is safe rather than a hole for two reasons: every field in all three
 * structures is already NATURALLY aligned — the format was designed that
 * way, so packed and unpacked layouts are identical — and
 * embx_layout_ok() re-checks the sizes at RUNTIME, which an
 * EmbCC-compiled build still executes. */
#ifndef __EMBCC__
_Static_assert(sizeof(struct embx_header) == EMBX_HDR_SIZE,
               "EMBX header must be 128 bytes");
_Static_assert(sizeof(struct embx_segment) == EMBX_SEG_SIZE,
               "EMBX segment entry must be 64 bytes");
_Static_assert(sizeof(struct embx_capability) == EMBX_CAP_SIZE,
               "EMBX capability entry must be 16 bytes");
#endif

/* Capability IDs (§5.6). KERNEL_EXT (9) is reserved by name and has no
 * consumer: the only kinds that could use it are refused kinds, so a
 * producer should not emit it. */
#define EMBX_CAP_FILESYSTEM 1
#define EMBX_CAP_NETWORK    2
#define EMBX_CAP_GPU        3
#define EMBX_CAP_AUDIO      4
#define EMBX_CAP_CAMERA     5
#define EMBX_CAP_USB        6
#define EMBX_CAP_SERIAL     7
#define EMBX_CAP_RAWDISK    8
#define EMBX_CAP_KERNEL_EXT 9
#define EMBX_CAP_MAX        9

/* CRC32C (Castagnoli, reflected, init/xorout 0xFFFFFFFF) — the house
 * checksum, the same one the kernel's hardware instruction computes.
 * embx_crc32c_selftest() checks the canonical "123456789" vector so a
 * mismatch with the kernel is caught here, not in a rejected image. */
embx_u32 embx_crc32c(const void *data, unsigned long len);
int embx_crc32c_selftest(void);

/* Name -> cap_id (case-insensitive), reverse of embx_cap_name; 0 if unknown.
 * Backs EmbLD's `--cap NAME`. */
embx_u32 embx_cap_id(const char *name);

/* Runtime restatement of the _Static_asserts above, for builds where
 * they were compiled out. Nonzero when the layout is right. */
int embx_layout_ok(void);

const char *embx_cap_name(embx_u32 cap_id);
const char *embx_type_name(embx_u16 binary_type);
const char *embx_seg_type_name(embx_u32 type);

#endif
