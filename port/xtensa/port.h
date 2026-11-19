#ifndef RTOS_PORT_INCLUDED
#define RTOS_PORT_INCLUDED
/**
 * @file port.h
 * Xtensa LX6 (ESP32) port of the kernel: critical sections, types and constants the
 * kernel needs. Startup is in port_start.S, the vector table in port_vectors.S, hardware
 * setup and the port functions in port_esp32.c.
 *

 * FPU: the single precision FPU (coprocessor 0) is handled lazily. CPENABLE is cleared at
 * every context switch; the first FPU instruction of a task traps, and the trap saves the
 * registers of the task that used the FPU last and loads the current task's. Tasks that
 * never touch floating point cost nothing. Interrupt handlers and timer callbacks in the
 * tick must not use the FPU: that traps to a panic.
 *
 * Interrupts: the kernel sees level 1 interrupts only. A peripheral source is routed by
 * the interrupt matrix to a level 1 CPU interrupt with rtos_port_attachInterrupt, and the
 * dispatcher in port_esp32.c calls the attached handler; the handler must clear the source
 * at the peripheral. RTOS_ISR(name) defines such a handler as name_rtosBody. The tick is
 * the CCOMPARE0 timer on CPU interrupt 6.
 *
 * Critical sections raise PS.INTLEVEL to PORT_INTLEVEL_CRITICAL with rsil, which masks
 * every interrupt the kernel knows about, and restore the saved PS afterwards.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rtos_config.h"
#include "rtos_types.h"
#include "port_regs.h"


/*
 * Defines
 */

/** Smallest stack we accept. Measured with tc11 (2026-11-04, QEMU): a task that only
    blocks leaves 300 bytes on its stack (seven 32 byte call8 frames down to
    rtos_port_switchContext plus the spill slots below the deepest frame), a running task
    that is interrupted 336 (the 176 byte interrupt frame, the dispatch, the handler and
    the switch out of it); the FPU trap needs less than the interrupt. tc12 shows the
    share does not grow with the depth of the task's own call chain (the live windows are
    spilled into frames the task already owns). 512 leaves a task with three or four
    call8 frames of its own (48 bytes each with a few locals); anything that prints or
    nests deeper wants more: own frames plus about 350. */
#define RTOS_PORT_MIN_STACK     512u

/** Cores this port can run the kernel on, and whether one kernel over all of them (SMP,
    nornInternal/nornCore/smp_plan.md) is the default: two cores, so AMP (a kernel per core) stays the default. The application may set RTOS_CFG_SMP itself. */
#define RTOS_PORT_CORES         2u
#define RTOS_PORT_SMP           1       /* the primitives below (nornInternal/nornCore/smp_plan.md step 7); AMP stays the default */
#define RTOS_PORT_SMP_DEFAULT   (RTOS_PORT_CORES > 2u)
#ifndef RTOS_CFG_SMP
# define RTOS_CFG_SMP           RTOS_PORT_SMP_DEFAULT
#endif

/** Recommended starting sizes (see README "Stacks"): the default for an ordinary task,
    and for a task that prints, nests calls or uses floating point. Neither is an upper
    bound; deep call chains and crypto or network tasks need more. */
#define RTOS_PORT_DEFAULT_STACK 1024u
#define RTOS_PORT_LARGE_STACK   2048u

/** Stack monitor: warn when fewer than this many bytes of a stack were never touched;
    two call8 frames, i.e. the next nested call or interrupt may overflow. */
#define RTOS_PORT_STACK_WARN    96u

/** The windowed ABI needs 16 byte aligned stack frames. */
#define RTOS_PORT_STACK_ALIGN   16u

#define RTOS_PORT_WEAK          __attribute__((weak))

/** Data that keeps its value across a reset: the .noinit section of esp32.ld, which
    _start does not clear. DRAM is retained through a software or watchdog reset. */
#define RTOS_PORT_NOINIT        __attribute__((section(".noinit")))

/** Strings live in DRAM; nothing to do. */
#define RTOS_PORT_STR(s)        (s)

/** Put a function, or a constant, in flash instead of IRAM/DRAM (see esp32.ld). Slower
    through the cache, never for interrupt handlers or what they call. */
#define RTOS_PORT_FLASH_TEXT    __attribute__((section(".flash.text.user")))
#define RTOS_PORT_FLASH_RODATA  __attribute__((section(".flash.rodata.user")))

