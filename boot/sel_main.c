/**
 * @file sel_main.c
 * The OTA selector (nornInternal/nornCore/ota_plan.md step 1): the image the ROM loads
 * from flash offset 0x1000 in an OTA build. It reads the slot table, picks the slot to
 * boot (the confirmed one, or a trial with boots left, counting the boot), checks the
 * slot's SHA-256, copies the slot's RAM image into IRAM and DRAM the way the ROM loader
 * would, tells the port where the slot's flash image is (two RTC store registers) and
 * jumps to the image's entry. Everything is read through the cache's flash window, so a
 * flash encrypted later (step 4) reads the same.
 *
 * The hash is followed by an ed25519 check of the signature over the digest (Monocypher,
 * boot/monocypher*.c, BSD-2 or CC0; the public key from tools/ota_keygen.py in
 * boot/ota_pubkey.h), so a slot is booted only if it came from the key's owner.
 *
 * No kernel, no interrupts, no libc; the console is the UART the ROM left at 115200 on
 * the crystal. Runs at the crystal's 40 MHz, which is what the ROM leaves; the hash goes
 * through the chip's SHA block (software SHA-256 took 1.06 s for a 390 KB image).
 */

#include <stdint.h>
#include <stddef.h>
#include "port_regs.h"
#include "ota_layout.h"
#include "monocypher-ed25519.h"
#include "ota_pubkey.h"

/* The compiler emits these for struct copies and clears; no libc here. */
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a = d; const unsigned char *b = s; while(n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { unsigned char *a = d; while(n--) *a++ = (unsigned char)c; return d; }


/*
 * Defines
 */

#define FLASH_DROM_WINDOW   0x3F400000ul
#define FLASH_PAGE          0x10000ul
#define RTC_STORE2          PORT_REG(DR_REG_RTCCNTL_BASE + 0x54)
#define RTC_STORE3          PORT_REG(DR_REG_RTCCNTL_BASE + 0x58)
#define TIMG0_WDTCONFIG0    PORT_REG(DR_REG_TIMG0_BASE + 0x48)
#define TIMG0_WDTWPROTECT   PORT_REG(DR_REG_TIMG0_BASE + 0x64)
#define TIMG1_WDTCONFIG0    PORT_REG(DR_REG_TIMG1_BASE + 0x48)
#define TIMG1_WDTWPROTECT   PORT_REG(DR_REG_TIMG1_BASE + 0x64)
#define RTC_WDTCONFIG0      PORT_REG(DR_REG_RTCCNTL_BASE + 0x8C)
#define RTC_WDTWPROTECT     PORT_REG(DR_REG_RTCCNTL_BASE + 0xA4)
#define UART_FIFO           PORT_REG(DR_REG_UART_BASE + 0x00)
#define UART_STATUS         PORT_REG(DR_REG_UART_BASE + 0x1C)

/* ROM */
extern void mmu_init(int cpuNo);
extern unsigned int cache_flash_mmu_set_rom(int cpuNo, int pid, unsigned int vaddr, unsigned int paddr, int psize, int num);
extern void Cache_Read_Disable_rom(int cpuNo);
extern void Cache_Read_Enable_rom(int cpuNo);
extern void Cache_Flush_rom(int cpuNo);
extern int esp_rom_spiflash_erase_sector(uint32_t sector);
extern int esp_rom_spiflash_write(uint32_t dest, const uint32_t *pSrc, int32_t len);


/*
 * Console
 */

static void putc_(char c)
{
    while(((UART_STATUS >> 16) & 0xFFu) >= 120u)
        ;
    UART_FIFO = (uint32_t)(uint8_t)c;
}

static void puts_(const char *s)
{
    for(; *s; ++s)
        putc_(*s);
}

static void putU32(uint32_t v)
{
    char buf[11];
    int i = 10;
    buf[i] = 0;
    do { buf[--i] = (char)('0' + v % 10u); v /= 10u; } while(v != 0);
    puts_(&buf[i]);
}

static void flush_(void)
{
    while(((UART_STATUS >> 16) & 0xFFu) != 0)
        ;
}

static inline uint32_t ccount(void)
{
    uint32_t v;
    __asm__ __volatile__ ("rsr.ccount %0" : "=a"(v));
    return v;
}

/** Cycles per millisecond. The ROM leaves the crystal's 40 MHz after a power-on reset,
    but the clock selection lives in the RTC domain: after a software reset the PLL is
    still selected and the CPU runs at 80 MHz (the divider is reset), so the time base is
    read, not assumed. */
static uint32_t cyclesPerMs(void)
{
    const uint32_t sel = (PORT_REG(DR_REG_RTCCNTL_BASE + 0x70) >> 27) & 3u;   /* SOC_CLK_SEL */
    const uint32_t div = PORT_REG(DR_REG_DPORT_BASE + 0x3C) & 3u;            /* CPU_PER_CONF */
    if(sel != 1u)
        return 40000u;
    return div == 0 ? 80000u : div == 1 ? 160000u : 240000u;
}


/*
 * Flash through the cache window
 */

static uint32_t _mapped = 0xFFFFFFFFul;         /* physical base of the mapped pages */

/** Maps @p pages 64 KB pages from physical @p base at the data window. */
static const uint8_t *mapFlash(uint32_t base, uint32_t pages)
{
    if(_mapped != base)
    {
        Cache_Read_Disable_rom(0);
        Cache_Flush_rom(0);
        mmu_init(0);
        (void)cache_flash_mmu_set_rom(0, 0, FLASH_DROM_WINDOW, base, 64, (int)pages);
        PORT_REG(DR_REG_DPORT_BASE + 0x44) &= ~((1ul << 0) | (1ul << 2) | (1ul << 3) | (1ul << 4));   /* PRO_CACHE_CTRL1: IRAM0, IROM0, DRAM1, DROM0 masks off */
        Cache_Read_Enable_rom(0);
        _mapped = base;
    }
    return (const uint8_t *)FLASH_DROM_WINDOW;
}


/*
 * CRC-32, and SHA-256 on the ESP32's SHA block
 */

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFul;
    while(n--)
    {
        uint8_t i;
        c ^= *p++;
        for(i=0; i<8; ++i)
            c = (c >> 1) ^ (0xEDB88320ul & (0u - (c & 1u)));
    }
    return ~c;
}

