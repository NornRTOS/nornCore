/* ESP32 ROM functions the port uses (addresses from Espressif's esp32.rom.ld). */
PROVIDE ( mmu_init = 0x400095a4 );
PROVIDE ( cache_flash_mmu_set_rom = 0x400095e0 );
PROVIDE ( Cache_Flush_rom = 0x40009a14 );
PROVIDE ( Cache_Read_Disable_rom = 0x40009ab8 );
PROVIDE ( Cache_Read_Enable_rom = 0x40009a84 );
