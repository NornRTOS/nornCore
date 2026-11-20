/* The selector's memory: the loader regions ESP-IDF's bootloader uses too, which no
   application image touches (the APP CPU's cache RAM, and the top of DRAM behind the
   ROM's own data), so the selector can load any slot without overwriting itself. */
MEMORY
{
    iram (RWX) : ORIGIN = 0x40078000, LENGTH = 0x8000
    dram (RW)  : ORIGIN = 0x3FFF0000, LENGTH = 0x10000
}
ENTRY(_sel_start)
SECTIONS
{
    .text : { KEEP(*(.entry.literal)) KEEP(*(.entry)) *(.literal .literal.* .text .text.*) } > iram
    .data : { *(.rodata .rodata.* .data .data.*) . = ALIGN(4); } > dram
    .bss (NOLOAD) : { _sel_bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); _sel_bss_end = .; } > dram
    _sel_stack_top = ORIGIN(dram) + LENGTH(dram);
    /DISCARD/ : { *(.xt.lit .xt.lit.*) *(.xt.prop .xt.prop.*) *(.comment) *(.eh_frame) }
}