/* The SHA accelerator (ESP32 TRM "SHA Accelerator"): a 64 byte block is written to the
   text registers as big endian words, START hashes the first block and CONTINUE the next
   ones, LOAD moves the digest into the text registers. Padding is the software's. */
#define SHA_TEXT(i)         PORT_REG(0x3FF03000ul + 4ul * (i))
#define SHA256_START        PORT_REG(0x3FF03090ul)
#define SHA256_CONTINUE     PORT_REG(0x3FF03094ul)
#define SHA256_LOAD         PORT_REG(0x3FF03098ul)
#define SHA256_BUSY         PORT_REG(0x3FF0309Cul)
#define DPORT_PERI_CLK_EN   PORT_REG(DR_REG_DPORT_BASE + 0x1C)
#define DPORT_PERI_RST_EN   PORT_REG(DR_REG_DPORT_BASE + 0x20)
#define DPORT_PERI_SHA      (1ul << 1)
#define DPORT_PERI_SECUREBOOT (1ul << 3)

typedef struct { uint8_t block[64]; uint32_t fill; uint64_t total; int started; } sha256_t;

static uint32_t bswap(uint32_t v)
{
    return (v << 24) | ((v & 0xFF00ul) << 8) | ((v >> 8) & 0xFF00ul) | (v >> 24);
}

static void shaBlock(sha256_t *s)
{
    const uint32_t *w = (const uint32_t *)s->block;
    uint32_t i;
    while(SHA256_BUSY) ;
    for(i=0; i<16; ++i)
        SHA_TEXT(i) = bswap(w[i]);
    if(s->started) SHA256_CONTINUE = 1; else { SHA256_START = 1; s->started = 1; }
}

