#ifndef RTOS_PORT_INCLUDED
#define RTOS_PORT_INCLUDED
/**
 * @file port.h
 * AVR port of the kernel: critical sections, interrupt wrapper, sleep and the hooks the
 * kernel needs from the hardware layer. The context switch itself is in port_context.S,
 * the tick timer and stack preparation in port_avr.c.
 *
 * Register conventions of avr-gcc used here:
 *   call-clobbered (saved by the ISR wrapper):  r0, r1, r18-r27, r30, r31, SREG, RAMPZ
 *   call-saved (saved by rtos_port_switchContext): r2-r17, r28, r29
 * A task that blocks voluntarily therefore only pays for the call-saved half; a task that
 * is preempted by an ISR pays for both halves, which is what the AVR itself requires.
 */

#include <stdint.h>
#include <stddef.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>

#include "rtos_config.h"
#include "rtos_types.h"


/*
 * Defines
 */

/** Smallest stack we accept: trampoline frame, one ISR frame, one switch frame and some
    room for the task's own calls. */
#define RTOS_PORT_MIN_STACK     96u

/** Recommended starting sizes (see README "Stacks"): the default for an ordinary task,
    and for a task that prints, nests calls or does software floating point. Neither is
    an upper bound. */
#define RTOS_PORT_DEFAULT_STACK 192u
#define RTOS_PORT_LARGE_STACK   384u

/** Stack monitor: warn when fewer than this many bytes of a stack were never touched;
    one interrupt frame, i.e. the next interrupt may overflow. */
#define RTOS_PORT_STACK_WARN    24u

/** Stack frame alignment required by the ABI; the AVR has none. */
#define RTOS_PORT_STACK_ALIGN   1u

/** Attribute making a function overridable by the application. */
#define RTOS_PORT_WEAK          __attribute__((weak))

/** Data that keeps its value across a reset (not zeroed by the startup code). */
#define RTOS_PORT_NOINIT        __attribute__((section(".noinit")))

/** How rtos_port_reset restarts: 0 jumps to the reset vector of the application without
    passing the bootloader, 1 lets the watchdog reset the chip (needs a bootloader that
    disables the watchdog; the original Mega 2560 bootloader does not). */
#ifndef RTOS_CFG_AVR_RESET_VIA_WDT
# define RTOS_CFG_AVR_RESET_VIA_WDT 0
#endif

/** Frequency of rtos_port_getRunTimeCounter: the tick timer counts at F_CPU/64. 32 bits
    wrap after 4.7 hours at 16 MHz; the kernel only uses differences. */
#define RTOS_PORT_TIMER_PRESCALER   64ul
#define RTOS_PORT_RUNTIME_HZ        (F_CPU / RTOS_PORT_TIMER_PRESCALER)
#define RTOS_PORT_TIMER_TOP         (F_CPU / RTOS_PORT_TIMER_PRESCALER / RTOS_CFG_TICK_HZ - 1ul)

/*
 * Tick timer selection: RTOS_CFG_TIMER is one of the 16 bit timers of the MCU: 1, 3, 4 or
 * 5 on the ATmega2560, only 1 on the ATmega328P. Left undefined it defaults to 3 where that
 * exists (keeps timer 1 free for PWM on the Mega) and to 1 otherwise. The register names
 * are built here so that the run time counter can be read inline.
 */
#ifndef RTOS_CFG_TIMER
# if defined(TCCR3A)
#  define RTOS_CFG_TIMER 3
# else
#  define RTOS_CFG_TIMER 1
# endif
#endif

#define RTOS_PORT_CAT2_(a, b)       a##b
#define RTOS_PORT_CAT2(a, b)        RTOS_PORT_CAT2_(a, b)
#define RTOS_PORT_CAT3_(a, b, c)    a##b##c
#define RTOS_PORT_CAT3(a, b, c)     RTOS_PORT_CAT3_(a, b, c)

