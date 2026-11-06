#ifndef RTOS_KERNEL_INCLUDED
#define RTOS_KERNEL_INCLUDED
/**
 * @file rtos_kernel.h
 * Kernel internal interface shared between rtos_kernel.c, rtos_sync.c and the port layer.
 * Not for application use.
 */

#include "rtos.h"

/*
 * Defines
 */

/** TCB flag: event wait requires all masked events. */
#define RTOS_TF_WAIT_ALL        0x01u
/** TCB flag: task is linked into the timed list. */
#define RTOS_TF_IN_TIMED_LIST   0x02u
/** TCB flag: the last blocking call ended by timeout. */
#define RTOS_TF_TIMED_OUT       0x04u
/** TCB flag: task is blocked in rtos_notifyWait or rtos_notifyTake. */
#define RTOS_TF_WAIT_NOTIFY     0x08u
/** TCB flag: the notify wait is a take, i.e. needs a non-zero value. */
#define RTOS_TF_NOTIFY_TAKE     0x10u
/** TCB flag: a notification has been received and not yet consumed. */
#define RTOS_TF_NOTIFY_PENDING  0x20u
/** Suspended by rtos_suspend: does not run until rtos_resume. */
#define RTOS_TF_SUSPENDED       0x40u
/** Made ready while suspended: becomes ready at rtos_resume. */
#define RTOS_TF_READY_PENDING   0x80u
/** rtos_evgroupWait: clear the satisfying bits in the group when woken. */
#define RTOS_TF_EVG_CLEAR       RTOS_TF_NOTIFY_TAKE   /* reused: the two never coexist */


/** Force inlining of a function on the hot path (context switch, interrupt tail), where
    a call, its register saves and the lost register allocation cost more than the code. */
#define RTOS_KERNEL_HOT_INLINE  static inline __attribute__((always_inline))


/*
 * Global type definitions
 */

/** The task control block. The context field must stay first: the AVR port addresses it
    with offset 0 from assembly code. */
typedef struct rtos_tcb
{
    rtos_port_ctx_t ctx;            /**< Saved stack pointer (AVR) or context handle. */
    rtos_taskFunc_t taskFunc;       /**< Entry function, NULL if the slot is unused. */
    void *arg;                      /**< Argument passed to the entry function. */
    struct rtos_tcb *pNextTimed;    /**< Link in the sorted timed wait list. */
    rtos_taskmask_t *pWaiters;      /**< Waiter mask of the object we block on, or NULL. */
    void *pXfer;                    /**< Item buffer for direct hand-over while blocked. */
    rtos_tick_t wakeAt;             /**< Absolute tick at which a timed wait ends. */
    rtos_tick_t periodBase;         /**< Last period boundary of rtos_waitPeriodic. */
    rtos_evt_t evtPending;          /**< Events posted but not yet consumed. */
    rtos_evt_t evtWait;             /**< Events awaited, 0 if not waiting for events. */
#if RTOS_CFG_NOTIFY
    uint16_t notifyValue;           /**< Task notification value. */
#endif
#if RTOS_CFG_RUNTIME_STATS
    rtos_runtime_t runTime;         /**< Accumulated run time, RTOS_RUNTIME_HZ. */
#endif
#if RTOS_CFG_STACK_CHECK
    uint8_t *pStackArea;            /**< Stack memory, for the high water mark. */
    size_t stackSize;               /**< Size of the stack memory. */
#endif
    const void *pWaitObj;           /**< Object waited on; see waitKind. */
    rtos_taskmask_t bit;            /**< 1 << prio, precomputed. */
    uint8_t prio;                   /**< Base priority = task id = array index. */
    uint8_t effPrio;                /**< Effective priority, raised by ceiling mutexes. */
    uint8_t flags;                  /**< RTOS_TF_* bits. */
    uint8_t cntOverrun;             /**< Saturating count of period overruns. */
    uint8_t waitKind;               /**< RTOS_WAIT_* while blocked, else RTOS_WAIT_NONE. */
} rtos_tcb_t;


/*
 * Global data declarations
 */

extern rtos_tcb_t rtos_tcbAry[RTOS_NUM_TASKS];
extern rtos_tcb_t *rtos_pCurrent;
extern rtos_tcb_t *rtos_pNext;
extern volatile rtos_tick_t rtos_tickCount;

