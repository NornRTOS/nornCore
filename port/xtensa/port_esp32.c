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
#define DPORT_APP_CACHE_CTRL1   PORT_REG(DR_REG_DPORT_BASE + 0x5C)
#define DPORT_CACHE_MASK_IRAM0  (1ul << 0)
#define DPORT_CACHE_MASK_IROM0  (1ul << 2)
#define DPORT_CACHE_MASK_DRAM1  (1ul << 3)
#define DPORT_CACHE_MASK_DROM0  (1ul << 4)
#define DPORT_CACHE_MMU_IA_CLR  (1ul << 13)    /* clears the cache's "MMU invalid access" state */
#define FLASH_DROM_WINDOW       0x3F400000ul    /* data bus window into the flash cache */
#define FLASH_IROM_WINDOW       0x400D0000ul    /* instruction bus window */
#define FLASH_PAGE_SIZE         0x10000ul
#define RTC_CNTL_OPTIONS0_      PORT_REG(DR_REG_RTCCNTL_BASE + 0x00)
#define RTC_CNTL_SW_CPU_STALL   PORT_REG(DR_REG_RTCCNTL_BASE + 0xAC)

#define CYCLES_PER_TICK         (F_CPU / RTOS_CFG_TICK_HZ)
#define CYCLES_PER_US           (F_CPU / 1000000ul)

/* SMP: TIMG1 timer 0 as the shared run time clock, and the FROM_CPU software interrupts
   (sources 24 and 25) as IPIs, CPU interrupt 13 on each core. */
#define TIMG1_T0CONFIG          PORT_REG(DR_REG_TIMG1_BASE + 0x00)
#define TIMG1_T0LO              PORT_REG(DR_REG_TIMG1_BASE + 0x04)
#define TIMG1_T0HI              PORT_REG(DR_REG_TIMG1_BASE + 0x08)
#define TIMG1_T0UPDATE          PORT_REG(DR_REG_TIMG1_BASE + 0x0C)
#define TIMG1_T0LOADLO          PORT_REG(DR_REG_TIMG1_BASE + 0x18)
#define TIMG1_T0LOADHI          PORT_REG(DR_REG_TIMG1_BASE + 0x1C)
#define TIMG1_T0LOAD            PORT_REG(DR_REG_TIMG1_BASE + 0x20)
#define T0_EN                   (1ul << 31)
#define T0_INCREASE             (1ul << 30)
#define T0_DIVIDER_SHIFT        13
#define CPU_INT_IPI             13u

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
#if RTOS_CFG_SMP
    /* The APP CPU's table and window masks as well, here where the caches are off, as
       Espressif's bootloader does; rtos_port_startCore turns its cache on. Its table is
       random after reset, with the invalid bit set in many entries: an access that strays
       past the mapped pages (the line fill at the end of a region, a read past a constant)
       lands on page 0 through the PRO's zeroed table but stops the APP CPU for good with
       the cache's "MMU invalid access" flag (2026-11-18: core 1 frozen inside the Wi-Fi
       library's transmit). mmu_init(1) zeroes it like the PRO's. */
    mmu_init(1);
    (void)cache_flash_mmu_set_rom(1, 0, FLASH_DROM_WINDOW, base, 64, (int)pages);
    (void)cache_flash_mmu_set_rom(1, 0, FLASH_IROM_WINDOW, base, 64, (int)pages);
    DPORT_APP_CACHE_CTRL1 &= ~(DPORT_CACHE_MASK_IRAM0 | DPORT_CACHE_MASK_IROM0 | DPORT_CACHE_MASK_DROM0 | DPORT_CACHE_MASK_DRAM1);
    /* The APP cache comes out of reset with its "MMU invalid access" flag set (bit 0 of
       DPORT 0x418, its debug register 0; the PRO's is clear after the ROM has run). With
       the flag standing, the APP CPU's first access to a flash window hangs for good
       instead of being served, whatever the tables and masks say. A pulse of this bit
       clears it (found 2026-11-18 with a probe, nornInternal/nornCore/measurements/step17-flash-core1.md). */
    DPORT_APP_CACHE_CTRL1 |= DPORT_CACHE_MMU_IA_CLR;
    __asm__ __volatile__ ("memw" : : : "memory");
    DPORT_APP_CACHE_CTRL1 &= ~DPORT_CACHE_MMU_IA_CLR;
