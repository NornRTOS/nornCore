/**
 * @file ota.c
 * hal/ota.h on the ESP32: the slot table (two copies, the selector's rules), the other
 * slot written sector by sector through the ROM's SPI flash driver with the cache off for
 * the duration of each operation (hal/xtensa/flash.c has the same dance), and SHA-256 in
 * software over what was written (the application runs at 240 MHz; 2 MB/s is plenty).
 */

#include "rtos.h"
#include "ota.h"
#ifdef PORT_OTA_SLOTS
#include "ota_layout.h"

#define RTC_STORE2      PORT_REG(DR_REG_RTCCNTL_BASE + 0x54)
#define RTC_STORE3      PORT_REG(DR_REG_RTCCNTL_BASE + 0x58)
#define ROM_CHIP_SIZE   (g_rom_flashchip[1])
#define IMAGE_MAGIC     0x41544F4Eul            /* "NOTA" */
#if OTA_STATE_TRIAL != OTA_SLOT_TRIAL || OTA_STATE_CONFIRMED != OTA_SLOT_CONFIRMED
# error ota.h and ota_layout.h disagree on the slot states
#endif
#define HEADER_BYTES    112u                    /* version 2: the signature after the digest */
#define PAGE            256u

extern int esp_rom_spiflash_read(uint32_t src, uint32_t *pDest, int32_t len);
extern int esp_rom_spiflash_write(uint32_t dest, const uint32_t *pSrc, int32_t len);
extern int esp_rom_spiflash_erase_sector(uint32_t sector);
extern uint32_t g_rom_flashchip[];
extern void Cache_Read_Disable_rom(int cpuNo);
extern void Cache_Read_Enable_rom(int cpuNo);
extern void Cache_Flush_rom(int cpuNo);


/*
 * Flash access, as hal/xtensa/flash.c
 */

static bool _cacheOff;

static rtos_port_cs_t begin(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
#if RTOS_CFG_SMP
    rtos_port_stallOtherCore();
#endif
    _cacheOff = rtos_port_flashPages() != 0;
    if(_cacheOff)
        Cache_Read_Disable_rom(0);
    return cs;
}

static void end(rtos_port_cs_t cs)
{
    if(_cacheOff)
    {
        Cache_Flush_rom(0);
        Cache_Read_Enable_rom(0);
#if RTOS_CFG_SMP
        Cache_Flush_rom(1);
#endif
    }
#if RTOS_CFG_SMP
    rtos_port_resumeOtherCore();
#endif
    rtos_port_exitCritical(cs);
}

static int flashRead(uint32_t addr, void *p, uint32_t n)      /* addr and n multiples of 4 */
{
    const rtos_port_cs_t cs = begin();
    const int r = esp_rom_spiflash_read(addr, (uint32_t *)p, (int32_t)n);
    end(cs);
    return r;
}

static int flashWrite(uint32_t addr, const void *p, uint32_t n)
{
    const rtos_port_cs_t cs = begin();
    const int r = esp_rom_spiflash_write(addr, (const uint32_t *)p, (int32_t)n);
    end(cs);
    return r;
}

static int flashErase(uint32_t addr)
{
    const rtos_port_cs_t cs = begin();
    const int r = esp_rom_spiflash_erase_sector(addr / OTA_SECTOR);
    end(cs);
    return r;
}


/*
 * CRC-32 and SHA-256
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

typedef struct { uint32_t h[8]; uint8_t block[64]; uint32_t fill; uint64_t total; } sha256_t;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void shaBlock(sha256_t *s)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, i;
    for(i=0; i<16; ++i)
        w[i] = ((uint32_t)s->block[4*i] << 24) | ((uint32_t)s->block[4*i+1] << 16) | ((uint32_t)s->block[4*i+2] << 8) | s->block[4*i+3];
    for(; i<64; ++i)
    {
        const uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        const uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for(i=0; i<64; ++i)
    {
        const uint32_t t1 = h + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        const uint32_t t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void shaInit(sha256_t *s)
{
    static const uint32_t h0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    uint32_t i;
    for(i=0; i<8; ++i) s->h[i] = h0[i];
    s->fill = 0; s->total = 0;
}

static void shaUpdate(sha256_t *s, const uint8_t *p, size_t n)
{
    s->total += n;
    while(n--)
    {
        s->block[s->fill++] = *p++;
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
    for(i=0; i<8; ++i) { out[4*i] = (uint8_t)(s->h[i] >> 24); out[4*i+1] = (uint8_t)(s->h[i] >> 16); out[4*i+2] = (uint8_t)(s->h[i] >> 8); out[4*i+3] = (uint8_t)s->h[i]; }
}


/*
 * The table
 */

static ota_table_t _table;
static int _ruling = -1;