/** The scheduler's state bytes, in one object so that every pick addresses them from a
    single base (on the Xtensa one l32r instead of three): the ready mask (bit n set:
    task n is ready; bit 0, the idle task, always), the boosted mask (the tasks whose
    effective priority is above their own, holding a mutex with a higher ceiling; the
    ready mask is indexed by base priority, so while one of these is ready the pick must
    look at effective priorities instead of the highest ready bit), and the switch request
    set whenever a task was made ready from an ISR, evaluated by rtos_isrTail. */
typedef struct rtos_kernel_sched_t
{
    rtos_taskmask_t ready;
    rtos_taskmask_t boosted;
    volatile uint8_t needSwitch;
} rtos_kernel_sched_t;
extern rtos_kernel_sched_t rtos_kernel_sched;
#define rtos_kernel_readyMask   (rtos_kernel_sched.ready)
#define rtos_kernel_boostedMask (rtos_kernel_sched.boosted)
#define rtos_needSwitch         (rtos_kernel_sched.needSwitch)


/*
 * Global prototypes (interrupts must be disabled when calling)
 */

void rtos_kernel_makeReady(rtos_tcb_t *pT);
void rtos_kernel_schedule(void);
bool rtos_kernel_blockCurrent(rtos_tick_t timeout);

/** Block the current task on an object: records what it waits on, then blockCurrent.
    Inline: it sits on the path of every blocking call. */
static inline bool rtos_kernel_blockOn(uint8_t waitKind, const void *pWaitObj, rtos_tick_t timeout)
{
    rtos_pCurrent->waitKind = waitKind;
    rtos_pCurrent->pWaitObj = pWaitObj;
    return rtos_kernel_blockCurrent(timeout);
}

extern const uint8_t rtos_kernel_hiBitNibble[16];

/** Index of the highest set bit of a byte. Must not be called with 0. */
static inline uint8_t rtos_kernel_highestBit8(uint8_t b)
{
    return (b & 0xF0u) ? (uint8_t)(4u + rtos_kernel_hiBitNibble[b >> 4]) : rtos_kernel_hiBitNibble[b];
}

/**
 * Index of the highest set bit of a task mask, i.e. the highest priority task in it. Must
 * not be called with 0 (the ready mask always holds the idle bit). A port with a count
 * leading zeros instruction defines RTOS_PORT_HIGHEST_BIT(m) and the nibble table is not
 * used.
 */
static inline uint8_t rtos_kernel_highestBit(rtos_taskmask_t m)
{
#if defined(RTOS_PORT_HIGHEST_BIT)
    return (uint8_t)RTOS_PORT_HIGHEST_BIT(m);
#elif RTOS_CFG_NUM_TASKS + 1 > 16
    if(m & 0xFF000000ul)
        return (uint8_t)(24u + rtos_kernel_highestBit8((uint8_t)(m >> 24)));
    if(m & 0x00FF0000ul)
        return (uint8_t)(16u + rtos_kernel_highestBit8((uint8_t)(m >> 16)));
    if(m & 0x0000FF00ul)
        return (uint8_t)(8u + rtos_kernel_highestBit8((uint8_t)(m >> 8)));
    return rtos_kernel_highestBit8((uint8_t)m);
#elif RTOS_CFG_NUM_TASKS + 1 > 8
    if(m & 0xFF00u)
        return (uint8_t)(8u + rtos_kernel_highestBit8((uint8_t)(m >> 8)));
    return rtos_kernel_highestBit8((uint8_t)m);
#else
    return rtos_kernel_highestBit8((uint8_t)m);
#endif
}

/** Byte every task stack is filled with at creation; also the stack bottom canary. */
#define RTOS_STACK_FILL     0xA5u

/** Persistent panic record, placed by the port so that it survives a reset. */
typedef struct rtos_panicRecord
{
    uint32_t magic;
    rtos_panicInfo_t info;
    uint32_t check;
} rtos_panicRecord_t;
extern rtos_panicRecord_t rtos_panicRecord;
#define RTOS_PANIC_MAGIC    0x50414E43ul

/** Called by rtos_init: latch and clear the persistent record. */
void rtos_kernel_panicInit(void);

/** CPU load in percent from the idle time and the total time of an interval. idle * 100
    must not overflow the run time type (it did on the AVR beyond 42.9 M counts, 172 s of
    idle time at 16 MHz), so both are halved together until total * 100 fits: at most 7
    shifts for 32 bit counts, a bounded loop. */