#endif
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
#if RTOS_CFG_SMP
    rtos_port_lockAcquire();                /* masked since _start: the lock goes with it */
#endif
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


/* SMP builds (PORT_FATAL_TRACE_DEPTH, port.h): _fatalEntry (port_vectors.S) kept the
   faulting window's registers here and spilled every window, so each frame's a0 (return
   address, window bits on top) and its caller's sp lie in the base save area below its
   sp, as Espressif's backtrace reads them. */
#if PORT_FATAL_TRACE_DEPTH > 0
uint32_t rtos_port_fatalRegs[16];

/* The fatal entry of the user exception vector (port_vectors.S has the weak default):
   keeps the faulting window's a0..a15 (a0 is in EXCSAVE1), leaves exception mode, spills
   every live window so that the frames can be walked (the SPILL_ALL_WINDOWS of
   port_context.S; a2..a4 of this window survive the rotation) and reports. */
#define FATAL_STR_(x) #x
#define FATAL_STR(x) FATAL_STR_(x)
__asm__(
    "    .section .text.rtos_port_fatalEntryUser, \"ax\"\n"
    "    .literal_position\n"
    "    .align  4\n"
    "    .global _fatalEntryUser\n"
    "_fatalEntryUser:\n"
    "    rsr     a0, excsave1\n"
    "    wsr     a2, excsave2\n"
    "    movi    a2, rtos_port_fatalRegs\n"
    "    s32i    a0, a2, 0\n"
    "    s32i    a1, a2, 4\n"
    "    rsr     a0, excsave2\n"
    "    s32i    a0, a2, 8\n"
    "    s32i    a3, a2, 12\n"
    "    s32i    a4, a2, 16\n"
    "    s32i    a5, a2, 20\n"
    "    s32i    a6, a2, 24\n"
    "    s32i    a7, a2, 28\n"
    "    s32i    a8, a2, 32\n"
    "    s32i    a9, a2, 36\n"
    "    s32i    a10, a2, 40\n"
    "    s32i    a11, a2, 44\n"
    "    s32i    a12, a2, 48\n"
    "    s32i    a13, a2, 52\n"
    "    s32i    a14, a2, 56\n"
    "    s32i    a15, a2, 60\n"
    "    rsr     a2, exccause\n"
    "    rsr     a3, epc1\n"
    "    rsr     a4, excvaddr\n"
    "    movi    a0, " FATAL_STR(PS_WOE | PS_UM) "\n"
    "    wsr     a0, ps\n"
    "    rsync\n"
    "    and     a12, a12, a12\n"
    "    rotw    3\n"
    "    and     a12, a12, a12\n"
    "    rotw    3\n"
    "    and     a12, a12, a12\n"
    "    rotw    3\n"
    "    and     a12, a12, a12\n"
    "    rotw    3\n"
    "    and     a12, a12, a12\n"
    "    rotw    4\n"
    "    mov     a6, a2\n"
    "    mov     a7, a3\n"
    "    mov     a8, a4\n"
    "    call4   rtos_port_fatal\n"
    "1:  j       1b\n");

static void putHex32(uint32_t v)
{
    uint8_t i;
    for(i=0; i<4; ++i)
        uart_putHex8((uint8_t)(v >> (24 - 8*i)));
}

