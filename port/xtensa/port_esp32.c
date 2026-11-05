/**
 * @file port_esp32.c
 * ESP32 port functions in C: early hardware setup, interrupt attach and dispatch, the
 * tick on CCOMPARE0, task stack preparation, time base and the fatal handler. The
 * assembler parts are in port_start.S, port_vectors.S and port_context.S.
 */

#include "rtos.h"
#include "rtos_kernel.h"
#include "rtos_assert.h"
#include "uart.h"


/*
 * Defines
 */

#define RTC_CNTL_OPTIONS0       PORT_REG(DR_REG_RTCCNTL_BASE + 0x00)
#define RTC_CNTL_RESET_STATE    PORT_REG(DR_REG_RTCCNTL_BASE + 0x34)
#define RTC_CNTL_SW_SYS_RST     (1ul << 31)
#define RTC_CNTL_WDTCONFIG0     PORT_REG(DR_REG_RTCCNTL_BASE + 0x8C)
#define RTC_CNTL_WDTWPROTECT    PORT_REG(DR_REG_RTCCNTL_BASE + 0xA4)
#define TIMG0_WDTCONFIG0        PORT_REG(DR_REG_TIMG0_BASE + 0x48)
#define TIMG0_WDTCONFIG1        PORT_REG(DR_REG_TIMG0_BASE + 0x4C)
#define TIMG0_WDTCONFIG2        PORT_REG(DR_REG_TIMG0_BASE + 0x50)
#define TIMG0_WDTFEED           PORT_REG(DR_REG_TIMG0_BASE + 0x60)
#define TIMG0_WDTWPROTECT       PORT_REG(DR_REG_TIMG0_BASE + 0x64)
#define WDT_EN                  (1ul << 31)
#define WDT_STG0_RESET_SYSTEM   (3ul << 29)
#define WDT_CPU_RESET_LEN_3US   (7ul << 18)
#define WDT_SYS_RESET_LEN_3US   (7ul << 15)
#ifndef HAL_APB_HZ
# define HAL_APB_HZ             F_CPU
#endif
#define TIMG1_WDTCONFIG0        PORT_REG(DR_REG_TIMG1_BASE + 0x48)
#define TIMG1_WDTWPROTECT       PORT_REG(DR_REG_TIMG1_BASE + 0x64)

/** Interrupt matrix: one register per peripheral source, one table per CPU. */
#define DPORT_PRO_INTR_MAP(src) PORT_REG(DR_REG_DPORT_BASE + 0x104 + 4ul * (src))
#define DPORT_APP_INTR_MAP(src) PORT_REG(DR_REG_DPORT_BASE + 0x218 + 4ul * (src))
#define DPORT_APPCPU_CTRL_A     PORT_REG(DR_REG_DPORT_BASE + 0x2C)     /* bit 0: resetting */
#define DPORT_APPCPU_CTRL_B     PORT_REG(DR_REG_DPORT_BASE + 0x30)     /* bit 0: clock gate */
#define DPORT_APPCPU_CTRL_C     PORT_REG(DR_REG_DPORT_BASE + 0x34)     /* bit 0: run stall */
#define DPORT_APPCPU_CTRL_D     PORT_REG(DR_REG_DPORT_BASE + 0x38)     /* boot address */
#define DPORT_CPU_INTR_FROM_CPU(n) PORT_REG(DR_REG_DPORT_BASE + 0xDC + 4ul * (n))
/** Cache bus masks: a set bit keeps the cache from serving that window. The DPORT names
    are the TRM's: IRAM0 is the instruction window at 0x400D0000, DROM0 the data window at
    0x3F400000; IROM0 and DRAM1 are unmasked too, as Espressif's bootloader does. */