static inline uint8_t rtos_kernel_loadPercent(rtos_runtime_t idle, rtos_runtime_t total)
{
    if(total == 0  ||  idle >= total)
        return 0;
    while(total > (rtos_runtime_t)-1 / 100u)
    {
        total >>= 1;
        idle >>= 1;
    }
    return (uint8_t)(100u - (uint8_t)((idle * 100u) / total));
}

/** Ticks until the next timed wake up or software timer expiry; RTOS_FOREVER if none.
    Interrupts disabled. Used by tickless idle. */
rtos_tick_t rtos_kernel_ticksToNextWake(void);

/** Ticks until the first software timer expires; RTOS_FOREVER if none. */
rtos_tick_t rtos_kernel_timerTicksToNext(void);

#if RTOS_CFG_TRACE
# define RTOS_TRACE(evt, a, b)  rtos_traceHook((uint8_t)(evt), (uint32_t)(a), (uint32_t)(b))
#else
# define RTOS_TRACE(evt, a, b)  do { } while(0)
#endif

/** Idle task part of the watchdog: check the monitored tasks, kick the hardware. */
void rtos_kernel_wdtIdle(void);

/** Timer task created by rtos_start when RTOS_CFG_TIMER_TASK_PRIO is set. */
void rtos_kernel_timerTaskCreate(void);
/** Pick the highest priority waiter in a mask (must not be 0), remove it and make it
    ready; returns its TCB. */
rtos_tcb_t *rtos_kernel_releaseHighestWaiter(rtos_taskmask_t *pWaiters);
void rtos_kernel_timerTick(rtos_tick_t now);

/* Read-only views for tests and debuggers; no locking, call with interrupts disabled. */
rtos_taskmask_t rtos_kernel_getReadyMask(void);
rtos_tcb_t *rtos_kernel_getTimedHead(void);

/* Task start in steps, for a port that starts tasks from a frameless stub: the first
   step sets the period base and enables interrupts and returns the function to call,
   the second the argument; the third never returns (called when the function does). */
rtos_taskFunc_t rtos_kernel_taskStarting(void);
void *rtos_kernel_taskStartArg(void);
void rtos_kernel_taskExit(void) __attribute__((noreturn));

/** Bookkeeping of the switch at the end of an interrupt (next task, stack check, trace,
    run time), out of line; the caller then calls rtos_port_switchContext itself. */
void rtos_kernel_isrPrepareSwitch(uint8_t hi);

/** Set a task's effective priority and keep rtos_kernel_boostedMask in step: the bit
    is set exactly while effPrio differs from prio. Interrupts disabled. */
RTOS_KERNEL_HOT_INLINE void rtos_kernel_setEffPrio(rtos_tcb_t *pT, uint8_t effPrio)
{
    pT->effPrio = effPrio;
    if(effPrio != pT->prio)
        rtos_kernel_boostedMask |= pT->bit;
    else
        rtos_kernel_boostedMask &= (rtos_taskmask_t)~pT->bit;
}

/** The slow paths of the scheduler and the interrupt tail for the moments when a ready
    task is boosted by a mutex ceiling (rtos_kernel_boostedMask & rtos_kernel_readyMask
    is not 0): the next task is then the highest effective priority among the ready
    tasks, not the highest ready bit. Out of line and cold, and the inline callers return
    right after calling them, so that the hot path neither pays a call nor keeps anything
    alive across one. The interrupt variant does the switch itself, one frame deeper than
    the hot path. Interrupts disabled. */
void rtos_kernel_scheduleBoosted(void) __attribute__((noinline, cold));
void rtos_kernel_isrTailBoosted(void) __attribute__((noinline, cold));

/** Body of rtos_isrTail for a port that calls it from its own interrupt epilogue without
    another call level: the switch is called from the caller's frame, so a task switched
    out of an interrupt leaves one frame less on its stack. */
RTOS_KERNEL_HOT_INLINE void rtos_kernel_isrTailInline(void)
{
    if(rtos_needSwitch)
    {
        rtos_needSwitch = 0;
        if((rtos_kernel_readyMask & rtos_kernel_boostedMask) != 0)
        {
            rtos_kernel_isrTailBoosted();
            return;
        }
        {
            const uint8_t hi = rtos_kernel_highestBit(rtos_kernel_readyMask);
            if(hi > rtos_pCurrent->effPrio)
            {
                rtos_kernel_isrPrepareSwitch(hi);
                rtos_port_switchContext();
            }
        }
    }
}

#endif  /* RTOS_KERNEL_INCLUDED */
