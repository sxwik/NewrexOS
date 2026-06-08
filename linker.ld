ENTRY(_start) /* Change from ENTRY(start) to ENTRY(_start) */
SECTIONS {
    . = 0x100000; /* 1MB loading address point */

    .text : {
        *(.multiboot) /* CRITICAL: Must be at the very top */
        *(.text)
    }

    .rodata : { *(.rodata) }
    .data   : { *(.data) }
    .bss    : { *(.bss) }
}