#define DPORT_PRO_CACHE_CTRL1   PORT_REG(DR_REG_DPORT_BASE + 0x44)
#define DPORT_CACHE_MASK_IRAM0  (1ul << 0)
#define DPORT_CACHE_MASK_IROM0  (1ul << 2)
#define DPORT_CACHE_MASK_DRAM1  (1ul << 3)
#define DPORT_CACHE_MASK_DROM0  (1ul << 4)
#define FLASH_DROM_WINDOW       0x3F400000ul    /* data bus window into the flash cache */
#define FLASH_IROM_WINDOW       0x400D0000ul    /* instruction bus window */
#define FLASH_PAGE_SIZE         0x10000ul
#define RTC_CNTL_OPTIONS0_      PORT_REG(DR_REG_RTCCNTL_BASE + 0x00)
#define RTC_CNTL_SW_CPU_STALL   PORT_REG(DR_REG_RTCCNTL_BASE + 0xAC)

#define CYCLES_PER_TICK         (F_CPU / RTOS_CFG_TICK_HZ)
#define CYCLES_PER_US           (F_CPU / 1000000ul)

#if F_CPU % RTOS_CFG_TICK_HZ != 0
# error RTOS_CFG_TICK_HZ does not divide F_CPU exactly
#endif


/*
 * Local data
 */

static rtos_port_isr_t _isrTable[32];

/** CCOMPARE0 value of the next tick; the previous tick was one period earlier. */
uint32_t rtos_port_nextTick;
#define _nextTick   rtos_port_nextTick
/** Cycles of all completed ticks: kept in step with the tick count by the two places
    that count ticks, so that the run time counter is an addition, not a multiplication. */
rtos_port_runtime_t rtos_port_runBase;
static uint32_t _tickInterrupts;
static uint32_t _wdtTimeoutMs;


/*
 * Special register access
 */

