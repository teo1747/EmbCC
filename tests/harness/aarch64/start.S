/* Bare-metal entry for the aarch64 test harness.
 *
 * qemu-system-aarch64 -M virt -kernel <elf> loads the image and jumps to
 * the ELF entry point at EL1 with SP undefined, .bss uninitialised, FP/SIMD
 * trapped and the MMU off. All four have to be dealt with before newlib will
 * run, so this file is longer than a `mov sp` — every step below is here
 * because leaving it out produced a hang.
 */
    .section .text.start, "ax"
    .global _start
_start:
    ldr     x0, =__stack_top
    mov     sp, x0

    /* 1. FP/SIMD. Every FP and SIMD instruction traps at reset
     * (CPACR_EL1.FPEN = 0b00), and newlib's memcpy/memset/printf all use q
     * registers. Without this the first one faults to a VBAR_EL1 that is
     * still zero and the guest spins on the vector page forever. */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb

    /* 2. .bss. The loader does not zero it, and newlib's reent state and
     * the page table below both live there. */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:

    /* 3. The MMU. This is not an optimisation: with the MMU off every
     * access is Device-nGnRnE, and unaligned accesses to Device memory
     * fault unconditionally -- so newlib's memset, which stores 8 bytes at
     * a 4-byte-aligned address, takes an alignment Data Abort. Mapping RAM
     * as Normal memory is what makes ordinary C legal. It also turns the
     * caches on, which is the difference between a test suite that takes
     * seconds and one that takes minutes.
     *
     * The map is flat: 1 GiB blocks in a single level-1 table, entry 0
     * Device (virt's MMIO window -- UART, GIC, virtio), entries 1..7 Normal
     * write-back over the 7 GiB of address space where RAM can appear. */
    ldr     x0, =__pgtable

    /* entry 0: PA 0, Device-nGnRnE (AttrIndx 0), AF, execute-never. */
    movz    x1, #0x0401
    movk    x1, #0x0060, lsl #48
    str     x1, [x0]

    /* entries 1..7: Normal WB (AttrIndx 1), inner-shareable, AF, block. */
    movz    x5, #0x0705
    mov     x2, #1
3:  lsl     x4, x2, #30
    orr     x1, x4, x5
    str     x1, [x0, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, #8
    b.lo    3b

    /* MAIR_EL1: attr0 = Device-nGnRnE (0x00), attr1 = Normal write-back
     * read/write-allocate, non-transient (0xFF). */
    movz    x1, #0xFF00
    msr     mair_el1, x1

    /* TCR_EL1: T0SZ=25 (39-bit VA) over a 4 KiB granule, so one level-1
     * table of 1 GiB blocks spans the whole address space; table walks
     * cacheable and inner-shareable; IPS = 40-bit PA. TTBR1 is unused, so
     * the upper half is disabled outright (EPD1). */
    ldr     x1, =(25 | (1 << 8) | (1 << 10) | (3 << 12) | (1 << 23) | (2 << 32))
    msr     tcr_el1, x1

    msr     ttbr0_el1, x0
    dsb     ishst
    tlbi    vmalle1
    dsb     ish
    isb

    mrs     x1, sctlr_el1
    orr     x1, x1, #(1 << 0)       /* M: MMU on */
    orr     x1, x1, #(1 << 2)       /* C: data cache on */
    orr     x1, x1, #(1 << 12)      /* I: instruction cache on */
    bic     x1, x1, #(1 << 1)       /* A: no strict alignment checking */
    msr     sctlr_el1, x1
    isb

    /* 4. argc = 0, argv = NULL: the tests take no arguments, and newlib's
     * startup does not require them. */
    mov     x0, #0
    mov     x1, #0
    bl      main

    /* A test that falls off the end of main exits with main's value, the
     * same rule the host-linked suite relied on. */
    bl      _exit
4:  b       4b