#define RTOS_PORT_TCCRnA      RTOS_PORT_CAT3(TCCR, RTOS_CFG_TIMER, A)
#define RTOS_PORT_TCCRnB      RTOS_PORT_CAT3(TCCR, RTOS_CFG_TIMER, B)
#define RTOS_PORT_TCNTn       RTOS_PORT_CAT2(TCNT, RTOS_CFG_TIMER)
#define RTOS_PORT_OCRnA       RTOS_PORT_CAT3(OCR,  RTOS_CFG_TIMER, A)
#define RTOS_PORT_TIMSKn      RTOS_PORT_CAT2(TIMSK, RTOS_CFG_TIMER)
#define RTOS_PORT_TIFRn       RTOS_PORT_CAT2(TIFR, RTOS_CFG_TIMER)
#define RTOS_PORT_OCIEnA      RTOS_PORT_CAT3(OCIE, RTOS_CFG_TIMER, A)
#define RTOS_PORT_OCFnA       RTOS_PORT_CAT3(OCF,  RTOS_CFG_TIMER, A)
#define RTOS_PORT_WGMn2       RTOS_PORT_CAT3(WGM,  RTOS_CFG_TIMER, 2)
#define RTOS_PORT_CSn1        RTOS_PORT_CAT3(CS,   RTOS_CFG_TIMER, 1)
#define RTOS_PORT_CSn0        RTOS_PORT_CAT3(CS,   RTOS_CFG_TIMER, 0)
#define RTOS_PORT_TICK_VECT   RTOS_PORT_CAT3(TIMER, RTOS_CFG_TIMER, _COMPA_vect)


/** Place a string literal in flash. Used for assertion messages. */
#define RTOS_PORT_STR(s)        PSTR(s)

#if defined(__AVR_HAVE_RAMPZ__)
# define RTOS_PORT_ISR_SAVE_RAMPZ      "in   r0, %1   \n\t" "push r0       \n\t"
# define RTOS_PORT_ISR_RESTORE_RAMPZ   "pop  r0       \n\t" "out  %1, r0   \n\t"
# define RTOS_PORT_ISR_OPERANDS        : : "I"(_SFR_IO_ADDR(SREG)), "I"(_SFR_IO_ADDR(RAMPZ))
#else
# define RTOS_PORT_ISR_SAVE_RAMPZ
# define RTOS_PORT_ISR_RESTORE_RAMPZ
# define RTOS_PORT_ISR_OPERANDS        : : "I"(_SFR_IO_ADDR(SREG))
#endif

/** Push everything an ISR body may clobber. r1 is cleared because the compiler relies on
    it being 0 and the interrupted code may have used it temporarily. */
#define RTOS_PORT_ISR_PROLOGUE                                      \
    "push r0       \n\t"                                            \
    "in   r0, %0   \n\t"                                            \
    "push r0       \n\t"                                            \
    "push r1       \n\t"                                            \
    "clr  r1       \n\t"                                            \
    RTOS_PORT_ISR_SAVE_RAMPZ                                        \
    "push r18      \n\t"                                            \
    "push r19      \n\t"                                            \
    "push r20      \n\t"                                            \
    "push r21      \n\t"                                            \
    "push r22      \n\t"                                            \
    "push r23      \n\t"                                            \
    "push r24      \n\t"                                            \
    "push r25      \n\t"                                            \
    "push r26      \n\t"                                            \
    "push r27      \n\t"                                            \
    "push r30      \n\t"                                            \
    "push r31      \n\t"

#define RTOS_PORT_ISR_EPILOGUE                                      \
    "pop  r31      \n\t"                                            \
    "pop  r30      \n\t"                                            \
    "pop  r27      \n\t"                                            \
    "pop  r26      \n\t"                                            \
    "pop  r25      \n\t"                                            \
    "pop  r24      \n\t"                                            \
    "pop  r23      \n\t"                                            \
    "pop  r22      \n\t"                                            \
    "pop  r21      \n\t"                                            \
    "pop  r20      \n\t"                                            \
    "pop  r19      \n\t"                                            \
    "pop  r18      \n\t"                                            \
    RTOS_PORT_ISR_RESTORE_RAMPZ                                     \
    "pop  r1       \n\t"                                            \
    "pop  r0       \n\t"                                            \
    "out  %0, r0   \n\t"                                            \
    "pop  r0       \n\t"

/**
 * Define an interrupt service routine that may use the kernel's FromIsr functions and
 * may end with a task switch. Usage:
 *     RTOS_ISR(TIMER4_COMPA_vect)
 *     {
 *         rtos_semGiveFromIsr(&mySem);
 *     }
 * The body is an ordinary C function running with interrupts disabled. The wrapper saves
 * the call-clobbered registers, runs the body, lets the kernel switch task if the body
 * made a higher priority task ready, and returns with reti.
 */