#define DEFINE_RSR(name)                                                        \
    static inline uint32_t rsr_##name(void)                                     \
    {                                                                           \
        uint32_t v;                                                             \
        __asm__ __volatile__ ("rsr %0, " #name : "=a"(v));                      \
        return v;                                                               \
    }
#define DEFINE_WSR(name)                                                        \
    static inline void wsr_##name(uint32_t v)                                   \
    {                                                                           \
        __asm__ __volatile__ ("wsr %0, " #name "\n\trsync" : : "a"(v) : "memory"); \
    }

DEFINE_RSR(interrupt)
DEFINE_RSR(intenable)
DEFINE_WSR(intenable)
DEFINE_WSR(intclear)
DEFINE_WSR(ccompare0)


/*
 * Function implementation
 */

/* Linker symbols of esp32.ld: their addresses are the values. */
extern uint8_t _flash_pages, _flash_lma_start, FLASH_BASE;

/* ROM functions (esp32_rom.ld). The MMU has one 64 KB page table per CPU; the cache of
   the PRO CPU must be off while the table is written. */
extern void mmu_init(int cpuNo);
extern unsigned int cache_flash_mmu_set_rom(int cpuNo, int pid, unsigned int vaddr, unsigned int paddr, int psize, int num);
extern void Cache_Flush_rom(int cpuNo);
extern void Cache_Read_Disable_rom(int cpuNo);
extern void Cache_Read_Enable_rom(int cpuNo);


void rtos_port_flashInit(void)
{
    const uint32_t pages = (uint32_t)&_flash_pages;
    const uint32_t base = (uint32_t)&FLASH_BASE;

    if(pages == 0)
        return;
    RTOS_CHECK((uint32_t)&_flash_lma_start == base  &&  pages * FLASH_PAGE_SIZE <= 0x300000ul, base, pages);
    Cache_Read_Disable_rom(0);
    Cache_Flush_rom(0);
    mmu_init(0);
    /* The same flash pages behind both windows: .flash.rodata is read through the first,
       .flash.text fetched through the second, at the offsets esp32.ld gave them. */
    (void)cache_flash_mmu_set_rom(0, 0, FLASH_DROM_WINDOW, base, 64, (int)pages);
    (void)cache_flash_mmu_set_rom(0, 0, FLASH_IROM_WINDOW, base, 64, (int)pages);
    DPORT_PRO_CACHE_CTRL1 &= ~(DPORT_CACHE_MASK_IRAM0 | DPORT_CACHE_MASK_IROM0 | DPORT_CACHE_MASK_DROM0 | DPORT_CACHE_MASK_DRAM1);
    Cache_Read_Enable_rom(0);
}


uint32_t rtos_port_flashPages(void)
{
    return (uint32_t)&_flash_pages;
}


void rtos_port_earlyInit(void)
{
    if(rtos_port_coreId() != 0)
        return;                         /* the PRO CPU has set up clocks and watchdogs */

    /* The ROM loader may leave a watchdog running; none is wanted until the kernel owns
       them. Both timer group watchdogs and the RTC watchdog. */
    TIMG0_WDTWPROTECT = WDT_WKEY;
    TIMG0_WDTCONFIG0  = 0;
    TIMG0_WDTWPROTECT = 0;
    TIMG1_WDTWPROTECT = WDT_WKEY;
    TIMG1_WDTCONFIG0  = 0;
    TIMG1_WDTWPROTECT = 0;
    RTC_CNTL_WDTWPROTECT = WDT_WKEY;
    RTC_CNTL_WDTCONFIG0  = 0;
    RTC_CNTL_WDTWPROTECT = 0;

    rtos_port_clockInit();
    rtos_port_flashInit();
}


void rtos_port_reset(void)
{
    (void)rtos_port_enterCritical();
    RTC_CNTL_OPTIONS0 |= RTC_CNTL_SW_SYS_RST;
    for(;;)
        ;
}


uint8_t rtos_port_getResetCause(void)
{
    /* RTC_CNTL_RESET_STATE_REG bits 5..0: reset cause of the PRO CPU (ESP32 TRM). */
    switch(RTC_CNTL_RESET_STATE & 0x3Fu)
    {
    case 1:  return RTOS_RESET_POWER_ON;
    case 3:  return RTOS_RESET_SOFTWARE;      /* SW_SYS_RESET */
    case 13: return RTOS_RESET_SOFTWARE;      /* SW_CPU_RESET */
    case 7:  return RTOS_RESET_WATCHDOG;      /* TG0WDT_SYS_RESET */
    case 8:  return RTOS_RESET_WATCHDOG;      /* TG1WDT_SYS_RESET */
    case 9:  return RTOS_RESET_WATCHDOG;      /* RTCWDT_SYS_RESET */
    case 12: return RTOS_RESET_WATCHDOG;      /* TGWDT_CPU_RESET */
    case 14: return RTOS_RESET_WATCHDOG;      /* RTCWDT_CPU_RESET */
    case 17: return RTOS_RESET_WATCHDOG;      /* RTCWDT_RTC_RESET */
    case 15: return RTOS_RESET_EXTERNAL;      /* EXT_CPU_RESET */
    case 16: return RTOS_RESET_BROWN_OUT;     /* RTCWDT_BROWN_OUT_RESET */
    default: return RTOS_RESET_UNKNOWN;
    }
}


void rtos_port_wdtStart(uint32_t timeoutMs)
{
    /* MWDT0 of timer group 0 counts APB clock / prescale; 16 bit prescale, so above
       65 MHz APB the counter runs at 10 kHz instead of 1 kHz. Stage 0 resets the system. */
    uint32_t prescale = HAL_APB_HZ / 1000ul;
    uint32_t ticks = timeoutMs;
    if(prescale > 0xFFFFul)
    {
        prescale = HAL_APB_HZ / 10000ul;
        ticks = timeoutMs * 10ul;
    }
    _wdtTimeoutMs = timeoutMs;
    TIMG0_WDTWPROTECT = WDT_WKEY;
    TIMG0_WDTCONFIG0 = 0;
    TIMG0_WDTCONFIG1 = prescale << 16;
    TIMG0_WDTCONFIG2 = ticks;
    TIMG0_WDTCONFIG0 = WDT_EN | WDT_STG0_RESET_SYSTEM | WDT_CPU_RESET_LEN_3US | WDT_SYS_RESET_LEN_3US;
    TIMG0_WDTFEED = 1;
    TIMG0_WDTWPROTECT = 0;
}


void rtos_port_wdtKick(void)
{
    TIMG0_WDTWPROTECT = WDT_WKEY;
    TIMG0_WDTFEED = 1;
    TIMG0_WDTWPROTECT = 0;
}


void rtos_port_fatal(uint32_t cause, uint32_t epc, uint32_t vaddr)
{
    uint8_t i;

    /* A fault before main leaves the UART at the ROM's divider, wrong since the clock
       switch: program ours so that the report is readable. */
    uart_init(uart_currentBaud());
    uart_puts("\r\nFATAL exccause=");
    uart_putU32(cause);
    uart_puts(" epc=0x");
    for(i=0; i<4; ++i)
        uart_putHex8((uint8_t)(epc >> (24 - 8*i)));
    uart_puts(" vaddr=0x");
    for(i=0; i<4; ++i)
        uart_putHex8((uint8_t)(vaddr >> (24 - 8*i)));
    uart_puts("\r\n");
    rtos_port_halt();
}


/* ---- interrupts ---------------------------------------------------------------------- */

void rtos_port_attachInterrupt(uint8_t cpuInt, uint8_t source, rtos_port_isr_t handler)
{
    rtos_port_cs_t cs;

    RTOS_CHECK(cpuInt < 32  &&  (PORT_LEVEL1_INT_MASK & (1ul << cpuInt)) != 0
                &&  handler != NULL, cpuInt, source);
    cs = rtos_port_enterCritical();
    _isrTable[cpuInt] = handler;
    if(source != RTOS_PORT_SRC_NONE)
    {
        if(rtos_port_coreId() == 0)
            DPORT_PRO_INTR_MAP(source) = cpuInt;
        else
            DPORT_APP_INTR_MAP(source) = cpuInt;
    }
    wsr_intenable(rsr_intenable() | (1ul << cpuInt));
    rtos_port_exitCritical(cs);
}


/** Runs every pending level 1 handler; interrupts masked. Inline: one frame less on the
    interrupted task's stack for the whole handler. */
static inline __attribute__((always_inline)) void dispatchLevel1(void)
{
    for(;;)
    {
        const uint32_t pending = rsr_interrupt() & rsr_intenable() & PORT_LEVEL1_INT_MASK;
        uint8_t n;
        rtos_port_isr_t handler;

        if(pending == 0)
            return;
        n = (uint8_t)__builtin_ctz(pending);
        wsr_intclear(1ul << n);        /* edge and software sources; no effect on others */
        handler = _isrTable[n];
        if(handler == NULL)
            rtos_port_fatal(0x1000ul + n, 0, 0);
        handler();
    }
}


/* ---- FPU ----------------------------------------------------------------------------- */

typedef struct fpuState
{
    uint32_t f[16];
    uint32_t fcr;
    uint32_t fsr;
} fpuState_t;

static fpuState_t _fpu[RTOS_NUM_TASKS];
static uint8_t _fpuOwner = RTOS_NO_TASK;
static uint32_t _fpuSwitches = 0;
static uint8_t _isrDepth = 0;

static inline void fpuEnable(void)
{
    __asm__ __volatile__ ("movi a2, 1\n\twsr a2, cpenable\n\trsync" : : : "a2", "memory");
}

static void fpuSave(fpuState_t *p)
{
    __asm__ __volatile__ (
        "ssi f0, %0, 0\n\t"   "ssi f1, %0, 4\n\t"   "ssi f2, %0, 8\n\t"   "ssi f3, %0, 12\n\t"
        "ssi f4, %0, 16\n\t"  "ssi f5, %0, 20\n\t"  "ssi f6, %0, 24\n\t"  "ssi f7, %0, 28\n\t"
        "ssi f8, %0, 32\n\t"  "ssi f9, %0, 36\n\t"  "ssi f10, %0, 40\n\t" "ssi f11, %0, 44\n\t"
        "ssi f12, %0, 48\n\t" "ssi f13, %0, 52\n\t" "ssi f14, %0, 56\n\t" "ssi f15, %0, 60\n\t"
        "rur.fcr a2\n\t" "s32i a2, %0, 64\n\t"
        "rur.fsr a2\n\t" "s32i a2, %0, 68\n\t"
        : : "a"(p) : "a2", "memory");
}

static void fpuRestore(const fpuState_t *p)
{
    __asm__ __volatile__ (
        "lsi f0, %0, 0\n\t"   "lsi f1, %0, 4\n\t"   "lsi f2, %0, 8\n\t"   "lsi f3, %0, 12\n\t"
        "lsi f4, %0, 16\n\t"  "lsi f5, %0, 20\n\t"  "lsi f6, %0, 24\n\t"  "lsi f7, %0, 28\n\t"
        "lsi f8, %0, 32\n\t"  "lsi f9, %0, 36\n\t"  "lsi f10, %0, 40\n\t" "lsi f11, %0, 44\n\t"
        "lsi f12, %0, 48\n\t" "lsi f13, %0, 52\n\t" "lsi f14, %0, 56\n\t" "lsi f15, %0, 60\n\t"
        "l32i a2, %0, 64\n\t" "wur.fcr a2\n\t"
        "l32i a2, %0, 68\n\t" "wur.fsr a2\n\t"
        : : "a"(p) : "a2", "memory");
}

/** First FPU instruction of the current task since the last switch. */
static void fpuTrap(uint32_t *pFrame)
{
    const uint8_t cur = rtos_pCurrent->prio;

    if(_isrDepth != 0)
        rtos_panic(RTOS_PANIC_FAULT, EXCCAUSE_CP0_DISABLED, pFrame[PORT_FP_PC / 4]);
    fpuEnable();
    if(_fpuOwner != cur)
    {
        if(_fpuOwner != RTOS_NO_TASK)
            fpuSave(&_fpu[_fpuOwner]);
        fpuRestore(&_fpu[cur]);
        _fpuOwner = cur;
        ++ _fpuSwitches;
    }
    pFrame[PORT_FP_CPENABLE / 4] = 1;   /* enabled when the frame is restored */
}


uint32_t rtos_port_fpuSwitchCount(void)
{
    return _fpuSwitches;
}


void rtos_port_exception(uint32_t cause, uint32_t *pFrame)
{
    if(cause == EXCCAUSE_LEVEL1_INTERRUPT)
    {
        ++ _isrDepth;
        dispatchLevel1();
        -- _isrDepth;
        rtos_kernel_isrTailInline();        /* the switch is called from this frame */
        /* Back in the frame of whichever task resumes here. Its saved CPENABLE may say
           on, but other tasks may have used the FPU meanwhile: only the owner of the
           registers may continue without the trap. */
        if(_fpuOwner != rtos_pCurrent->prio)
            pFrame[PORT_FP_CPENABLE / 4] = 0;
    }
    else if(cause == EXCCAUSE_CP0_DISABLED)
        fpuTrap(pFrame);
    else
        rtos_port_fatal(cause, pFrame[PORT_FP_PC / 4], 0);
}


/* ---- AMP ----------------------------------------------------------------------------- */

/* The memory the APP CPU's image may be loaded into: its halves of IRAM and DRAM
   (port/xtensa/mem_core1.ld). Anything else in a segment header is a corrupt image. */
#define CORE1_IRAM_START    0x40090000ul
#define CORE1_IRAM_END      0x400A0000ul
#define CORE1_DRAM_START    0x3FFC8000ul
#define CORE1_DRAM_END      0x3FFDC000ul

static bool core1Window(uint32_t addr, uint32_t len)
{
    return (addr >= CORE1_IRAM_START  &&  len <= CORE1_IRAM_END - addr)
           ||  (addr >= CORE1_DRAM_START  &&  len <= CORE1_DRAM_END - addr);
}

void rtos_port_startAppCpu(const uint8_t *image, uint32_t size)
{
    /* esptool image: 8 byte header (magic 0xE9, segment count, flash mode, flash size and
       speed, entry) plus 16 bytes of extended header, then segments of (load address,
       length, data). IRAM only takes 32 bit accesses, so the copy is word by word. The
       image is trusted for nothing: every segment must lie inside the image, be a whole
       number of words, and land inside the APP CPU's memory, or the start is refused
       (rtos_port_fatal, cause 0x2000 + the segment number, with the offending address
       and length), before anything is written. */
    uint32_t entry, off = 24;
    uint8_t segments, i;

    if(image == NULL  ||  size < 24u  ||  image[0] != 0xE9)
        rtos_port_fatal(0x2000ul, 0, size);
    entry = *(const uint32_t *)(image + 4);
    segments = image[1];
    if(entry < CORE1_IRAM_START  ||  entry >= CORE1_IRAM_END)
        rtos_port_fatal(0x2000ul, entry, 0);

    /* Pass 1: check every segment header against the image and the memory windows. */
    for(i=0; i<segments; ++i)
    {
        uint32_t addr, len;
        if(off > size - 8u)
            rtos_port_fatal(0x2001ul + i, off, size);
        addr = *(const uint32_t *)(image + off);
        len = *(const uint32_t *)(image + off + 4);
        if((len & 3u) != 0  ||  (addr & 3u) != 0  ||  len > size - off - 8u  ||  !core1Window(addr, len))
            rtos_port_fatal(0x2001ul + i, addr, len);
        off += 8u + len;
    }

    /* Pass 2: copy. */
    off = 24;
    for(i=0; i<segments; ++i)
    {
        const uint32_t addr = *(const uint32_t *)(image + off);
        const uint32_t len = *(const uint32_t *)(image + off + 4);
        volatile uint32_t *dst = (volatile uint32_t *)addr;
        const uint8_t *src = image + off + 8;
        uint32_t k;

        for(k=0; k<len; k+=4)
            dst[k / 4] = (uint32_t)src[k] | ((uint32_t)src[k + 1] << 8)
                         | ((uint32_t)src[k + 2] << 16) | ((uint32_t)src[k + 3] << 24);
        off += 8u + len;
    }

    /* Release the APP CPU: no stall, clock on, reset pulse, boot address. */
    RTC_CNTL_SW_CPU_STALL &= ~(0x3Ful << 20);
    RTC_CNTL_OPTIONS0_ &= ~3ul;
    DPORT_APPCPU_CTRL_B |= 1ul;
    DPORT_APPCPU_CTRL_C &= ~1ul;
    DPORT_APPCPU_CTRL_A |= 1ul;
    DPORT_APPCPU_CTRL_A &= ~1ul;
    DPORT_APPCPU_CTRL_D = entry;
}


void rtos_port_signalOtherCore(void)
{
    __asm__ __volatile__ ("memw" : : : "memory");
    DPORT_CPU_INTR_FROM_CPU(rtos_port_coreId() == 0 ? 1u : 0u) = 1;
}


void rtos_port_ackCrossCore(void)
{
    DPORT_CPU_INTR_FROM_CPU(rtos_port_coreId()) = 0;
}


/* ---- tick ---------------------------------------------------------------------------- */

static void tickIsr(void)
{
    ++ _tickInterrupts;
    /* The timer interrupt is cleared by writing CCOMPARE0, which fires on equality only:
       if the new value is already behind CCOUNT the tick is counted at once. After a
       tickless sleep several periods have passed: all of them are counted here. */
    do
    {
        _nextTick += CYCLES_PER_TICK;
        rtos_port_runBase += CYCLES_PER_TICK;
        wsr_ccompare0(_nextTick);
        rtos_tickFromIsr();
    }
    while((int32_t)(_nextTick - rtos_port_ccount()) <= 0);
}


uint32_t rtos_port_tickInterrupts(void)
{
    return _tickInterrupts;
}


void rtos_port_idleSleepTicks(rtos_tick_t ticks)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    uint32_t n = ticks == RTOS_FOREVER ? 1000ul : (uint32_t)ticks;

    /* Never sleep past half the watchdog timeout, and not for very long anyway, so that
       the 32 bit cycle arithmetic stays well defined. */
    if(_wdtTimeoutMs != 0  &&  n > _wdtTimeoutMs * RTOS_CFG_TICK_HZ / 2000ul)
        n = (uint32_t)(_wdtTimeoutMs * RTOS_CFG_TICK_HZ / 2000ul);
    if(n > 1000ul)
        n = 1000ul;
    if(n > 1)
        wsr_ccompare0(_nextTick + (n - 1u) * CYCLES_PER_TICK);

    /* waiti lowers the interrupt level and waits in one instruction: an interrupt that
       is already pending is taken at once and we return here after it. */
    __asm__ __volatile__ ("waiti 0" : : : "memory");

    /* Back with interrupts masked again. Count the ticks that passed while asleep if an
       interrupt other than the tick woke us, and put the compare back on the grid. */
    (void)rtos_port_enterCritical();
    while((int32_t)(_nextTick - rtos_port_ccount()) <= 0)
    {
        rtos_tickFromIsr();
        _nextTick += CYCLES_PER_TICK;
        rtos_port_runBase += CYCLES_PER_TICK;
    }
    wsr_ccompare0(_nextTick);
    rtos_kernel_isrTailInline();        /* a caught up tick may have readied a task */
    rtos_port_exitCritical(cs);
}


void rtos_port_startTimer(void)
{
    _nextTick = rtos_port_ccount() + CYCLES_PER_TICK;
    wsr_ccompare0(_nextTick);
    rtos_port_attachInterrupt(PORT_INT_TIMER0, RTOS_PORT_SRC_NONE, tickIsr);
}


/* ---- time ---------------------------------------------------------------------------- */



uint32_t rtos_getTimeUs(void)
{
    rtos_port_runtime_t t;
    const rtos_port_cs_t cs = rtos_port_enterCritical();

    if(_nextTick == 0)
        t = rtos_port_ccount();         /* timer not started: cycle counter only */
    else
        t = rtos_port_getRunTimeCounter();
    rtos_port_exitCritical(cs);
    return (uint32_t)(t / CYCLES_PER_US);
}


/* ---- tasks --------------------------------------------------------------------------- */

extern void _rtos_port_taskStart(void);

rtos_port_ctx_t rtos_port_initStack(uint8_t *stackArea, size_t stackSize, void (*entry)(void))
{
    /* The task starts by a retw from rtos_port_switchContext with a window increment of
       1: the return address is _rtos_port_taskStart, and the underflow that follows
       loads the fake caller frame below sp, which gives the task its stack pointer. */
    uint32_t * const top = (uint32_t *)(stackArea + stackSize);
    uint32_t * const sp = top - PORT_INIT_FRMSZ / 4;

    (void)entry;                        /* the trampoline is called by _rtos_port_taskStart */
    sp[-4] = 0;                         /* fake caller a0: no return */
    sp[-3] = (uint32_t)top;             /* fake caller a1: stack pointer of the task */
    sp[-2] = 0;
    sp[-1] = 0;
    sp[0] = ((uint32_t)_rtos_port_taskStart & 0x3FFFFFFFul) | 0x40000000ul;
    sp[1] = PS_WOE | PS_UM | PS_INTLEVEL(PORT_INTLEVEL_CRITICAL);
    sp[2] = 0;
    sp[3] = 0;
    return sp;
}