static bool tableValid(const ota_table_t *t)
{
    return t->magic == OTA_TABLE_MAGIC  &&  t->version == OTA_TABLE_VERSION  &&  t->active < OTA_SLOTS
        &&  crc32((const uint8_t *)t, offsetof(ota_table_t, crc32)) == t->crc32;
}

/** The ruling copy into _table. */
static int readTable(void)
{
    static ota_table_t copy;
    int i;
    _ruling = -1;
    for(i=0; i<OTA_TABLE_COPIES; ++i)
    {
        if(flashRead(OTA_TABLE_ADDR + (uint32_t)i * OTA_SECTOR, &copy, (sizeof(copy) + 3u) & ~3u) != 0)
            return OTA_ERR_FLASH;
        if(tableValid(&copy)  &&  (_ruling < 0  ||  copy.sequence > _table.sequence))
        {
            _table = copy;
            _ruling = i;
        }
    }
    return _ruling >= 0 ? OTA_OK : OTA_ERR_TABLE;
}

/** _table into the copy that did not rule, with the next sequence number. */
static int writeTable(void)
{
    const uint32_t addr = OTA_TABLE_ADDR + (uint32_t)((_ruling + 1) % OTA_TABLE_COPIES) * OTA_SECTOR;
    ++ _table.sequence;
    _table.crc32 = crc32((const uint8_t *)&_table, offsetof(ota_table_t, crc32));
    if(flashErase(addr) != 0  ||  flashWrite(addr, &_table, (sizeof(_table) + 3u) & ~3u) != 0)
        return OTA_ERR_FLASH;
    _ruling = (_ruling + 1) % OTA_TABLE_COPIES;
    return OTA_OK;
}


/*
 * Running slot
 */

int ota_runningSlot(void)
{
    return (RTC_STORE2 & 0xFFFFFF00ul) == OTA_RTC_MAGIC ? (int)(RTC_STORE2 & 1u) : OTA_ERR_NO_SELECTOR;
}

int ota_runningState(void)
{
    const int slot = ota_runningSlot();
    int r;
    if(slot < 0)
        return slot;
    if((r = readTable()) != OTA_OK)
        return r;
    return (int)_table.slot[slot].state;
}

int ota_confirm(void)
{
    const int slot = ota_runningSlot();
    int r;
    if(slot < 0)
        return slot;
    if((r = readTable()) != OTA_OK)
        return r;
    if(_table.slot[slot].state == OTA_SLOT_CONFIRMED)
        return OTA_OK;
    _table.slot[slot].state = OTA_SLOT_CONFIRMED;
    _table.active = (uint32_t)slot;
    _table.trialsLeft = 0;
    return writeTable();
}


/*
 * Writing the other slot
 */

static struct
{
    int active;                 /**< a write is in progress */
    uint32_t slot;              /**< the slot being written */
    uint32_t base;              /**< its flash offset */
    uint8_t header[HEADER_BYTES];
    uint32_t headerFill;
    uint32_t ramLength, flashLength;
    uint32_t pos;               /**< image bytes consumed after the header */
    uint32_t nextErase;         /**< first flash address not yet erased in the current part */
    uint8_t page[PAGE];         /**< bytes waiting for a page write */
    uint32_t pageFill;
    uint32_t pageAddr;          /**< flash address the page starts at */
    sha256_t sha;
} _w;

static uint32_t partBase(void)
{
    return _w.base + (_w.pos < _w.ramLength ? 0u : OTA_SLOT_FLASH_OFF);
}

static uint32_t partOffset(void)
{
    return _w.pos < _w.ramLength ? _w.pos : _w.pos - _w.ramLength;
}

static int flushPage(void)
{
    uint32_t n = (_w.pageFill + 3u) & ~3u;
    if(_w.pageFill == 0)
        return OTA_OK;
    while(_w.pageFill < n)
        _w.page[_w.pageFill++] = 0xFF;
    while(_w.nextErase < _w.pageAddr + n)
    {
        if(flashErase(_w.nextErase) != 0)
            return OTA_ERR_FLASH;
        _w.nextErase += OTA_SECTOR;
    }
    if(flashWrite(_w.pageAddr, _w.page, n) != 0)
        return OTA_ERR_FLASH;
    _w.pageFill = 0;
    return OTA_OK;
}

