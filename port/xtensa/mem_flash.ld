/* Code and constants that run from flash through the cache (see esp32.ld). drom and irom
   are the virtual windows the MMU maps the same flash pages into; flash is where the
   .flash.* sections are stored, at FLASH_BASE (arch.mk), as a separate image. A build
   without FLASH_OBJS leaves these sections empty and the mapping is skipped. */
MEMORY
{
    drom (R)   : ORIGIN = 0x3F400000, LENGTH = 0x300000
    irom (RX)  : ORIGIN = 0x400D0000, LENGTH = 0x300000
    flash (RX) : ORIGIN = @FLASH_BASE@, LENGTH = 0x300000
    rtc_slow (RW) : ORIGIN = 0x50000000, LENGTH = 0x2000     /* kept through deep sleep (hal/power.h) */
}
FLASH_BASE = @FLASH_BASE@;
