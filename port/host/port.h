#ifndef RTOS_PORT_INCLUDED
#define RTOS_PORT_INCLUDED
/**
 * @file port.h
 * Host (Linux/PC) port of the kernel for unit testing without hardware. Contexts are
 * ucontext_t, critical sections are counters, and interrupts are simulated by the test
 * calling rtos_host_simulateIsr. See port_host.c.
 *
 * RTOS_HOST_CORES (rtos_config.h, default 1) above 1 gives the port that many "cores": one
 * thread each, started with rtos_port_startCore; a critical section then also takes the
 * one kernel spinlock, so that what the kernel protects with interrupt masking on one core
 * is protected across cores (nornInternal/nornCore/smp_plan.md, step 2); rtos_port_ipi interrupts another
 * core. With 1 the port is the single threaded one it always was.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rtos_config.h"
#include "rtos_types.h"

#define RTOS_PORT_MIN_STACK     1u

/** Cores this port can run the kernel on, and whether one kernel over all of them (SMP,
    nornInternal/nornCore/smp_plan.md) is the default: RTOS_HOST_CORES threads, 1 unless the test's
    rtos_config.h says otherwise. The application may set RTOS_CFG_SMP itself. */
#ifndef RTOS_HOST_CORES
# define RTOS_HOST_CORES        1
#endif
#define RTOS_PORT_CORES         RTOS_HOST_CORES
#define RTOS_PORT_SMP           (RTOS_HOST_CORES > 1)     /* spinlock, IPI, core start: below */
#define RTOS_PORT_SMP_DEFAULT   (RTOS_PORT_CORES > 2u)
#ifndef RTOS_CFG_SMP
# define RTOS_CFG_SMP           RTOS_PORT_SMP_DEFAULT
#endif
#define RTOS_PORT_DEFAULT_STACK 1u
#define RTOS_PORT_LARGE_STACK   1u
#define RTOS_PORT_STACK_WARN    8u      /* the host stacks are dummies; tests dirty them */
/* The host port runs tasks on its own malloc'd stacks and ignores the area given to
   rtos_createTask, so the tests may pass 1 byte arrays. */
#define RTOS_PORT_STACK_ALIGN   1u
#define RTOS_PORT_WEAK          __attribute__((weak))
#define RTOS_PORT_NOINIT
#define RTOS_PORT_STR(s)        (s)
#define RTOS_PORT_RUNTIME_HZ    RTOS_CFG_TICK_HZ

/** Define an interrupt body on the host; it is just a function the test can simulate. */
#define RTOS_ISR(vector)        RTOS_ISR_(vector)
#define RTOS_ISR_(vector)       void vector##_rtosBody(void)

typedef void *rtos_port_ctx_t;
typedef int rtos_port_cs_t;
typedef uint32_t rtos_port_runtime_t;

rtos_port_cs_t rtos_port_enterCritical(void);
void rtos_port_exitCritical(rtos_port_cs_t s);
void rtos_port_enableInterrupts(void);
void rtos_port_idleSleep(void);
void rtos_port_idleSleepTicks(rtos_tick_t ticks);
void rtos_port_halt(void) __attribute__((noreturn));
void rtos_port_switchContext(void);
rtos_port_ctx_t rtos_port_initStack(uint8_t *stackArea, size_t stackSize, void (*entry)(void));
/** Frees the ucontext and the stack rtos_port_initStack allocated (rtos_reapTask). */
void rtos_port_freeStack(rtos_port_ctx_t ctx);
/** True inside rtos_host_simulateIsr. */
bool rtos_port_inIsr(void);
#define RTOS_PORT_IN_ISR()  rtos_port_inIsr()
void rtos_port_startTimer(void);
rtos_port_runtime_t rtos_port_getRunTimeCounter(void);
void rtos_port_reset(void) __attribute__((noreturn));
uint8_t rtos_port_getResetCause(void);
void rtos_port_wdtStart(uint32_t timeoutMs);
void rtos_port_wdtKick(void);

/* ---- several cores (RTOS_HOST_CORES > 1) --------------------------------------------- */

/** The core (thread) this code runs on: 0 is the main thread, the others are the threads
    rtos_port_startCore made. Thread local, so a context that migrates between cores (the
    SMP kernel, smp_plan.md step 3) reads the core it is on. */
#if RTOS_HOST_CORES > 1
extern __thread uint8_t rtos_port_hostCore;
static inline uint8_t rtos_port_coreId(void) { return rtos_port_hostCore; }
#else
static inline uint8_t rtos_port_coreId(void) { return 0; }
#endif

/** Start core \a core (1 .. RTOS_PORT_CORES - 1) as a thread running \a entry, which does
    not return until the core is to stop; rtos_host_joinCore waits for that. */
void rtos_port_startCore(uint8_t core, void (*entry)(void));

/** The mask alone, for rtos_kernel_current: nothing to do on the host, where a running
    context cannot be interrupted between two instructions. */
static inline rtos_port_cs_t rtos_port_irqMask(void) { return 0; }
static inline void rtos_port_irqRestore(rtos_port_cs_t s) { (void)s; }

/** Interrupt core \a core: its rtos_port_ipiHandler runs as a simulated interrupt the next
    time that core has interrupts enabled (at the end of its critical section, in
    rtos_port_enableInterrupts, or at once if it is in rtos_port_idleSleep). Several IPIs
    before the core gets to it are one, as in hardware. Callable from any core, also inside
    a critical section. */
void rtos_port_ipi(uint8_t core);

/** What an IPI runs on the receiving core, in interrupt context with the kernel lock held.
    Weak and empty in the port; the SMP kernel (smp_plan.md step 4) defines it, and a test
    may. */
void rtos_port_ipiHandler(void);

/* Host specific test support. */

/** Wait for the thread of core \a core to return from its entry function. */
void rtos_host_joinCore(uint8_t core);

/** The core that holds the kernel lock, RTOS_NO_TASK (0xFF) if none; diagnostics. */
uint8_t rtos_host_lockOwner(void);

/** True if an IPI to \a core has not been taken yet. */
bool rtos_host_ipiPending(uint8_t core);

/** A thread that stands for the hardware around the cores: it is no core, and from it
    rtos_host_tick and rtos_host_simulateIsr raise their interrupt for core 0 instead of
    running it, delivered when core 0 next enables interrupts (its tasks' critical sections,
    its idle loop). That is how a test drives time while core 0 runs a task, as a timer
    does. rtos_host_raiseIsr raises an interrupt for any core. */
void rtos_host_startHardware(void (*fn)(void));
void rtos_host_joinHardware(void);
void rtos_host_raiseIsr(uint8_t core, void (*isrBody)(void));

/** Run \a isrBody as if it were an interrupt: with interrupts disabled, followed by the
    kernel's ISR tail which may switch task. */
void rtos_host_simulateIsr(void (*isrBody)(void));

/** Advance the system time by one tick, as the timer interrupt would. */
void rtos_host_tick(void);

/** True while inside a simulated ISR or a critical section. */
int rtos_host_interruptsDisabled(void);

#endif  /* RTOS_PORT_INCLUDED */