static void shaInit(sha256_t *s)
{
    DPORT_PERI_CLK_EN |= DPORT_PERI_SHA;
    DPORT_PERI_RST_EN &= ~(DPORT_PERI_SHA | DPORT_PERI_SECUREBOOT);  /* SHA stays in reset while secure boot is (ESP-IDF periph_ctrl) */
    s->fill = 0; s->total = 0; s->started = 0;
}

static void shaUpdate(sha256_t *s, const uint8_t *p, size_t n)
{
    s->total += n;
    while(n != 0)
    {
        if(s->fill == 0  &&  n >= 64  &&  (((uintptr_t)p) & 3u) == 0)
        {
            /* whole aligned blocks straight from the source */
            const uint32_t *w = (const uint32_t *)p;
            uint32_t i;
            while(SHA256_BUSY) ;
            for(i=0; i<16; ++i)
                SHA_TEXT(i) = bswap(w[i]);
            if(s->started) SHA256_CONTINUE = 1; else { SHA256_START = 1; s->started = 1; }
            p += 64; n -= 64;
            continue;
        }
        s->block[s->fill++] = *p++; --n;
        if(s->fill == 64) { shaBlock(s); s->fill = 0; }
    }
}

static void shaFinal(sha256_t *s, uint8_t out[32])
{
    const uint64_t bits = s->total * 8u;
    uint32_t i;
    s->block[s->fill++] = 0x80;
    if(s->fill > 56) { while(s->fill < 64) s->block[s->fill++] = 0; shaBlock(s); s->fill = 0; }
    while(s->fill < 56) s->block[s->fill++] = 0;
    for(i=0; i<8; ++i) s->block[56 + i] = (uint8_t)(bits >> (56 - 8*i));
    shaBlock(s);
    while(SHA256_BUSY) ;
    SHA256_LOAD = 1;
    while(SHA256_BUSY) ;
    for(i=0; i<8; ++i)
    {
        /* The digest words come out as the big endian bytes of the hash already (the ESP32
           SHA block keeps the text registers' byte order), so no swap here. */
        const uint32_t v = SHA_TEXT(i);
        out[4*i] = (uint8_t)(v >> 24); out[4*i+1] = (uint8_t)(v >> 16); out[4*i+2] = (uint8_t)(v >> 8); out[4*i+3] = (uint8_t)v;
    }
}


/*
 * The table
 */

static ota_table_t _table;
static sha256_t _sha;

static int tableValid(const ota_table_t *t)
{
    return t->magic == OTA_TABLE_MAGIC  &&  t->version == OTA_TABLE_VERSION
        &&  t->active < OTA_SLOTS
        &&  crc32((const uint8_t *)t, offsetof(ota_table_t, crc32)) == t->crc32;
}

/** The ruling copy into _table; returns its copy index or -1. */
static int readTable(void)
{
    const uint8_t * const page0 = mapFlash(0, 1);
    int best = -1, i;
    for(i=0; i<OTA_TABLE_COPIES; ++i)
    {
        const ota_table_t * const t = (const ota_table_t *)(page0 + OTA_TABLE_ADDR + (uint32_t)i * OTA_SECTOR);
        if(tableValid(t)  &&  (best < 0  ||  t->sequence > _table.sequence))
        {
            const uint32_t *src = (const uint32_t *)t;
            uint32_t *dst = (uint32_t *)&_table, n;
            for(n = 0; n < sizeof(_table) / 4; ++n)     /* no libc here */
                dst[n] = src[n];
            best = i;
        }
    }
    return best;
}

