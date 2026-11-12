/**
 * @file flash.c
 * ESP32: the data area in the SPI flash through the ROM's flash functions. The ROM
 * drives the flash over SPI1 while the cache fetches code and constants over SPI0 from
 * the same chip, so the cache is switched off for the duration of every operation (only
 * if code runs from flash at all: rtos_port_flashPages() says), inside a critical
 * section, and nothing in this file may itself live in flash (it does not: the HAL goes
 * to IRAM unless an application lists it in FLASH_OBJS, which it must not do for this
 * file). The ROM functions want 4 byte aligned addresses, lengths and RAM buffers; the
 * callers' buffers are copied through an aligned one in 32 byte pieces.
 *
 * ROM symbols from port/xtensa/esp32_rom.ld, verified against Espressif's
 * esp32.rom.spiflash.ld (ESP-IDF v3.3 and v5.2): read 0x40062ed8, write 0x40062d50,
 * erase_sector 0x40062ccc, read_status 0x4006226c, write_status 0x400622f0, the chip
 * descriptor g_rom_flashchip 0x3ffae270. There is no unlock in the ROM scripts (IDF
 * patches it), so hal_flashInit clears the block protection bits itself if any is set.
 *
 * The ROM's chip descriptor says 2 MB after reset (its default; Espressif's second stage
 * bootloader, which nornRTOS does not have, corrects it from the image header), and the
 * ROM refuses every address beyond that size: an operation at 0x3FE000 fails with 1.
 * hal_flashInit therefore raises the descriptor's size to cover the area. The sector and
 * page sizes in the descriptor (4096, 256) are the ones every supported part has.
 */

#include "flash.h"
#include "rtos.h"

/* ROM: esp_rom_spiflash_* . Results: 0 ok, 1 error, 2 timeout. */
extern int esp_rom_spiflash_read(uint32_t src, uint32_t *pDest, int32_t len);
extern int esp_rom_spiflash_write(uint32_t dest, const uint32_t *pSrc, int32_t len);
extern int esp_rom_spiflash_erase_sector(uint32_t sector);
extern int esp_rom_spiflash_read_status(void *pChip, uint32_t *pStatus);
extern int esp_rom_spiflash_write_status(void *pChip, uint32_t status);
/** The ROM's chip descriptor: device_id, chip_size, block_size, sector_size, page_size,
    status_mask (esp_rom_spiflash_chip_t); in RAM, so the size can be corrected. */
extern uint32_t g_rom_flashchip[6];
#define ROM_CHIP_SIZE   (g_rom_flashchip[1])
extern void Cache_Read_Disable_rom(int cpuNo);
extern void Cache_Read_Enable_rom(int cpuNo);

#define AREA_SIZE       ((uint32_t)HAL_FLASH_BLOCKS * HAL_FLASH_BLOCK_SIZE)
#define STATUS_BP_MASK  0x7Cul          /* BP0..BP4 of the flash status register */

static bool _cacheOff;
static int _lastResult;

int hal_flashLastResult(void) { return _lastResult; }

/** Around every SPI operation: interrupts off, and the cache off if it is in use. */
static rtos_port_cs_t begin(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    _cacheOff = rtos_port_flashPages() != 0;
    if(_cacheOff)
        Cache_Read_Disable_rom(0);
    return cs;
}

static void end(rtos_port_cs_t cs)
{
    if(_cacheOff)
        Cache_Read_Enable_rom(0);
    rtos_port_exitCritical(cs);
}


bool hal_flashInit(void)
{
    uint32_t status = 0;
    int res;
    rtos_port_cs_t cs = begin();
    if(ROM_CHIP_SIZE < HAL_FLASH_BASE + AREA_SIZE)
        ROM_CHIP_SIZE = HAL_FLASH_BASE + AREA_SIZE;
    res = esp_rom_spiflash_read_status(g_rom_flashchip, &status);
    if(res == 0  &&  (status & STATUS_BP_MASK) != 0)
        res = esp_rom_spiflash_write_status(g_rom_flashchip, status & ~STATUS_BP_MASK);
    end(cs);
    _lastResult = res;
    return res == 0;
}


bool hal_flashRead(uint32_t addr, void *pBuf, size_t n)
{
    uint8_t *pOut = pBuf;
    uint32_t words[8];
    bool ok = true;

    if(pBuf == NULL  ||  addr + n > AREA_SIZE  ||  addr + n < addr)
        return false;
    while(n > 0  &&  ok)
    {
        /* the aligned span that covers the next piece of the request */
        const uint32_t first = addr & ~3ul;
        uint32_t span = (uint32_t)(addr - first) + (uint32_t)n;
        uint32_t i, take;
        if(span > sizeof words)
            span = sizeof words;
        span = (span + 3ul) & ~3ul;
        {
            const rtos_port_cs_t cs = begin();
            _lastResult = esp_rom_spiflash_read(HAL_FLASH_BASE + first, words, (int32_t)span);
            ok = _lastResult == 0;
            end(cs);
        }
        take = span - (addr - first);
        if(take > n)
            take = (uint32_t)n;
        for(i = 0; i < take; ++i)
            pOut[i] = ((const uint8_t *)words)[addr - first + i];
        pOut += take;
        addr += take;
        n -= take;
    }
    return ok;
}


bool hal_flashWrite(uint32_t addr, const void *pData, size_t n)
{
    const uint8_t *pIn = pData;
    uint32_t words[8];
    bool ok = true;

    if(pData == NULL  ||  (addr & 3u) != 0  ||  (n & 3u) != 0  ||  addr + n > AREA_SIZE  ||  addr + n < addr)
        return false;
    while(n > 0  &&  ok)
    {
        const uint32_t take = n < sizeof words ? (uint32_t)n : (uint32_t)sizeof words;
        uint32_t i;
        for(i = 0; i < take; ++i)
            ((uint8_t *)words)[i] = pIn[i];
        {
            const rtos_port_cs_t cs = begin();
            _lastResult = esp_rom_spiflash_write(HAL_FLASH_BASE + addr, words, (int32_t)take);
            ok = _lastResult == 0;
            end(cs);
        }
        pIn += take;
        addr += take;
        n -= take;
    }
    return ok;
}


bool hal_flashEraseBlock(uint8_t block)
{
    bool ok;
    rtos_port_cs_t cs;
    if(block >= HAL_FLASH_BLOCKS)
        return false;
    cs = begin();
    _lastResult = esp_rom_spiflash_erase_sector((HAL_FLASH_BASE / HAL_FLASH_BLOCK_SIZE) + block);
    ok = _lastResult == 0;
    end(cs);
    return ok;
}
