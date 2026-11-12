#ifndef FLASH_INCLUDED
#define FLASH_INCLUDED
/**
 * @file flash.h
 * A small reserved area of non-volatile memory for the application's data: two erase
 * blocks, read, written and erased through one API on every architecture. Meant for a
 * configuration store (nornStore) or anything else that keeps a few hundred bytes
 * across resets. Implemented per architecture in hal/xtensa/flash.c and hal/avr/flash.c:
 *
 *   ESP32    the last two 4 KB sectors of the SPI flash (HAL_FLASH_BASE, 0x3FE000 on a
 *            4 MB part), through the ROM's SPI flash functions, with the flash cache
 *            switched off and interrupts masked while the SPI bus is in use. Erasing a
 *            sector takes tens of milliseconds, writing a page a fraction of one; the
 *            tick is lost for that long, so write and erase only when the system can
 *            afford it. Read is quick. No image of the build reaches this area: the RAM
 *            image is at 0x1000 and FLASH_BASE (code in flash) is 0x80000 and up.
 *   AVR      the EEPROM in two halves (2 x 2 KB on the Mega 2560, 2 x 512 B on the UNO).
 *            Bytes are written in place at about 3.4 ms each, with interrupts enabled;
 *            erase fills the half with 0xFF the same way. hal_flashWrite writes only the
 *            bytes that differ (avr-libc's eeprom_update_block).
 *
 *   hal_flashInit()                  once, before the first write; unlocks a protected part
 *   hal_flashRead(addr, buf, n)      any addr and n inside the area
 *   hal_flashWrite(addr, data, n)    addr and n multiples of HAL_FLASH_WRITE_ALIGN; the bytes
 *                                    must have been erased (0xFF) on the ESP32
 *   hal_flashEraseBlock(block)       block 0 or 1 to 0xFF
 *
 * All return false on a driver or argument error. Addresses are relative to the area:
 * 0 .. HAL_FLASH_BLOCKS * HAL_FLASH_BLOCK_SIZE - 1. Not for interrupt context.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hal_arch.h"

#if defined(__AVR__)
# include <avr/io.h>
# define HAL_FLASH_BLOCK_SIZE    ((uint16_t)((E2END + 1) / 2))
# define HAL_FLASH_WRITE_ALIGN   1u
#else
# define HAL_FLASH_BLOCK_SIZE    4096u
# define HAL_FLASH_WRITE_ALIGN   4u
/** Byte address of the area in the SPI flash: the last two sectors of a 4 MB part.
    Override from the application's build (EXTRA_CFLAGS=-DHAL_FLASH_BASE=...) for
    another part. */
# ifndef HAL_FLASH_BASE
#  define HAL_FLASH_BASE         0x3FE000ul
# endif
#endif
#define HAL_FLASH_BLOCKS         2u

bool hal_flashInit(void);
bool hal_flashRead(uint32_t addr, void *pBuf, size_t n);
bool hal_flashWrite(uint32_t addr, const void *pData, size_t n);
bool hal_flashEraseBlock(uint8_t block);
/** The driver's result code of the last operation, for a diagnostic print: 0 ok; ESP32:
    1 error, 2 timeout from the ROM; AVR: always 0. */
int hal_flashLastResult(void);

#endif  /* FLASH_INCLUDED */