/** Writes _table into the copy that did not rule (@p ruling), with the next sequence. */
static void writeTable(int ruling)
{
    const int target = (ruling + 1) % OTA_TABLE_COPIES;
    const uint32_t addr = OTA_TABLE_ADDR + (uint32_t)target * OTA_SECTOR;
    ++ _table.sequence;
    _table.crc32 = crc32((const uint8_t *)&_table, offsetof(ota_table_t, crc32));
    Cache_Read_Disable_rom(0);
    (void)esp_rom_spiflash_erase_sector(addr / OTA_SECTOR);
    (void)esp_rom_spiflash_write(addr, (const uint32_t *)&_table, (int32_t)((sizeof(_table) + 3u) & ~3u));
    Cache_Flush_rom(0);
    Cache_Read_Enable_rom(0);
    _mapped = 0xFFFFFFFFul;                     /* the window's lines are stale */
}


/*
 * Slots
 */

static uint32_t slotBase(uint32_t slot)
{
    return slot == 0 ? OTA_SLOT_A : OTA_SLOT_B;
}

/** SHA-256 of the slot's RAM and flash images against the table. */
static int slotChecks(uint32_t slot)
{
    const ota_slot_t * const e = &_table.slot[slot];
    const uint8_t * const p = mapFlash(slotBase(slot), OTA_SLOT_SIZE / FLASH_PAGE);
    uint8_t digest[32];
    uint32_t i;
    if(e->ramLength == 0  ||  e->ramLength > OTA_SLOT_FLASH_OFF  ||  e->flashLength > OTA_SLOT_SIZE - OTA_SLOT_FLASH_OFF)
        return 0;
    shaInit(&_sha);
    shaUpdate(&_sha, p, e->ramLength);
    shaUpdate(&_sha, p + OTA_SLOT_FLASH_OFF, e->flashLength);
    shaFinal(&_sha, digest);
    for(i=0; i<32; ++i)
        if(digest[i] != e->sha256[i])
        {
            puts_("digest "); putU32(((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) | ((uint32_t)digest[2] << 8) | digest[3]);
            puts_(" expected "); putU32(((uint32_t)e->sha256[0] << 24) | ((uint32_t)e->sha256[1] << 16) | ((uint32_t)e->sha256[2] << 8) | e->sha256[3]); puts_(": ");
            return 0;
        }
    return 1;
}

/** ed25519 over the slot's 32 digest bytes against the compiled in public key. */
static int slotSigned(uint32_t slot)
{
    const ota_slot_t * const e = &_table.slot[slot];
    return crypto_ed25519_check(e->signature, OTA_PUBKEY, e->sha256, 32) == 0;
}

/** Copies the slot's RAM image (esptool's format) into place; returns the entry or 0. */
static uint32_t loadSlot(uint32_t slot)
{
    const uint8_t *p = mapFlash(slotBase(slot), OTA_SLOT_SIZE / FLASH_PAGE);
    const uint8_t * const end = p + _table.slot[slot].ramLength;
    uint32_t entry, segments, i;
    if(p[0] != 0xE9)
        return 0;
    segments = p[1];
    entry = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    p += 24;                                    /* the header and its extension */
    for(i=0; i<segments; ++i)
    {
        const uint32_t addr = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        const uint32_t len  = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        const uint32_t *src = (const uint32_t *)(p + 8);
        uint32_t *dst = (uint32_t *)addr;
        uint32_t n;
        p += 8 + len;
        if(p > end  ||  (len & 3u) != 0)
            return 0;
        for(n = len / 4; n != 0; --n)
            *dst++ = *src++;
    }
    return entry;
}


/*
 * Main
 */

/** The ROM arms the RTC watchdog for the boot (flash boot mode, about 0.4 s): off, as
    the port does first thing, so that a long hash is not a reset and a halt stays a halt.
    The application's port arms its own watchdog later. */
static void watchdogsOff(void)
{
    TIMG0_WDTWPROTECT = WDT_WKEY; TIMG0_WDTCONFIG0 = 0; TIMG0_WDTWPROTECT = 0;
    TIMG1_WDTWPROTECT = WDT_WKEY; TIMG1_WDTCONFIG0 = 0; TIMG1_WDTWPROTECT = 0;
    RTC_WDTWPROTECT = WDT_WKEY;   RTC_WDTCONFIG0 = 0;   RTC_WDTWPROTECT = 0;
}

void sel_main(void)
{
    const uint32_t t0 = ccount();
    int ruling;
    uint32_t slot, entry, tried = 0, tHash, tTable = 0, tVerify = 0;

    watchdogsOff();
    puts_("\r\nnornOS selector: ");
    ruling = readTable();
    if(ruling < 0)
    {
        puts_("no slot table\r\n");
        goto halt;
    }
    puts_("table "); putU32(_table.sequence); puts_(" slots "); putU32(_table.slot[0].state); putc_('/'); putU32(_table.slot[1].state); puts_(", ");
    slot = _table.active;
    for(;;)
    {
        ota_slot_t * const e = &_table.slot[slot];
        if(e->state == OTA_SLOT_TRIAL  &&  _table.trialsLeft == 0)
        {
            e->state = OTA_SLOT_REJECTED;       /* it never confirmed itself */
            puts_("slot "); putU32(slot); puts_(" rejected, ");
            _table.active = slot ^ 1u;
            writeTable(ruling); ruling = (ruling + 1) % OTA_TABLE_COPIES;
            slot = _table.active;
            if(++tried >= OTA_SLOTS) break;
            continue;
        }
        if(e->state == OTA_SLOT_TRIAL  ||  e->state == OTA_SLOT_CONFIRMED)
        {
            if(slotChecks(slot))
            {
                tVerify = ccount();
                if(slotSigned(slot))
                {
                    tVerify = ccount() - tVerify;
                    break;
                }
                puts_("slot "); putU32(slot); puts_(" bad signature, ");
            }
            else
            {
                puts_("slot "); putU32(slot); puts_(" fails its hash, ");
            }
            e->state = OTA_SLOT_REJECTED;
            writeTable(ruling); ruling = (ruling + 1) % OTA_TABLE_COPIES;
        }
        slot ^= 1u;
        if(++tried >= OTA_SLOTS) break;
    }
    if(tried >= OTA_SLOTS)
    {
        puts_("no bootable slot\r\n");
        goto halt;
    }
    tHash = ccount();
    if(_table.slot[slot].state == OTA_SLOT_TRIAL)
    {
        /* This boot counts. After the hash: the cache reads flash at half speed after the
           ROM's erase and write until the next reset (measured: hash 185 -> 390 ms). */
        -- _table.trialsLeft;
        writeTable(ruling); ruling = (ruling + 1) % OTA_TABLE_COPIES;
        tTable = ccount() - tHash;
        tHash = ccount();
    }
    entry = loadSlot(slot);
    if(entry == 0)
    {
        puts_("slot image unreadable\r\n");
        goto halt;
    }
    puts_("slot "); putU32(slot);
    puts_(_table.slot[slot].state == OTA_SLOT_TRIAL ? " (trial), " : ", ");
    putU32(_table.slot[slot].ramLength); puts_(" + "); putU32(_table.slot[slot].flashLength);
    {
        const uint32_t perMs = cyclesPerMs();
        puts_(" bytes, hash "); putU32((tHash - t0 - tTable - tVerify) / perMs); puts_(" ms, verify ");
        putU32(tVerify / perMs); puts_(" ms, load ");
        putU32((ccount() - tHash) / perMs); puts_(" ms");
        if(tTable != 0) { puts_(", table "); putU32(tTable / perMs); puts_(" ms"); }
        puts_(" at "); putU32(perMs / 1000u); puts_(" MHz");
    }
    puts_("\r\n");
    flush_();
    RTC_STORE2 = OTA_RTC_MAGIC | slot;
    RTC_STORE3 = slotBase(slot) + OTA_SLOT_FLASH_OFF;
    ((void (*)(void))entry)();
halt:
    flush_();
    for(;;)
        ;
}
