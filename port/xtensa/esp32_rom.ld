/* ESP32 ROM functions the port uses (addresses from Espressif's esp32.rom.ld). */
PROVIDE ( mmu_init = 0x400095a4 );
PROVIDE ( cache_flash_mmu_set_rom = 0x400095e0 );
PROVIDE ( Cache_Flush_rom = 0x40009a14 );
PROVIDE ( Cache_Read_Disable_rom = 0x40009ab8 );
PROVIDE ( Cache_Read_Enable_rom = 0x40009a84 );
/* SPI flash driver of the ROM, for hal/xtensa/flash.c (esp32.rom.spiflash.ld, ESP-IDF
   v3.3 and v5.2 agree). The results are 0 ok, 1 error, 2 timeout. */
PROVIDE ( esp_rom_spiflash_read = 0x40062ed8 );
PROVIDE ( esp_rom_spiflash_write = 0x40062d50 );
PROVIDE ( esp_rom_spiflash_erase_sector = 0x40062ccc );
PROVIDE ( esp_rom_spiflash_read_status = 0x4006226c );
PROVIDE ( esp_rom_spiflash_write_status = 0x400622f0 );
PROVIDE ( g_rom_flashchip = 0x3ffae270 );