static void fatalTrace(uint32_t epc)
{
    uint32_t a0 = rtos_port_fatalRegs[0], sp = rtos_port_fatalRegs[1];
    uint8_t i;
    uart_puts("regs a2..a7:");
    for(i = 2; i <= 7; ++i)
    {
        uart_puts(" 0x");
        putHex32(rtos_port_fatalRegs[i]);
    }
    uart_puts("\r\nbacktrace: 0x");
    putHex32(epc);
    for(i = 0; i < PORT_FATAL_TRACE_DEPTH; ++i)
    {
        if((sp & 3u) != 0  ||  sp < 0x3FFAE000ul  ||  sp >= 0x40000000ul  ||  (a0 >> 30) == 0)
            break;
        uart_puts(" 0x");
        putHex32((a0 & 0x3FFFFFFFul) | 0x40000000ul);
        a0 = *(const uint32_t *)(sp - 16);
        sp = *(const uint32_t *)(sp - 12);
    }
    uart_puts("\r\n");
}
#endif

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
#if PORT_FATAL_TRACE_DEPTH > 0
    fatalTrace(epc);
#endif
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
static uint32_t _fpuSwitches = 0;
#if RTOS_CFG_SMP
/* Per core: the task whose state is in this core's FPU registers (RTOS_NO_TASK when it
   was saved at the task's switch out, so that it may continue on the other core), and
   the interrupt depth. */
static uint8_t _fpuOwnerOf[RTOS_PORT_CORES] = { RTOS_NO_TASK, RTOS_NO_TASK };
static uint8_t _isrDepthOf[RTOS_PORT_CORES];
# define _fpuOwner  _fpuOwnerOf[rtos_port_coreId()]
# define _isrDepth  _isrDepthOf[rtos_port_coreId()]
#else
static uint8_t _fpuOwner = RTOS_NO_TASK;
static uint8_t _isrDepth = 0;
#endif

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


bool rtos_port_inIsr(void)
{
    return _isrDepth != 0;
}


#if RTOS_CFG_TICKLESS
static volatile bool _sleeping;             /* defined with the tick code below */
static void catchUpTicks(void);
#endif

