/*
 * nornOS on the ESP32 without a second stage bootloader: the ROM loader reads the image at
 * flash offset 0x1000 and copies every segment straight into IRAM and DRAM, then jumps to
 * _start. Nothing runs from flash, so no MMU or cache setup is needed.
 *
 * IRAM starts at 0x40080400: the first kilobyte is left to the ROM loader, and 0x40080400
 * is 1 KB aligned as VECBASE requires. DRAM ends well below 0x3FFE0000, where the ROM keeps
 * its own data and the stack it runs the loader on. With two cores (AMP) each core gets
 * its own image and half of the memory; see mem_core0.ld and mem_core1.ld.
 *
 * Code too big for IRAM can run from flash: the objects and archives named in FLASH_OBJS
 * (Makefile) get their text in .flash.text and their constants in .flash.rodata, both
 * stored in a second image at FLASH_BASE and mapped by rtos_port_flashInit through the
 * flash MMU and the cache before main (port_esp32.c). Any object may also put a function
 * in flash with RTOS_PORT_FLASH_TEXT. Interrupt handlers and the kernel stay in IRAM.
 */

/* The MEMORY blocks come from mem_single.ld (or mem_core0.ld, mem_core1.ld) and
   mem_flash.ld; the ld_*.ld includes list the module's section patterns. arch.mk
   generates them and concatenates everything into the build directory. */

ENTRY(_start)

SECTIONS
{
    .vectors :
    {
        KEEP(*(.vectors))
    } > iram

    /* Constants of the flash objects, read through the data cache window. */
    .flash.rodata :
    {
        . = ALIGN(4);
        _flash_rodata_start = .;
        INCLUDE @LD_DIR@/ld_flash_rodata.ld
        *(.flash.rodata .flash.rodata.*)
        . = ALIGN(4);
        _flash_rodata_end = .;
    } > drom AT > flash

    /* Code of the flash objects, directly behind the constants in the same flash pages,
       seen through the instruction cache window. */
    .flash.text ORIGIN(irom) + SIZEOF(.flash.rodata) : AT(LOADADDR(.flash.rodata) + SIZEOF(.flash.rodata))
    {
        _flash_text_start = .;
        INCLUDE @LD_DIR@/ld_flash_text.ld
        *(.flash.text .flash.text.*)
        . = ALIGN(4);
        _flash_text_end = .;
    }
    ASSERT(SIZEOF(.flash.rodata) + SIZEOF(.flash.text) <= LENGTH(flash), "flash sections too big")
    _flash_lma_start = LOADADDR(.flash.rodata);
    _flash_lma_end = LOADADDR(.flash.text) + SIZEOF(.flash.text);
    /* 64 KB MMU pages to map, 0 when nothing is in flash. */
    _flash_pages = (_flash_lma_end - FLASH_BASE + 0xFFFF) >> 16;

    .text :
    {
        *(.entry.literal)
        *(.entry)
        *(.literal .text .literal.* .text.*)
        *(.iram1 .iram1.*)
        INCLUDE @LD_DIR@/ld_iram.ld
    } > iram

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > dram

    .data :
    {
        . = ALIGN(4);
        *(.data .data.*)
        *(.dram1 .dram1.*)
        INCLUDE @LD_DIR@/ld_dram.ld
        . = ALIGN(4);
    } > dram

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > dram

    /* Kept across a reset: not cleared by _start. */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        *(.noinit .noinit.*)
        . = ALIGN(4);
    } > dram

    /* The idle task (main) runs on the top of DRAM. */
    _heap_start = .;
    _stack_top = ORIGIN(dram) + LENGTH(dram);

    /DISCARD/ :
    {
        *(.xt.lit .xt.lit.*)
        *(.xt.prop .xt.prop.*)
        *(.comment)
        *(.eh_frame)
    }
}