#define RTOS_ISR(vector)        RTOS_ISR_(vector)
#define RTOS_ISR_(vector)                                                       \
    void vector##_rtosBody(void) __attribute__((used, noinline));               \
    ISR(vector, ISR_NAKED)                                                      \
    {                                                                           \
        __asm__ __volatile__                                                    \
        ( RTOS_PORT_ISR_PROLOGUE                                                \
          "call " #vector "_rtosBody  \n\t"                                     \
          "call rtos_isrTail          \n\t"                                     \
          RTOS_PORT_ISR_EPILOGUE                                                \
          "reti                       \n\t"                                     \
          RTOS_PORT_ISR_OPERANDS                                                \
        );                                                                      \
    }                                                                           \
    void vector##_rtosBody(void)


/*
 * Global type definitions
 */

/** Saved context of a task: its stack pointer. Must be the first TCB member. */
typedef uint8_t *rtos_port_ctx_t;

/** Saved interrupt state of a critical section. */
typedef uint8_t rtos_port_cs_t;

/** Run time count, see RTOS_PORT_RUNTIME_HZ. */
typedef uint32_t rtos_port_runtime_t;


/*
 * Inline functions
 */

/** Enter a critical section: disable interrupts, return the previous state. Nestable. */
static inline rtos_port_cs_t rtos_port_enterCritical(void)
{
    const rtos_port_cs_t s = SREG;
    cli();
    return s;
}

/** Leave a critical section: restore the interrupt state saved at entry. */
static inline void rtos_port_exitCritical(rtos_port_cs_t s)
{
    __asm__ __volatile__ ("" : : : "memory");
    SREG = s;
}

/** Unconditionally enable interrupts. Used when a task starts and when the idle task
    begins. */
static inline void rtos_port_enableInterrupts(void)
{
    sei();
}

/** Sleep until the next interrupt. Peripherals and timers keep running. */
static inline void rtos_port_idleSleep(void)
{
    set_sleep_mode(SLEEP_MODE_IDLE);
    sleep_enable();
    sleep_cpu();
    sleep_disable();
}

/** Tickless idle is not implemented on the AVR: sleep until the next interrupt, which
    is at most one tick away. */
static inline void rtos_port_idleSleepTicks(rtos_tick_t ticks)
{
    (void)ticks;
    rtos_port_idleSleep();
}

/** Stop forever with interrupts off. simavr terminates the simulation on this pattern. */
static inline void rtos_port_halt(void) __attribute__((noreturn));
static inline void rtos_port_halt(void)
{
    cli();
    for(;;)
    {
        sleep_enable();
        sleep_cpu();
    }
}


/*
 * Global prototypes
 */

/** Save the current task's call-saved registers and stack pointer, switch to rtos_pNext.
    Interrupts must be disabled. Implemented in port_context.S. */
void rtos_port_switchContext(void);

/** Prepare an unused stack so that the first switch to it lands in \a entry. */
rtos_port_ctx_t rtos_port_initStack(uint8_t *stackArea, size_t stackSize, void (*entry)(void));

/** Configure and start the tick timer. */
void rtos_port_startTimer(void);

/** Run time counter value at the last tick, maintained by the tick ISR (port_avr.c). */
extern rtos_port_runtime_t rtos_port_runBase;

/** Free running time at RTOS_PORT_RUNTIME_HZ. Interrupts must be disabled. Inline: it is
    read at every context switch. */
static inline rtos_port_runtime_t rtos_port_getRunTimeCounter(void)
{
    rtos_port_runtime_t base = rtos_port_runBase;
    uint16_t cnt = RTOS_PORT_TCNTn;

    if(RTOS_PORT_TIFRn & _BV(RTOS_PORT_OCFnA))
    {
        /* A tick is pending but not yet counted. */
        base += RTOS_PORT_TIMER_TOP + 1ul;
        cnt = RTOS_PORT_TCNTn;
    }
    return base + cnt;
}

/** Reset the MCU through the watchdog. Never returns. */
void rtos_port_reset(void) __attribute__((noreturn));

/** RTOS_RESET_* cause of the last reset, from MCUSR captured at startup. */
uint8_t rtos_port_getResetCause(void);

/** Start the hardware watchdog in reset mode; timeoutMs is rounded up to 15 ms..8 s. */
void rtos_port_wdtStart(uint32_t timeoutMs);

/** Restart the watchdog timeout. */
void rtos_port_wdtKick(void);

#endif  /* RTOS_PORT_INCLUDED */