void rtos_port_exception(uint32_t cause, uint32_t *pFrame)
{
    if(cause == EXCCAUSE_LEVEL1_INTERRUPT)
    {
        ++ _isrDepth;
#if RTOS_CFG_SMP
        rtos_port_lockAcquire();            /* masked by the hardware: the lock follows */
#endif
#if RTOS_CFG_TICKLESS
        if(_sleeping)                       /* the end of a tickless sleep: ticks first */
        {
            _sleeping = false;
            catchUpTicks();
        }
#endif
        dispatchLevel1();
        -- _isrDepth;
        rtos_kernel_isrTailInline();        /* the switch is called from this frame */
#if RTOS_CFG_SMP
        rtos_port_lockRelease();            /* here again when the interrupted task resumes */
#endif
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

/** Release the APP CPU into \a entry: no stall, clock on, reset pulse, boot address. */
static void releaseAppCpu(uint32_t entry)
{
    RTC_CNTL_SW_CPU_STALL &= ~(0x3Ful << 20);
    RTC_CNTL_OPTIONS0_ &= ~3ul;
    DPORT_APPCPU_CTRL_B |= 1ul;
    DPORT_APPCPU_CTRL_C &= ~1ul;
    DPORT_APPCPU_CTRL_A |= 1ul;
    DPORT_APPCPU_CTRL_A &= ~1ul;
    DPORT_APPCPU_CTRL_D = entry;
}

static bool core1Window(uint32_t addr, uint32_t len)
{
    /* addr <= END before the subtraction: an address above a window would otherwise
       underflow END - addr to a huge value and pass. */
    return (addr >= CORE1_IRAM_START  &&  addr <= CORE1_IRAM_END  &&  len <= CORE1_IRAM_END - addr)
           ||  (addr >= CORE1_DRAM_START  &&  addr <= CORE1_DRAM_END  &&  len <= CORE1_DRAM_END - addr);
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

    releaseAppCpu(entry);
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


/* ---- SMP (nornInternal/nornCore/smp_plan.md, step 7) --------------------------------------------------- */

#if RTOS_CFG_SMP

/* The kernel lock (port.h has the fast path). A test-and-set lock has no bound on the
   wait of a loser, unlike the ticket lock it replaced (2026-11-18, step16-lock.md: 62
   against 36 cycles for a section); with two cores and a holder that does work between
   its sections the waiter wins in practice, and tc12 / tc17 keep watch over the latency. */
volatile uint32_t rtos_port_lockWord;

void rtos_port_lockSpin(void)
{
    uint32_t got;
    do
    {
        while(rtos_port_lockWord != 0u)     /* read until free: no bus traffic of S32C1I meanwhile */
            ;
        got = 1u;
        __asm__ __volatile__ ("movi a8, 0\n\twsr a8, scompare1\n\ts32c1i %0, %1, 0"
                              : "+a"(got) : "a"(&rtos_port_lockWord) : "a8", "memory");
    }
    while(got != 0u);
}

rtos_port_runtime_t rtos_port_getRunTimeCounter(void)
{
    uint32_t lo, hi;
    TIMG1_T0UPDATE = 1;                     /* latch lo and hi together */
    /* The write is posted: a read of the group before LO makes sure the latch has taken
       place, or LO is the value of the previous latch, milliseconds old (measured
       2026-11-18: this read alone turned a 1 ms "IPI delay" into 5 us). */
    (void)TIMG1_T0CONFIG;
    lo = TIMG1_T0LO;
    hi = TIMG1_T0HI;
    return ((rtos_port_runtime_t)hi << 32) | lo;
}

#define DPORT_PERIP_CLK_EN      PORT_REG(DR_REG_DPORT_BASE + 0xC0)
#define DPORT_PERIP_RST_EN      PORT_REG(DR_REG_DPORT_BASE + 0xC4)
#define DPORT_TIMERGROUP1_EN    (1ul << 15)

static void sharedClockStart(void)
{
    /* Timer group 1 is clock gated after reset (the ROM uses group 0 only). */
    DPORT_PERIP_CLK_EN |= DPORT_TIMERGROUP1_EN;
    DPORT_PERIP_RST_EN &= ~DPORT_TIMERGROUP1_EN;
    TIMG1_T0CONFIG = 0;
    TIMG1_T0LOADLO = 0;
    TIMG1_T0LOADHI = 0;
    TIMG1_T0LOAD = 1;
    TIMG1_T0CONFIG = T0_EN | T0_INCREASE | ((HAL_APB_HZ / RTOS_PORT_RUNTIME_HZ) << T0_DIVIDER_SHIFT);   /* 1 MHz */
}

uint32_t rtos_port_ipiSent[RTOS_PORT_CORES], rtos_port_ipiTaken[RTOS_PORT_CORES];   /* diagnostics */

/* RTC_CNTL_SW_CPU_STALL: bits 20..25 stall the APP CPU (write 0x21), 26..31 the PRO CPU;
   RTC_CNTL_OPTIONS0: bits 0..1 (write 2) and 2..3 complete the two. The pairs are what
   Espressif's esp_cpu_stall writes; releaseAppCpu clears the APP pair. */
void rtos_port_stallOtherCore(void)
{
    if(rtos_port_coreId() == 0)
    {
        RTC_CNTL_SW_CPU_STALL = (RTC_CNTL_SW_CPU_STALL & ~(0x3Ful << 20)) | (0x21ul << 20);
        RTC_CNTL_OPTIONS0_ = (RTC_CNTL_OPTIONS0_ & ~3ul) | 2ul;
    }
    else
    {
        RTC_CNTL_SW_CPU_STALL = (RTC_CNTL_SW_CPU_STALL & ~(0x3Ful << 26)) | (0x21ul << 26);
        RTC_CNTL_OPTIONS0_ = (RTC_CNTL_OPTIONS0_ & ~(3ul << 2)) | (2ul << 2);
    }
    __asm__ __volatile__ ("memw" : : : "memory");
}

void rtos_port_resumeOtherCore(void)
{
    __asm__ __volatile__ ("memw" : : : "memory");
    if(rtos_port_coreId() == 0)
    {
        RTC_CNTL_SW_CPU_STALL &= ~(0x3Ful << 20);
        RTC_CNTL_OPTIONS0_ &= ~3ul;
    }
    else
    {
        RTC_CNTL_SW_CPU_STALL &= ~(0x3Ful << 26);
        RTC_CNTL_OPTIONS0_ &= ~(3ul << 2);
    }
}

static void ipiIsr(void)
{
    const uint8_t me = rtos_port_coreId();
    DPORT_CPU_INTR_FROM_CPU(me) = 0;
    ++ rtos_port_ipiTaken[me];
    rtos_port_ipiHandler();
}

void rtos_port_ipi(uint8_t core)
{
    ++ rtos_port_ipiSent[core];
    __asm__ __volatile__ ("memw" : : : "memory");
    DPORT_CPU_INTR_FROM_CPU(core) = 1;
}

/* Core 1 runs the same image: its idle stack is this array, its entry is the kernel's
   idle loop for it, and _start_core1 (port_start.S) brings it here with windows, PS and
   VECBASE set and interrupts masked. */
#ifndef RTOS_PORT_CORE1_STACK
# define RTOS_PORT_CORE1_STACK  RTOS_PORT_LARGE_STACK    /* idle, the IPI frame and the kernel's switch */
#endif
static uint32_t _core1Stack[RTOS_PORT_CORE1_STACK / 4u] __attribute__((aligned(16)));
uint32_t *rtos_port_core1StackTop = &_core1Stack[RTOS_PORT_CORE1_STACK / 4u];
static void (*_core1Entry)(void);
extern void _start_core1(void);

#define INTR_SOURCES        69u             /* peripheral interrupt sources of the matrix */
#define CPU_INT_PARKED      16u             /* a CPU interrupt this core never enables */

void rtos_port_core1Main(void)
{
    uint32_t src;

    rtos_port_lockAcquire();                /* masked from reset: the lock goes with it */
    /* The matrix routes every source to CPU interrupt 6 by default, the line of the tick
       timer. On this core nothing but the IPI is wanted: park every source on an interrupt
       this core never enables. */
    for(src = 0; src < INTR_SOURCES; ++src)
        DPORT_APP_INTR_MAP(src) = CPU_INT_PARKED;
    rtos_port_attachInterrupt(CPU_INT_IPI, RTOS_PORT_SRC_FROM_CPU1, ipiIsr);   /* on this core's matrix */
    _core1Entry();
    rtos_port_halt();
}

void rtos_port_startCore(uint8_t core, void (*entry)(void))
{
    RTOS_CHECK(core == 1  &&  entry != NULL, core, (uintptr_t)entry);
    _core1Entry = entry;
    if((uint32_t)&_flash_pages != 0)
    {
        /* Its MMU entries, window masks and the cleared invalid access flag were set in
           rtos_port_flashInit, with the caches off; its cache goes on right before it
           runs, as Espressif's start-up does. */
        Cache_Flush_rom(1);
        Cache_Read_Enable_rom(1);
    }
    rtos_port_attachInterrupt(CPU_INT_IPI, RTOS_PORT_SRC_FROM_CPU0, ipiIsr);   /* core 0's IPI */
    __asm__ __volatile__ ("memw" : : : "memory");
    releaseAppCpu((uint32_t)&_start_core1);
}

/* The switch: the kernel set rtos_pNext. A task that owns this core's FPU registers has
   them saved first, so that it may go on on the other core (lazy restore stays: the next
   FPU instruction traps there). The assembler routine takes the two context slots, since
   rtos_pCurrent is per core here. */
extern void rtos_port_switchContextTo(rtos_port_ctx_t *pFromCtx, rtos_port_ctx_t *pToCtx);

void rtos_port_switchContext(void)
{
    rtos_tcb_t * const pFrom = rtos_pCurrent;
    rtos_tcb_t * const pTo = rtos_pNext;

    if(_fpuOwner == pFrom->prio)
    {
        fpuEnable();
        fpuSave(&_fpu[pFrom->prio]);
        _fpuOwner = RTOS_NO_TASK;
    }
    rtos_pCurrent = pTo;
    rtos_port_switchContextTo(&pFrom->ctx, &pTo->ctx);
}

#endif  /* RTOS_CFG_SMP */


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


#if RTOS_CFG_TICKLESS
/* _sleeping (declared above the interrupt entry): true from the moment idle programs the
   long sleep until the first interrupt after it has counted the ticks that passed. Set and
   cleared with interrupts masked. */

/** Count the ticks that passed while asleep and put the compare back on the grid. Called
    with interrupts masked: from the interrupt entry when an interrupt ends a tickless
    sleep, before its handler runs, so that the handler and any task it wakes see the
    current tick (a timeout computed from a stale count would end at the catch-up, up to
    the whole sleep too early), and from idle after waiti as a safety net. Writing
    CCOMPARE0 clears a pending tick interrupt, so the ticks are not counted twice. */
static void catchUpTicks(void)
{
    if(rsr_interrupt() & (1ul << PORT_INT_TIMER0))
        ++ _tickInterrupts;                 /* the tick itself ended the sleep: counted here, its handler is not run */
    while((int32_t)(_nextTick - rtos_port_ccount()) <= 0)
    {
        _nextTick += CYCLES_PER_TICK;
        rtos_port_runBase += CYCLES_PER_TICK;
        rtos_tickFromIsr();
    }
    wsr_ccompare0(_nextTick);
}
#endif


#if RTOS_CFG_TICKLESS
void rtos_port_idleSleepTicks(rtos_tick_t ticks)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    uint32_t n = ticks == RTOS_FOREVER ? 1000ul : (uint32_t)ticks;

    /* Never sleep past half the watchdog timeout, and never further than 2^30 cycles:
       the catch-up below compares CCOUNT differences as signed 32 bit values, which only
       works within half the counter range (1000 ticks at 1 kHz is 240 M cycles; a 100 Hz
       tick would make 1000 ticks 2.4 G cycles and break it, hence the bound in cycles). */
    if(_wdtTimeoutMs != 0  &&  n > _wdtTimeoutMs * RTOS_CFG_TICK_HZ / 2000ul)
        n = (uint32_t)(_wdtTimeoutMs * RTOS_CFG_TICK_HZ / 2000ul);
    if(n > 1000ul)
        n = 1000ul;
    if(n > (1ul << 30) / CYCLES_PER_TICK)
        n = (1ul << 30) / CYCLES_PER_TICK;
    if(n > 1)
        wsr_ccompare0(_nextTick + (n - 1u) * CYCLES_PER_TICK);
    _sleeping = true;

    /* waiti lowers the interrupt level and waits in one instruction: an interrupt that
       is already pending is taken at once and we return here after it. The interrupt's
       entry (rtos_port_exception) has counted the ticks that passed before it ran its
       handler, and may have switched to a task it woke; we are back here when idle runs
       again. */
    __asm__ __volatile__ ("waiti 0" : : : "memory");

    /* Back with interrupts masked again. The safety net: nothing left to count unless an
       interrupt was taken at a level the entry does not see. */
    (void)rtos_port_enterCritical();
    _sleeping = false;
    catchUpTicks();
    rtos_kernel_isrTailInline();        /* a caught up tick may have readied a task */
    rtos_port_exitCritical(cs);
}
#endif  /* RTOS_CFG_TICKLESS */


void rtos_port_startTimer(void)
{
#if RTOS_CFG_SMP
    sharedClockStart();                     /* before the first run time reading */
#endif
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
#if RTOS_CFG_SMP
    if(_nextTick != 0)
        return (uint32_t)t;                 /* the shared clock counts microseconds */
#endif
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