/** Run time counter: the CPU cycle counter CCOUNT, extended to 64 bits by the port. */
#if RTOS_CFG_SMP
# define RTOS_PORT_RUNTIME_HZ   1000000ul               /* TIMG1 timer 0 at APB / 80: one clock for both cores, in us, so that no 64 bit division is needed to read it */
#else
# define RTOS_PORT_RUNTIME_HZ   F_CPU
#endif
/** Frames printed after a fatal exception (port_esp32.c, port_vectors.S), SMP builds. */
#ifndef PORT_FATAL_TRACE_DEPTH
# if RTOS_CFG_SMP
#  define PORT_FATAL_TRACE_DEPTH 10
# else
#  define PORT_FATAL_TRACE_DEPTH 0
# endif
#endif
#if RTOS_CFG_SMP && RTOS_CFG_TICKLESS
# error RTOS_CFG_TICKLESS with RTOS_CFG_SMP is not implemented (the tick stays on core 0, every tick)
#endif
#define RTOS_PORT_CYCLES_PER_TICK   (F_CPU / RTOS_CFG_TICK_HZ)

/** nsau gives the leading zero count in one instruction. */
#define RTOS_PORT_HIGHEST_BIT(m)    (31 - __builtin_clz((uint32_t)(m)))

/** The scheduler is inlined into the blocking path: one 32 byte frame less per task
    (see rtos_kernel.c). */
#define RTOS_PORT_INLINE_SCHEDULE   1

/** Interrupt body definition; the port's dispatcher calls it. */
#define RTOS_ISR(vector)        RTOS_ISR_(vector)
#define RTOS_ISR_(vector)       void vector##_rtosBody(void)

/** No peripheral source: the CPU interrupt is internal (timer, software). */
#define RTOS_PORT_SRC_NONE      0xFFu


/*
 * Global type definitions
 */

/** Saved context of a task: its stack pointer. Must be the first TCB member. */
typedef uint32_t *rtos_port_ctx_t;

/** Saved PS of a critical section. */
typedef uint32_t rtos_port_cs_t;

typedef uint64_t rtos_port_runtime_t;

/** Interrupt handler as stored by rtos_port_attachInterrupt. */
typedef void (*rtos_port_isr_t)(void);


/*
 * Inline functions
 */

#if RTOS_CFG_SMP
/* SMP (nornInternal/nornCore/smp_plan.md): a critical section is this core's interrupt mask plus the one
   kernel spinlock, taken when the mask goes on and released when it goes off, so that
   "interrupts masked" and "lock held" are the same state on every core. The saved PS says
   whether a section is the outermost one. The interrupt entry (rtos_port_exception) takes
   the lock itself, since the hardware has masked for it, and releases it on the way out. */
extern volatile uint32_t rtos_port_lockWord;    /* 0 free, 1 held */
void rtos_port_lockSpin(void);                  /* the slow path: wait until it is ours */

/* Test-and-set with S32C1I (compare against SCOMPARE1, store if equal): 12 cycles for the
   instruction, about 36 for a whole uncontended section with the mask (measured 2026-11-18,
   nornInternal/nornCore/measurements/step16-lock.md). The fast path is inline so that no call frame is paid;
   only a lost race leaves the line. MEMW keeps the section's accesses inside it; it costs
   one cycle when nothing is outstanding. */
static inline void rtos_port_lockAcquire(void)
{
    uint32_t got = 1u;
    __asm__ __volatile__ ("movi a8, 0\n\twsr a8, scompare1\n\ts32c1i %0, %1, 0"
                          : "+a"(got) : "a"(&rtos_port_lockWord) : "a8", "memory");
    if(got != 0u)                           /* got is the old value: held by the other core */
        rtos_port_lockSpin();
    __asm__ __volatile__ ("memw" : : : "memory");
}

static inline void rtos_port_lockRelease(void)
{
    __asm__ __volatile__ ("memw" : : : "memory");
    rtos_port_lockWord = 0u;
}

static inline rtos_port_cs_t rtos_port_enterCritical(void)
{
    uint32_t ps;
    __asm__ __volatile__ ("rsil %0, " RTOS_PORT_STRINGIFY(PORT_INTLEVEL_CRITICAL)
                          : "=a"(ps) : : "memory");
    if((ps & PS_INTLEVEL_MASK) < PORT_INTLEVEL_CRITICAL)
        rtos_port_lockAcquire();
    return ps;
}

static inline void rtos_port_exitCritical(rtos_port_cs_t ps)
{
    if((ps & PS_INTLEVEL_MASK) < PORT_INTLEVEL_CRITICAL)
        rtos_port_lockRelease();
    __asm__ __volatile__ ("wsr %0, ps\n\trsync" : : "a"(ps) : "memory");
}

static inline void rtos_port_enableInterrupts(void)
{
    uint32_t ps;
    rtos_port_lockRelease();
    __asm__ __volatile__ ("rsil %0, 0" : "=a"(ps) : : "memory");
    (void)ps;
}

