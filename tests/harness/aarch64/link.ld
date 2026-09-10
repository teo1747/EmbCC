/* Test-harness image layout for QEMU virt (RAM base 0x40000000).
 *
 * 0x40080000 is the offset QEMU's virt loader hands a Linux kernel image,
 * so it is guaranteed clear of the DTB and any loader scratch. Heap and
 * stack are carved out of the same image rather than probed at run time:
 * a test that overruns either should fail loudly here, not corrupt itself.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        *(.text.start)
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)

        /* The level-1 page table start.S installs. It lives INSIDE .bss so
         * the zeroing loop clears it: only the first 8 entries are written
         * by hand, and the remaining 504 must read as invalid descriptors. */
        . = ALIGN(4096);
        __pgtable = .;
        . = . + 0x1000;
    }
    . = ALIGN(16);
    __bss_end = .;

    /* sbrk's arena — 4 MiB, enough for the suite's calloc/malloc tests. */
    . = ALIGN(16);
    __heap_start = .;
    . = . + 0x400000;
    __heap_end = .;

    /* 1 MiB of stack, growing down from __stack_top. */
    . = ALIGN(16);
    . = . + 0x100000;
    . = ALIGN(16);
    __stack_top = .;
}
