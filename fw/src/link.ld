OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    DDR (rwx) : ORIGIN = 0x80000000, LENGTH = 512M
}

SECTIONS
{
    .text : {
        KEEP(*(.vectors))
        KEEP(*(.fwversion))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        __text_end = .;
    } > DDR
    .data : { *(.data*) . = ALIGN(4); } > DDR
    /* Nothing runs static constructors (start.S): C++ must not need any. */
    .init_array : { *(.init_array*) *(.ctors*) } > DDR
    ASSERT(SIZEOF(.init_array) == 0, "static constructors in the image")
    /* Last 8 bytes of the image: magic, then CRC-32 of everything before,
     * filled in by tools/mkapp.py. */
    .imagecheck : { KEEP(*(.imagecheck)) } > DDR
    __image_end = .;
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > DDR
    . = ALIGN(16);
    . += 0x10000;
    __irq_stack_top = .;
    . += 0x10000;
    __svc_stack_top = .;
    /* Not zeroed at boot (folder kept-lines arena). */
    .noinit __svc_stack_top (NOLOAD) : { *(.noinit*) }
    /* Sample memory: from here to the DMA region (samples.c). */
    __sample_pool = ALIGN(0x100000);
    /* Uncached DMA buffers (mmu.h DMA_REGION); not in the image. */
    .dma 0x9ff00000 (NOLOAD) : { *(.dma*) }
    /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) }
}