/** This core's interrupt mask alone, without the lock: for a read of this core's own
    state that must not be split by a migration (rtos_kernel_current). */
static inline rtos_port_cs_t rtos_port_irqMask(void)
{
    uint32_t ps;
    __asm__ __volatile__ ("rsil %0, " RTOS_PORT_STRINGIFY(PORT_INTLEVEL_CRITICAL)
                          : "=a"(ps) : : "memory");
    return ps;
}

static inline void rtos_port_irqRestore(rtos_port_cs_t ps)
{
    __asm__ __volatile__ ("wsr %0, ps\n\trsync" : : "a"(ps) : "memory");
}
#else
static inline rtos_port_cs_t rtos_port_enterCritical(void)
{
    uint32_t ps;
    __asm__ __volatile__ ("rsil %0, " RTOS_PORT_STRINGIFY(PORT_INTLEVEL_CRITICAL)
                          : "=a"(ps) : : "memory");
    return ps;
}

static inline void rtos_port_exitCritical(rtos_port_cs_t ps)
{
    __asm__ __volatile__ ("wsr %0, ps\n\trsync" : : "a"(ps) : "memory");
}

static inline void rtos_port_enableInterrupts(void)
{
    uint32_t ps;
    __asm__ __volatile__ ("rsil %0, 0" : "=a"(ps) : : "memory");
    (void)ps;
}
#endif

/** Wait for an interrupt with everything unmasked. */
static inline void rtos_port_idleSleep(void)
{
    __asm__ __volatile__ ("waiti 0" : : : "memory");
}

/** Tickless idle: sleep until the next interrupt, with the tick timer moved out to
    \a ticks from now (RTOS_FOREVER: as far as allowed) so that the CPU does not wake
    for ticks nobody waits for. The missed ticks are counted on wake up. */
void rtos_port_idleSleepTicks(rtos_tick_t ticks);

/** Number of tick interrupts taken; with tickless idle fewer than the tick count. */
uint32_t rtos_port_tickInterrupts(void);

static inline void rtos_port_halt(void) __attribute__((noreturn));
static inline void rtos_port_halt(void)
{
    for(;;)
        __asm__ __volatile__ ("waiti 15" : : : "memory");
}

/** 0 on the PRO CPU, 1 on the APP CPU (PRID special register). */
static inline uint8_t rtos_port_coreId(void)
{
    uint32_t prid;
    __asm__ __volatile__ ("rsr %0, prid" : "=a"(prid));
    return (prid & 0xFFFFu) == 0xABABu ? 1u : 0u;
}

/** Read the CPU cycle counter. */
static inline uint32_t rtos_port_ccount(void)
{
    uint32_t c;
    __asm__ __volatile__ ("rsr %0, ccount" : "=a"(c));
    return c;
}


/*
 * Global prototypes
 */

void rtos_port_switchContext(void);
rtos_port_ctx_t rtos_port_initStack(uint8_t *stackArea, size_t stackSize, void (*entry)(void));
/** Release what rtos_port_initStack allocated for a context (rtos_reapTask): nothing, the
    context lives on the application's stack. */
static inline void rtos_port_freeStack(rtos_port_ctx_t ctx)
{
    (void)ctx;
}
/** True inside a level 1 interrupt (rtos_port_exception); the dynamic task API refuses
    calls from there. */
bool rtos_port_inIsr(void);
#define RTOS_PORT_IN_ISR()  rtos_port_inIsr()
void rtos_port_startTimer(void);
/** Tick bookkeeping of port_esp32.c: the CCOUNT value of the next tick and the cycles of
    all completed ticks (kept in step by the tick interrupt). */
extern uint32_t rtos_port_nextTick;
extern rtos_port_runtime_t rtos_port_runBase;

#if RTOS_CFG_SMP
/** SMP: the 64 bit counter of TIMG1 timer 0 at RTOS_PORT_RUNTIME_HZ, one clock for every
    core (CCOUNT is per core and the cores start at different times). Interrupts masked. */
rtos_port_runtime_t rtos_port_getRunTimeCounter(void);

/** Start core \a core (1) with the same image: its own stack, vectors and the IPI
    interrupt, then \a entry (the kernel's idle loop of that core) which never returns. */
void rtos_port_startCore(uint8_t core, void (*entry)(void));

/** Interrupt core \a core: its rtos_port_ipiHandler runs there (FROM_CPU interrupt). */
void rtos_port_ipi(uint8_t core);

/** Defined by the kernel: runs in the IPI's interrupt context with the lock held. */
void rtos_port_ipiHandler(void);

