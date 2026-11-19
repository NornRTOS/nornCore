/**
 * @file flash.c
 * AVR: the data area is the EEPROM, in two halves (E2END + 1 is 4096 on the ATmega2560,
 * 1024 on the ATmega328P). EEPROM bytes are written in place, so there is no erase in
 * the hardware sense: hal_flashEraseBlock writes 0xFF where a byte is not 0xFF already,
 * and hal_flashWrite writes only the bytes that differ (avr-libc's eeprom_update_*), which
 * keeps both the time (about 3.4 ms per byte written) and the wear down. The avr-libc
 * routines wait for the previous write to finish and mask interrupts only around the
 * two-instruction write trigger, so the kernel keeps ticking during a long write; the
 * calling task is simply busy for the duration.
 */

#include "flash.h"
#include <avr/eeprom.h>

#define AREA_SIZE   ((uint32_t)HAL_FLASH_BLOCKS * HAL_FLASH_BLOCK_SIZE)


bool hal_flashInit(void)
{
    return true;
}


bool hal_flashRead(uint32_t addr, void *pBuf, size_t n)
{
    if(pBuf == NULL  ||  addr + n > AREA_SIZE)
        return false;
    eeprom_read_block(pBuf, (const void *)(uintptr_t)addr, n);
    return true;
}


bool hal_flashWrite(uint32_t addr, const void *pData, size_t n)
{
    if(pData == NULL  ||  addr + n > AREA_SIZE)
        return false;
    eeprom_update_block(pData, (void *)(uintptr_t)addr, n);
    return true;
}


bool hal_flashEraseBlock(uint8_t block)
{
    uint16_t i;
    uint16_t addr;
    if(block >= HAL_FLASH_BLOCKS)
        return false;
    addr = (uint16_t)((uint16_t)block * HAL_FLASH_BLOCK_SIZE);
    for(i = 0; i < HAL_FLASH_BLOCK_SIZE; ++i)
        eeprom_update_byte((uint8_t *)(uintptr_t)(addr + i), 0xFFu);
    return true;
}


int hal_flashLastResult(void)
{
    return 0;
}