int ota_begin(void)
{
    const int running = ota_runningSlot();
    int r;
    if(running < 0)
        return running;
    if((r = readTable()) != OTA_OK)
        return r;
    _w.slot = (uint32_t)running ^ 1u;
    _w.base = _w.slot == 0 ? OTA_SLOT_A : OTA_SLOT_B;
    _w.headerFill = 0; _w.pos = 0; _w.pageFill = 0; _w.ramLength = 0; _w.flashLength = 0;
    _w.nextErase = _w.base;
    _w.pageAddr = _w.base;
    _w.active = 1;
    if(ROM_CHIP_SIZE < OTA_SLOT_B + OTA_SLOT_SIZE)
        ROM_CHIP_SIZE = OTA_SLOT_B + OTA_SLOT_SIZE;    /* the ROM's idea of the chip may be smaller */
    shaInit(&_w.sha);
    return OTA_OK;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int ota_write(const void *data, size_t n)
{
    const uint8_t *p = data;
    if(!_w.active)
        return OTA_ERR_STATE;
    while(n != 0)
    {
        if(_w.headerFill < HEADER_BYTES)
        {
            _w.header[_w.headerFill++] = *p++; --n;
            if(_w.headerFill == HEADER_BYTES)
            {
                if(le32(_w.header) != IMAGE_MAGIC  ||  le32(_w.header + 4) != 2u)
                    return OTA_ERR_FORMAT;
                _w.ramLength = le32(_w.header + 8);
                _w.flashLength = le32(_w.header + 12);
                if(_w.ramLength == 0  ||  _w.ramLength > OTA_SLOT_FLASH_OFF
                   ||  _w.flashLength > OTA_SLOT_SIZE - OTA_SLOT_FLASH_OFF)
                    return OTA_ERR_FORMAT;
            }
            continue;
        }
        if(_w.pos >= _w.ramLength + _w.flashLength)
            return OTA_ERR_FORMAT;                      /* more than the header said */
        if(_w.pageFill == 0)
        {
            _w.pageAddr = partBase() + partOffset();
            if(partOffset() == 0)                       /* a part starts: its sectors are not erased yet */
                _w.nextErase = partBase();
        }
        {
            /* up to the page's end, the part's end and the caller's end */
            uint32_t take = PAGE - _w.pageFill;
            const uint32_t partLeft = (_w.pos < _w.ramLength ? _w.ramLength : _w.ramLength + _w.flashLength) - _w.pos;
            uint32_t i;
            if(take > partLeft) take = partLeft;
            if(take > n) take = (uint32_t)n;
            for(i=0; i<take; ++i)
                _w.page[_w.pageFill + i] = p[i];
            shaUpdate(&_w.sha, p, take);
            _w.pageFill += take; _w.pos += take; p += take; n -= take;
            if(_w.pageFill == PAGE  ||  _w.pos == _w.ramLength  ||  _w.pos == _w.ramLength + _w.flashLength)
            {
                const int r = flushPage();
                if(r != OTA_OK) { _w.active = 0; return r; }
            }
        }
    }
    return OTA_OK;
}

int ota_end(void)
{
    uint8_t digest[32];
    uint32_t i;
    int r;
    if(!_w.active)
        return OTA_ERR_STATE;
    _w.active = 0;
    if(_w.headerFill < HEADER_BYTES  ||  _w.pos != _w.ramLength + _w.flashLength)
        return OTA_ERR_FORMAT;
    if((r = flushPage()) != OTA_OK)
        return r;
    shaFinal(&_w.sha, digest);
    for(i=0; i<32; ++i)
        if(digest[i] != _w.header[16 + i])
            return OTA_ERR_HASH;
    if((r = readTable()) != OTA_OK)
        return r;
    _table.slot[_w.slot].state = OTA_SLOT_TRIAL;
    _table.slot[_w.slot].ramLength = _w.ramLength;
    _table.slot[_w.slot].flashLength = _w.flashLength;
    for(i=0; i<32; ++i)
        _table.slot[_w.slot].sha256[i] = digest[i];
    for(i=0; i<64; ++i)
        _table.slot[_w.slot].signature[i] = _w.header[48 + i];   /* checked by the selector at boot */
    _table.active = _w.slot;
    _table.trialsLeft = OTA_TRIALS;
    return writeTable();
}

void ota_abort(void)
{
    _w.active = 0;
}

uint32_t ota_written(void)
{
    return _w.pos;
}

#else   /* not an OTA build */

int ota_runningSlot(void)                      { return OTA_ERR_NO_SELECTOR; }
int ota_runningState(void)                     { return OTA_ERR_NO_SELECTOR; }
int ota_confirm(void)                          { return OTA_ERR_NO_SELECTOR; }
int ota_begin(void)                            { return OTA_ERR_NO_SELECTOR; }
int ota_write(const void *data, size_t n)      { (void)data; (void)n; return OTA_ERR_NO_SELECTOR; }
int ota_end(void)                              { return OTA_ERR_NO_SELECTOR; }
void ota_abort(void)                           { }
uint32_t ota_written(void)                     { return 0; }

#endif