/** IPIs sent to and taken by each core; diagnostics for the SMP tests. */
extern uint32_t rtos_port_ipiSent[RTOS_PORT_CORES], rtos_port_ipiTaken[RTOS_PORT_CORES];

/** The APP CPU's ROM start-up, run when the core is released (rtos_port_startCore), uses
    the DRAM from the end of the ROM's APP data (0x3FFE4350) up to here as its data and
    stack (measured 2026-11-18: words up to 0x3FFE7E00 written). Memory below this address
    and above 0x3FFE4350 is free for an application only in a one core image; an SMP
    image must not keep anything there that lives across rtos_start (nornWIFI's spare
    heap begins here in SMP builds). */
#define RTOS_PORT_ROM_APP_CPU_END   0x3FFE8000ul

/** Halt the other core in hardware (RTC_CNTL stall) until rtos_port_resumeOtherCore(),
    for an operation the other core must not run beside: the flash write with the cache
    off (hal/xtensa/flash.c), a DPORT register read (the ESP32's DPORT access erratum).
    Call inside a critical section only: holding the kernel lock guarantees the other
    core is not inside one, so it cannot be stopped with the lock in hand. Espressif
    parks the other core in software instead (an interrupt that spins); the hardware
    stall is immediate and needs no handler on the other side. */
void rtos_port_stallOtherCore(void);
void rtos_port_resumeOtherCore(void);
#else
/** Cycles of all completed ticks plus the cycles since the last tick boundary. Inline
    and an addition only: it is read at every context switch. Interrupts masked. */
static inline rtos_port_runtime_t rtos_port_getRunTimeCounter(void)
{
    const uint32_t sinceTick = rtos_port_ccount() - (rtos_port_nextTick - RTOS_PORT_CYCLES_PER_TICK);
    return rtos_port_runBase + sinceTick;
}
#endif

/** Called from _start before main: watchdogs off, clocks known, flash code mapped. */
void rtos_port_earlyInit(void);

/** Map the .flash.* sections (esp32.ld) through the flash MMU and enable the cache; a
    no-op when nothing is in flash. Called by rtos_port_earlyInit on the PRO CPU. */
void rtos_port_flashInit(void);

/** Number of 64 KB flash pages mapped by rtos_port_flashInit. */
uint32_t rtos_port_flashPages(void);

/** Switch the CPU to F_CPU (port_clock.c); called by rtos_port_earlyInit. */
void rtos_port_clockInit(void);

/** Reset the chip (digital system reset through RTC_CNTL). Never returns. */
void rtos_port_reset(void) __attribute__((noreturn));

/** RTOS_RESET_* cause of the last reset of the PRO CPU. */
uint8_t rtos_port_getResetCause(void);

/** Start the timer group 0 watchdog with a system reset after timeoutMs. */
void rtos_port_wdtStart(uint32_t timeoutMs);

/** Feed the watchdog. */
void rtos_port_wdtKick(void);

/** Route peripheral interrupt \a source (ESP32 TRM table, or RTOS_PORT_SRC_NONE for an
    internal CPU interrupt) to level 1 CPU interrupt \a cpuInt and enable it. */
void rtos_port_attachInterrupt(uint8_t cpuInt, uint8_t source, rtos_port_isr_t handler);

/** Called from the exception entry: level 1 interrupts and the FPU trap. */
void rtos_port_exception(uint32_t cause, uint32_t *pFrame);

/** How often the FPU registers were switched to another task (lazy FPU context). */
uint32_t rtos_port_fpuSwitchCount(void);

/** AMP: copy the APP CPU's image (an esptool image: header and segments for its own
    IRAM/DRAM, see mem_core1.ld) into place and start the APP CPU at its entry. Called on
    the PRO CPU before the kernel starts. The image is validated before anything is
    written: magic, entry and every segment (inside the image, word aligned, inside the
    APP CPU's memory); a bad image ends in rtos_port_fatal with cause 0x2000 + segment. */
void rtos_port_startAppCpu(const uint8_t *image, uint32_t size);

/** Raise the FROM_CPU interrupt of the other core (source 24 for the PRO CPU, 25 for the
    APP CPU); the receiver clears it with rtos_port_ackCrossCore. */
void rtos_port_signalOtherCore(void);
void rtos_port_ackCrossCore(void);
#define RTOS_PORT_SRC_FROM_CPU0     24u
#define RTOS_PORT_SRC_FROM_CPU1     25u

/** Unhandled exception or interrupt; prints the cause on UART0 and halts. */
void rtos_port_fatal(uint32_t cause, uint32_t epc, uint32_t vaddr) __attribute__((noreturn));

#endif  /* RTOS_PORT_INCLUDED */
