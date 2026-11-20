#ifndef RTOS_INCLUDED
#define RTOS_INCLUDED
/**
 * @file rtos.h
 * @brief Public API of the nornOS kernel.
 *
 * A fixed, compile-time known set of tasks, one unique priority per task (the task id is
 * its priority, 0 is the implicit idle task, a higher id means a higher priority), a
 * ready bitmask scheduler, a tick driven wake-up list sorted by time, per-task event
 * flags, counting semaphores and immediate-priority-ceiling mutexes. The kernel is written
 * in C99 and talks to the hardware only through the port layer (port.h).
 *
 * @par Calling contexts
 * The rule for the whole API: a function without a FromIsr suffix is for task context
 * and may block or switch task at once. Inside an RTOS_ISR body, a software timer callback
 * running in the tick (RTOS_CFG_TIMER_TASK_PRIO 0) and the trace hook, only these are
 * allowed: the *FromIsr variants, rtos_timerStart(), rtos_timerStop(), rtos_getTick(),
 * rtos_getTimeUs(), the plain readers (rtos_queueCount(), rtos_mboxIsFull(),
 * rtos_poolFreeCount(), rtos_evgroupGet(), rtos_timerIsActive(), rtos_timerRemaining())
 * and rtos_panic(). They never block and never switch; the switch happens once at the end
 * of the ISR. The idle task (the context of rtos_start() and the idle hook) must never
 * block either: a blocking call from it panics with RTOS_PANIC_ARGUMENT.
 *
 * @par Task ids
 * Task ids are 1 to RTOS_CFG_NUM_TASKS. Every function that takes one checks it with
 * RTOS_CHECK (compiled in every configuration, RTOS_PANIC_ARGUMENT on failure); the
 * diagnostics accept the idle task 0 as well, and rtos_getTaskInfo() reports an invalid
 * id by returning false instead.
 *
 * @par Task life cycle
 * A slot is unused until rtos_createTask() fills it; the task is then ready, running or
 * blocked (rtos_suspend() is a modifier on top of those), and if its function ever
 * returns it is terminated: RTOS_TASK_TERMINATED in rtos_getTaskInfo(), never scheduled
 * again, deaf to events, notifications and resumes. It must not hold a mutex at that
 * point (checked, see rtos_createTask()). The slot and the stack of a terminated task
 * stay in use. With RTOS_CFG_DYNAMIC_TASKS a task can also end with rtos_exit(), another
 * task reaps the slot with rtos_reapTask() and fills it again with rtos_startTask(),
 * which is rtos_createTask() for a running system (group Dynamic tasks). design.md in
 * the nornDocs repository has the states and the rules.
 *
 * @par Two kinds of checks
 * "Checked" in this documentation means RTOS_CHECK: always compiled, a violation is
 * rtos_panic() with RTOS_PANIC_ARGUMENT. "Asserted" means RTOS_ASSERT: compiled only with
 * RTOS_CFG_ASSERT, a violation calls rtos_assertFailed(). Without RTOS_CFG_ASSERT an
 * asserted contract that is broken is undefined behaviour.
 *
 * @par Timeouts
 * Every timeout is in ticks: RTOS_NO_WAIT polls, RTOS_FOREVER never times out, any
 * other value must be at most RTOS_MAX_TIMEOUT (checked). RTOS_MS() converts
 * milliseconds.
 *
 * @defgroup rtos_api nornOS kernel API
 * @brief Everything an application calls; see rtos.h for the calling context rules.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "rtos_config.h"
#include "rtos_types.h"
#include "port.h"

/* SMP (nornInternal/nornCore/smp_plan.md): one kernel over RTOS_PORT_CORES cores. Steps 1 to 3 of the plan
   are in: the option, the second signature word, the host port's cores, and the kernel's
   per core state (the running task, the next one, the switch request, an idle task per
   core, the other cores started by rtos_start into their idle loops). Step 4 schedules
   the N highest ranked ready tasks over the N cores (IPI to the core that must give way),
   step 5 accounts time and load per core (rtos_getCoreLoad, rtos_taskInfo_t.core), step
   6 adds affinity as a sub option (RTOS_CFG_SMP_AFFINITY, rtos_setAffinity). Off, nothing
   in the kernel changes. */
#ifndef RTOS_CFG_SMP_AFFINITY
# define RTOS_CFG_SMP_AFFINITY  0
#endif
#ifndef RTOS_PORT_SMP
# define RTOS_PORT_SMP          0       /* the port has the SMP primitives (spinlock, IPI, core start) */
#endif
#if RTOS_CFG_SMP && !RTOS_PORT_SMP
# error RTOS_CFG_SMP needs a port with SMP support (RTOS_PORT_SMP; so far the host port with RTOS_HOST_CORES > 1)
#endif
#if RTOS_CFG_SMP && RTOS_PORT_CORES < 2
# error RTOS_CFG_SMP on a port with one core
#endif
#if RTOS_CFG_SMP_AFFINITY && !RTOS_CFG_SMP
# error RTOS_CFG_SMP_AFFINITY needs RTOS_CFG_SMP
#endif
#if RTOS_CFG_SMP && RTOS_PORT_CORES > 8
# error at most 8 cores: a core mask is a byte
#endif

/** The second configuration word: the options whose default comes from the port (cores,
    SMP, affinity). Checked by rtos_init() only with RTOS_CFG_SMP, since none of them
    changes the layout of anything otherwise. */
#define RTOS_CONFIG_SIGNATURE2                                                      \
    ( (uint32_t)(RTOS_CFG_SMP != 0)                                                 \
    | ((uint32_t)(RTOS_CFG_SMP_AFFINITY != 0) << 1)                                 \
    | ((uint32_t)(RTOS_PORT_CORES & 15u) << 2) )

/**
 * @defgroup rtos_defs Constants and types
 * @ingroup rtos_api
 * @brief Limits, timeouts, static initializers and the kernel object types.
 * @{
 */

/*
 * Defines
 */

/** Version string of the kernel. */
#define RTOS_VERSION            "2.0.0-dev"

/** Task id of the implicit idle task. Priority 0, always ready. */
#define RTOS_IDLE_TASK          0u

/** Number of tasks including the idle task. */
#define RTOS_NUM_TASKS          (RTOS_CFG_NUM_TASKS + 1u)

/** Pseudo task id meaning "no task". */
#define RTOS_NO_TASK            0xFFu

/** @name Stack sizes
    From the port (README "Stacks"): RTOS_STACK_MIN is the technical minimum
    the kernel accepts, enough for the kernel and the port plus a few frames of the
    task's own and meant for very simple tasks whose use has been measured;
    RTOS_STACK_DEFAULT is the size to start from; RTOS_STACK_LARGE is the recommended
    starting size for tasks that print, nest calls or use floating point, not an upper
    bound: a deep enough call chain or a crypto or network task needs more (tc12 shows
    32 nested frames alone take 1.4 KB on the ESP32). The minimum is not a promise that
    every application works with it. Measure with rtos_getStackFree, the stack monitor,
    tc11 and tc12.
    @{ */
#define RTOS_STACK_MIN          RTOS_PORT_MIN_STACK     /**< Technical minimum. */
#define RTOS_STACK_DEFAULT      RTOS_PORT_DEFAULT_STACK /**< Size to start from. */
#define RTOS_STACK_LARGE        RTOS_PORT_LARGE_STACK   /**< Tasks that print, nest or use floats. */

/** Declares a static task stack of @p size bytes, aligned as the port wants it
    (RTOS_PORT_STACK_ALIGN), so that no byte is lost to the rounding rtos_createTask()
    otherwise does: `RTOS_STACK(_stack, RTOS_STACK_DEFAULT);`. */
#define RTOS_STACK(name, size)                                                  \
    static uint8_t name[size] __attribute__((aligned(RTOS_PORT_STACK_ALIGN)))

/**
 * @brief Declares the stack and creates the task in one line.
 *
 * `RTOS_TASK(1, blink, NULL, RTOS_STACK_DEFAULT);` is a static, aligned stack of
 * @p stackSize bytes named after the function plus the rtos_createTask() call, with the
 * same checks and the same cost. @p fn must be the function's name (it is pasted into the
 * stack's name), @p prio its priority and id, @p arg what it receives. For a stack the
 * application wants to own, or to pass a function pointer, call rtos_createTask().
 */
#define RTOS_TASK(prio, fn, arg, stackSize)                                     \
    do {                                                                        \
        RTOS_STACK(rtos_stack_##fn, stackSize);                                 \
        rtos_createTask((prio), (fn), (arg), rtos_stack_##fn, (stackSize));     \
    } while(0)
/** @} */

/** Timeout value meaning "wait forever". */
#define RTOS_FOREVER            ((rtos_tick_t)-1)

/** Timeout value meaning "do not block, just poll". */
#define RTOS_NO_WAIT            ((rtos_tick_t)0)

/** The longest finite timeout, timer delay or period, periodic wait and watchdog interval
    in ticks: half the range of the tick type (32767 ticks with 16 bit ticks, about 32.7 s
    at 1 kHz; 2^31 - 1 with 32 bit ticks, about 24.8 days). The kernel compares tick
    values as signed differences, which is what lets the counter wrap freely, and that
    only works within half the range. A larger value (other than RTOS_FOREVER) is a
    contract violation: RTOS_PANIC_ARGUMENT. rtos_delay is the exception: it takes any
    value and splits a longer delay into waits of at most this length. */
#define RTOS_MAX_TIMEOUT        ((rtos_tick_t)(RTOS_FOREVER >> 1))

/** Option for rtos_waitEvents: resume when any of the masked events is pending. */
#define RTOS_WAIT_ANY           0u

/** Option for rtos_waitEvents: resume only when all masked events are pending. */
#define RTOS_WAIT_ALL           1u

/** rtos_notify operation: replace the value. */
#define RTOS_NOTIFY_SET         0u
/** rtos_notify operation: OR the value into the current one (flags). */
#define RTOS_NOTIFY_OR          1u
/** rtos_notify operation: increment the current value by one (counting). */
#define RTOS_NOTIFY_INC         2u

/** Static initializer for a counting semaphore without an upper limit. */
#define RTOS_SEM_INIT(initialCount)     { (initialCount), 0xFFu, 0, 0 }

/** Static initializer for a binary semaphore: a give while the token is available is
    lost (and counted in the lost field). */
#define RTOS_SEM_INIT_BINARY(initialCount)  { (initialCount), 1u, 0, 0 }

/** Static initializer for a counting semaphore with an upper limit. */
#define RTOS_SEM_INIT_MAX(initialCount, maxCount)  { (initialCount), (maxCount), 0, 0 }

/** Static initializer for a ceiling mutex. The ceiling is the highest priority of any task
    that will ever lock the mutex. Start with RTOS_MUTEX_INIT_ANY and lower the ceiling only
    when a measurement shows the holder delaying a task with a deadline. */
#if RTOS_CFG_RUNTIME_STATS >= 2
# define RTOS_MUTEX_INIT(ceilingPrio)   { (ceilingPrio), RTOS_NO_TASK, 0, 0, 0 }
#else
# define RTOS_MUTEX_INIT(ceilingPrio)   { (ceilingPrio), RTOS_NO_TASK, 0, 0 }
#endif

/** The ceiling that is safe for any set of tasks: the highest priority there is. A holder
    then runs above every other task until it unlocks, so the section must be short; the
    choice for an application that would rather not work out the ceiling, and the right one
    for a lock any task may take (nornStore uses it). RTOS_MUTEX_INIT_ANY is the matching
    static initializer. Same code, same cost: the ceiling is a number in the mutex. */
#define RTOS_MUTEX_CEILING_ANY          ((uint8_t)RTOS_CFG_NUM_TASKS)
#define RTOS_MUTEX_INIT_ANY             RTOS_MUTEX_INIT(RTOS_MUTEX_CEILING_ANY)

/** Static initializer for a queue. \a storage is a uint8_t array of itemSize*capacity
    bytes. Capacity is at most 255. */
#define RTOS_QUEUE_INIT(storage, itemSize, capacity)                            \
    { (storage), (itemSize), (capacity), 0, 0, 0, 0, 0, 0, 0 }

/** Static initializer for a mailbox. \a storage is a uint8_t array of itemSize bytes. */
#define RTOS_MBOX_INIT(storage, itemSize)   { (storage), (itemSize), 0, 0, 0 }

/** Static initializer for a memory pool; rtos_poolInit must still be called to build the
    free list. */
#define RTOS_POOL_INIT(storage, blockSize, numBlocks)                           \
    { (storage), (blockSize), (numBlocks), 0, 0, NULL, (numBlocks), 0 }

/** Frequency of the run time counters returned by rtos_getTaskRunTime, in counts per
    second. */
#define RTOS_RUNTIME_HZ         RTOS_PORT_RUNTIME_HZ
/** Convert a run time count to milliseconds. */
#define RTOS_RUNTIME_TO_MS(t)   ((t) / (RTOS_RUNTIME_HZ / 1000ul))

/** Type of a run time count; width chosen by the port. */
typedef rtos_port_runtime_t rtos_runtime_t;

/** Convert milliseconds to ticks at compile time. */
#define RTOS_MS(ms)             ((rtos_tick_t)(((uint32_t)(ms) * RTOS_CFG_TICK_HZ) / 1000ul))
/** Convert seconds to ticks at compile time: RTOS_S(1) is one second. */
#define RTOS_S(s)               ((rtos_tick_t)((uint32_t)(s) * RTOS_CFG_TICK_HZ))


/*
 * Global type definitions
 */

/** The task function. It receives the argument given at creation and should never return;
    if it does, the task is silently terminated and never scheduled again. */
typedef void (*rtos_taskFunc_t)(void *arg);

/** Software timer callback. With RTOS_CFG_TIMER_TASK_PRIO 0 it runs in the tick interrupt
    with interrupts disabled: keep it short and use only *FromIsr kernel functions,
    rtos_timerStart/Stop and the readers. With a timer task (RTOS_CFG_TIMER_TASK_PRIO
    above 0) it runs in that task and may use the whole API. */
typedef void (*rtos_timerFunc_t)(void *arg);

/** A software timer. Treat as opaque; initialize with rtos_timerInit. */
typedef struct rtos_timer
{
    struct rtos_timer *pNext;   /**< Link in the sorted list of active timers. */
    struct rtos_timer *pNextPending; /**< Link in the list of expired timers waiting for
                                     the timer task (RTOS_CFG_TIMER_TASK_PRIO). */
    rtos_tick_t expiresAt;      /**< Absolute tick of the next expiry. */
    rtos_tick_t period;         /**< Reload period, 0 for a one shot timer. */
    rtos_timerFunc_t callback;  /**< Called at expiry. */
    void *arg;                  /**< Passed to the callback. */
    uint8_t active;             /**< 1 while in the list. */
    uint8_t pending;            /**< 1 while in the pending list. */
} rtos_timer_t;

/** A counting semaphore. Treat as opaque. */
typedef struct rtos_sem
{
    uint8_t count;              /**< Number of free tokens. */
    uint8_t maxCount;           /**< Upper limit of count; 1 for a binary semaphore. */
    rtos_taskmask_t waiters;    /**< Tasks blocked on this semaphore. */
    uint8_t lost;               /**< Gives that found the count at the limit (saturates). */
} rtos_sem_t;

/** A mutex using the immediate priority ceiling protocol. Treat as opaque. */
typedef struct rtos_mutex
{
    uint8_t ceiling;            /**< Priority the owner is raised to while holding it. */
    uint8_t owner;              /**< Owning task id or RTOS_NO_TASK. */
    uint8_t savedPrio;          /**< Owner's effective priority before the lock. */
    rtos_taskmask_t waiters;    /**< Fallback only: tasks blocked if ceiling was too low. */
#if RTOS_CFG_RUNTIME_STATS >= 2
    uint16_t contended;         /**< Locks that found the mutex held (saturating); 0 in a correct system. */
#endif
} rtos_mutex_t;


/** A message queue: fixed size items copied by value, FIFO. Treat as opaque. */
typedef struct rtos_queue
{
    uint8_t *buf;               /**< itemSize * capacity bytes of storage. */
    uint8_t itemSize;           /**< Bytes per item. */
    uint8_t capacity;           /**< Number of items the storage holds. */
    uint8_t count;              /**< Items currently stored. */
    uint8_t head;               /**< Index of the oldest item. */
    uint8_t tail;               /**< Index where the next item is written. */
    rtos_taskmask_t readers;    /**< Tasks blocked in receive. */
    rtos_taskmask_t writers;    /**< Tasks blocked in send. */
    uint8_t maxCount;           /**< High water mark of count. */
    uint16_t dropped;           /**< Sends that failed: full and no wait, or timed out. */
} rtos_queue_t;

/** A mailbox: one slot holding the most recent item. A post overwrites an unread item.
    Treat as opaque. */
typedef struct rtos_mbox
{
    uint8_t *buf;               /**< itemSize bytes of storage. */
    uint8_t itemSize;           /**< Bytes per item. */
    uint8_t full;               /**< 1 if the slot holds an unread item. */
    rtos_taskmask_t readers;    /**< Tasks blocked in fetch. */
    uint16_t overwrites;        /**< Posts that replaced an unread item. */
} rtos_mbox_t;


/** A pool of fixed size memory blocks. Treat as opaque. */
typedef struct rtos_pool
{
    uint8_t *storage;           /**< blockSize * numBlocks bytes. */
    uint16_t blockSize;         /**< Bytes per block, at least sizeof(void *). */
    uint8_t numBlocks;          /**< Total number of blocks. */
    uint8_t numFree;            /**< Currently free blocks. */
    rtos_taskmask_t waiters;    /**< Tasks blocked in alloc. */
    void *pFree;                /**< Head of the free list. */
    uint8_t minFree;            /**< Low water mark of numFree. */
    uint16_t failed;            /**< Allocations that returned NULL. */
} rtos_pool_t;


/** A stream buffer (RTOS_CFG_STREAMS): a ring of bytes, writes and reads of any length,
    one writer and one reader at a time. Treat as opaque. */
typedef struct rtos_stream
{
    uint8_t *buf;               /**< size bytes of storage. */
    uint16_t size;
    uint16_t head;              /**< Index of the oldest byte: the reader's. */
    uint16_t tail;              /**< Index where the next byte goes: the writer's. */
    uint16_t count;             /**< Bytes stored. */
    uint16_t trigger;           /**< The reader is woken when count reaches this. */
    rtos_taskmask_t readers;    /**< The task blocked in read, if any. */
    rtos_taskmask_t writers;    /**< The task blocked in write, if any. */
} rtos_stream_t;

/** Why the kernel stopped the system; the first field of rtos_panicInfo_t. */
typedef enum rtos_panicReason
{
    RTOS_PANIC_NONE = 0,
    RTOS_PANIC_ASSERT,          /**< pFile/line: the failed RTOS_ASSERT. */
    RTOS_PANIC_STACK_OVERFLOW,  /**< arg1: the task whose stack bottom was overwritten; arg2: 0
                                     from the canary check, the PC of the write from the
                                     hardware guard (RTOS_CFG_STACK_GUARD). */
    RTOS_PANIC_WATCHDOG,        /**< arg1: task that did not check in, RTOS_NO_TASK: idle. */
    RTOS_PANIC_DEADLOCK,        /**< arg1: task that would block, arg2: task it waits for. */
    RTOS_PANIC_FAULT,           /**< CPU exception; arg1: cause, arg2: address (port). */
    RTOS_PANIC_CONFIG,          /**< rtos_init: the application was compiled against another
                                     configuration than the kernel; arg1: the application's
                                     signature, arg2: the kernel's (RTOS_CONFIG_SIGNATURE). */
    RTOS_PANIC_ARGUMENT,        /**< A mandatory check refused an argument or an input that
                                     would corrupt memory (RTOS_CHECK in rtos_assert.h, compiled
                                     in every configuration): a task id out of range, a stack
                                     too small, a NULL storage or callback, a foreign block
                                     freed; arg1 and arg2 name the values. */
    RTOS_PANIC_APPLICATION      /**< rtos_panic called by the application; args are its own. */
} rtos_panicReason_t;

/** What happened; filled in by rtos_panic and kept across a reset. */
typedef struct rtos_panicInfo
{
    uint8_t reason;             /**< rtos_panicReason_t. */
    uint8_t task;               /**< Task that was running. */
    uint16_t line;              /**< Source line of a failed assertion. */
    uint32_t arg1;
    uint32_t arg2;
    rtos_tick_t tick;           /**< Time of the panic. */
    const char *pFile;          /**< Source file of a failed assertion; only meaningful in
                                     the same firmware image. */
} rtos_panicInfo_t;

/** @name Reset causes, returned by rtos_getResetCause()
    @{ */
#define RTOS_RESET_UNKNOWN      0u
#define RTOS_RESET_POWER_ON     1u
#define RTOS_RESET_EXTERNAL     2u
#define RTOS_RESET_BROWN_OUT    3u
#define RTOS_RESET_WATCHDOG     4u
#define RTOS_RESET_SOFTWARE     5u
#define RTOS_RESET_DEEP_SLEEP   6u      /**< Woke from deep sleep (hal/power.h); RAM is fresh, POWER_RTC_DATA is kept. */
/** @} */

/** @name Wait kinds: what a task is blocked on, in rtos_taskInfo_t
    @{ */
#define RTOS_WAIT_NONE          0u
#define RTOS_WAIT_DELAY         1u
#define RTOS_WAIT_PERIODIC      2u
#define RTOS_WAIT_EVENTS        3u
#define RTOS_WAIT_SEM           4u
#define RTOS_WAIT_MUTEX         5u
#define RTOS_WAIT_QUEUE_SEND    6u
#define RTOS_WAIT_QUEUE_RECV    7u
#define RTOS_WAIT_MBOX          8u
#define RTOS_WAIT_POOL          9u
#define RTOS_WAIT_NOTIFY        10u
#define RTOS_WAIT_EVGROUP       11u
#define RTOS_WAIT_TERMINATED    12u /**< The task function returned: final, see rtos_createTask(). */
#define RTOS_WAIT_STREAM_READ   13u
#define RTOS_WAIT_STREAM_WRITE  14u
/** @} */

/** @name Task states, in rtos_taskInfo_t
    @{ */
#define RTOS_TASK_NONE          0u  /**< Never created. */
#define RTOS_TASK_RUNNING       1u
#define RTOS_TASK_READY         2u
#define RTOS_TASK_BLOCKED       3u
#define RTOS_TASK_SUSPENDED     4u  /**< rtos_suspend; waitKind says what it was doing. */
#define RTOS_TASK_TERMINATED    5u  /**< Its function returned or it called rtos_exit();
                                         never runs again. rtos_reapTask() frees the slot. */
/** @} */

/** Result of a call that reports failure instead of panicking: the dynamic task API
    (RTOS_CFG_DYNAMIC_TASKS). rtos_errName() gives the name for a message. */
typedef enum rtos_err
{
    RTOS_OK = 0,
    RTOS_ERR_BUSY,              /**< The slot is in use (created, running or terminated and not yet reaped). */
    RTOS_ERR_STATE,             /**< The task is not in the state the call needs (not terminated). */
    RTOS_ERR_ARG,               /**< An argument out of range: the id, a NULL function or stack, a stack below RTOS_STACK_MIN. */
    RTOS_ERR_CONTEXT            /**< Called from an interrupt, where the port can tell. */
} rtos_err_t;

/** rtos_evgroupWait option: clear the bits that satisfied the wait, atomically. */
#define RTOS_WAIT_CLEAR         0x02u

/** An event group: flags shared by any number of tasks. Treat as opaque. */
typedef struct rtos_evgroup
{
    rtos_evt_t bits;            /**< Current flags. */
    rtos_taskmask_t waiters;    /**< Tasks blocked in rtos_evgroupWait. */
} rtos_evgroup_t;

/** Static initializer for an event group. */
#define RTOS_EVGROUP_INIT(initialBits)  { (initialBits), 0 }

/** Heap statistics, see rtos_heapStats. */
typedef struct rtos_heapStats
{
    size_t total;               /**< Bytes handed to rtos_heapInit. */
    size_t free;                /**< Bytes in free blocks now. */
    size_t minFree;             /**< Lowest value of free so far. */
    size_t largestFree;         /**< Largest single free block now. */
    uint16_t allocs;            /**< Successful rtos_malloc calls. */
    uint16_t frees;             /**< rtos_free calls. */
    uint16_t failed;            /**< rtos_malloc calls that returned NULL. */
} rtos_heapStats_t;

/** Snapshot of a task for diagnostics, see rtos_getTaskInfo. */
typedef struct rtos_taskInfo
{
    uint8_t state;              /**< RTOS_TASK_*. */
    uint8_t prio;
    uint8_t effPrio;            /**< prio, or a mutex ceiling while holding it. */
    uint8_t waitKind;           /**< RTOS_WAIT_* while blocked. */
    const void *pWaitObj;       /**< The semaphore, mutex, queue, mailbox or pool. */
    bool hasTimeout;            /**< wakeAt is valid. */
    rtos_tick_t wakeAt;         /**< Tick of the timeout. */
    size_t stackSize;
    size_t stackFree;
    rtos_runtime_t runTime;     /**< 0 without RTOS_CFG_RUNTIME_STATS. */
    uint16_t switches;          /**< Times switched in, wrapping; 0 below RTOS_CFG_RUNTIME_STATS 2. */
    uint8_t overruns;
    uint8_t mutexHeld;          /**< Mutexes the task owns; 0 without RTOS_CFG_DYNAMIC_TASKS. */
    uint8_t core;               /**< The core running it, RTOS_NO_CORE when it does not run (0 on one core). */
} rtos_taskInfo_t;

/** rtos_taskInfo_t.core of a task that is not running. */
#define RTOS_NO_CORE            0xFFu

/** @} */


/*
 * Global prototypes
 */

/**
 * @defgroup rtos_life Kernel life cycle
 * @ingroup rtos_api
 * @brief Initialize the kernel, create the tasks, start the scheduler.
 * @{
 */

/**
 * @brief The function behind rtos_init(); call rtos_init() instead.
 *
 * @param configSignature
 *     RTOS_CONFIG_SIGNATURE as the application sees it.
 */
void rtos_initChecked(uint32_t configSignature);

#if RTOS_CFG_SMP
/**
 * @brief rtos_initChecked() with the second configuration word (RTOS_CFG_SMP builds).
 *
 * @param configSignature2
 *     RTOS_CONFIG_SIGNATURE2 as the application sees it; a mismatch is RTOS_PANIC_CONFIG
 *     with arg1 the application's word and arg2 the kernel's, as for the first word.
 */
void rtos_initChecked2(uint32_t configSignature, uint32_t configSignature2);
#endif

/**
 * @brief Resets all kernel data; call once before any other kernel function.
 *
 * Clears every task slot, the ready mask, the wake-up list and the tick count, and takes
 * the panic record of the previous boot, if any, out of the memory that survives a reset
 * (see rtos_getLastPanic()).
 *
 * The macro passes RTOS_CONFIG_SIGNATURE, every layout changing option of the
 * application's rtos_config.h folded into one number, to rtos_initChecked(). A kernel
 * compiled with another configuration (stale objects, a library) stops at once instead
 * of corrupting memory. An RTOS_CFG_SMP build passes RTOS_CONFIG_SIGNATURE2 as well.
 *
 * @pre Called from main(), before rtos_createTask() and rtos_start(), and only once.
 *
 * @post No task exists; the tick count is 0.
 *
 * @warning A configuration mismatch panics with RTOS_PANIC_CONFIG (arg1: the
 *          application's signature, arg2: the kernel's).
 *
 * @see rtos_createTask(), rtos_start()
 */
#if RTOS_CFG_SMP
# define rtos_init()    rtos_initChecked2(RTOS_CONFIG_SIGNATURE, RTOS_CONFIG_SIGNATURE2)
#else
# define rtos_init()    rtos_initChecked(RTOS_CONFIG_SIGNATURE)
#endif

/**
 * @brief Creates a task: a priority 1..RTOS_CFG_NUM_TASKS, a function, its argument and
 *        a stack the application owns.
 *
 * The short form: one task per priority, the priority is the task id, a higher number
 * runs first; the stack is a static array, RTOS_STACK_DEFAULT bytes to begin with; call
 * it from main() after rtos_init(), and the task runs from rtos_start(). RTOS_TASK()
 * declares the stack and makes this call in one line; RTOS_STACK() declares an aligned
 * stack for this call.
 *
 * @param prio
 *     Task priority and id, 1 to RTOS_CFG_NUM_TASKS. Must not be in use.
 * @param taskFunc
 *     Entry function of the task; an endless loop, as a rule. Must not be NULL.
 * @param arg
 *     Passed to @p taskFunc unchanged. May be NULL.
 * @param stackArea
 *     Stack memory, valid for as long as the system runs (static storage).
 * @param stackSize
 *     Its size in bytes, at least RTOS_STACK_MIN after the alignment below. Measure with
 *     rtos_getStackFree() or the stack monitor.
 *
 * @pre rtos_init() has been called.
 * @pre Called from main() before rtos_start(). While the scheduler runs, rtos_startTask()
 *      (RTOS_CFG_DYNAMIC_TASKS) creates a task.
 * @post The task is ready; it starts in @p taskFunc with @p arg.
 *
 * @warning Checked: @p prio out of range, a NULL @p taskFunc or @p stackArea, a slot in
 *          use or a stack below the minimum panic with RTOS_PANIC_ARGUMENT. There is no
 *          error return.
 *
 * @par The details
 *     The start of the stack area is moved up and its size rounded down to the stack
 *     alignment of the port (RTOS_PORT_STACK_ALIGN: 1 byte on the AVR, 16 on the ESP32),
 *     which may cost a few bytes unless the array is declared with RTOS_STACK(). With
 *     RTOS_CFG_STACK_CHECK the whole area is filled with a pattern, which
 *     rtos_getStackFree() and the stack monitor read back. The kernel never allocates
 *     memory for a task.
 *
 * @par A task function that returns
 *     The task is terminated: RTOS_TASK_TERMINATED in rtos_getTaskInfo(), never
 *     scheduled again, its slot and stack kept. It must not hold a mutex then: a mutex
 *     taken into the grave keeps its owner for ever and every later lock waits without
 *     a deadlock panic. Checked for every mutex whose ceiling is above the task's
 *     priority (RTOS_PANIC_ARGUMENT with the task id and its effective priority); a
 *     mutex whose ceiling equals the priority cannot be told apart and is the caller's
 *     responsibility.
 *
 * @see RTOS_TASK(), RTOS_STACK(), rtos_taskFunc_t, RTOS_STACK_MIN, rtos_getStackFree()
 */
void rtos_createTask( uint8_t prio
                    , rtos_taskFunc_t taskFunc
                    , void *arg
                    , uint8_t *stackArea
                    , size_t stackSize
                    );

/**
 * @brief Starts the tick and the scheduler; never returns.
 *
 * Creates the timer task when RTOS_CFG_TIMER_TASK_PRIO is above 0, starts the tick
 * timer and switches to the highest priority ready task. The calling context becomes the
 * idle task: it runs rtos_idleHook(), the watchdog and the stack monitor when they are
 * configured, and sleeps (RTOS_CFG_IDLE_SLEEP) until the next interrupt.
 *
 * @pre rtos_init() has been called and the tasks are created.
 *
 * @see rtos_init(), rtos_createTask(), rtos_idleHook()
 */
void rtos_start(void);

/** @} */


/**
 * @defgroup rtos_time Time and delays
 * @ingroup rtos_api
 * @brief The tick, delays and drift free periodic tasks.
 * @{
 */

/**
 * @brief Returns the number of ticks since rtos_start().
 *
 * The counter is RTOS_CFG_TICK_BITS wide and wraps (16 bits: after 65.5 s at 1 kHz;
 * 32 bits: after 49.7 days). Compare tick values as differences, never directly.
 *
 * @return The current tick count.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 *
 * @see rtos_getTimeUs(), RTOS_MS()
 */
rtos_tick_t rtos_getTick(void);

/**
 * @brief Blocks the calling task for at least @p ticks ticks.
 *
 * The task wakes on the tick @p ticks + 1 from now, so the real delay is between @p ticks
 * and @p ticks + 1 tick periods. A delay of 0 waits for the next tick: it is not a yield.
 *
 * @param ticks
 *     Delay in ticks; any value. RTOS_FOREVER blocks for good: nothing can wake the task
 *     again. A delay of RTOS_MAX_TIMEOUT or more is done as a sequence of waits, each of
 *     which may add up to one tick.
 *
 * @par Context
 *     Task only, not the idle task (checked).
 *
 * @see rtos_waitPeriodic(), RTOS_MS()
 */
void rtos_delay(rtos_tick_t ticks);

/**
 * @brief Blocks the calling task until its next period boundary.
 *
 * Each boundary is the previous boundary plus @p period, so the task runs drift free
 * regardless of its execution time. The first boundary is the start of the task plus
 * @p period.
 *
 * If the boundary has already passed (an overrun), the call returns at once, counts the
 * overrun (rtos_getOverrunCount()) and the next boundary is computed from now. A task
 * whose start up work takes longer than one period therefore sees an overrun at its
 * first call; that is harmless.
 *
 * @param period
 *     Period in ticks, 1 to RTOS_MAX_TIMEOUT. It may change from call to call.
 *
 * @return true if the boundary had already passed (overrun), false after a normal wait.
 *
 * @par Context
 *     Task only, not the idle task.
 *
 * @warning Checked: a period of 0 or above RTOS_MAX_TIMEOUT panics with
 *          RTOS_PANIC_ARGUMENT.
 *
 * @see rtos_getOverrunCount(), rtos_delay()
 */
bool rtos_waitPeriodic(rtos_tick_t period);

/** @} */


/**
 * @defgroup rtos_events Task event flags
 * @ingroup rtos_api
 * @brief RTOS_CFG_EVT_BITS private flags per task; any task or ISR posts, the owner waits.
 * @{
 */

/**
 * @brief Waits until events in @p mask are pending for the calling task.
 *
 * Events posted earlier stay pending until a wait consumes them, so nothing is lost
 * between two waits. The events that satisfy the wait are cleared and returned; pending
 * events outside @p mask are kept.
 *
 * @param mask
 *     The events to wait for. Must not be 0 (asserted).
 *
 * @param opt
 *     RTOS_WAIT_ANY: resume when any event of @p mask is pending. RTOS_WAIT_ALL: resume
 *     only when all of them are.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return The consumed events (with RTOS_WAIT_ALL: @p mask), 0 on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_postEvents(), rtos_postEventsFromIsr(), rtos_evgroupWait()
 */
rtos_evt_t rtos_waitEvents(rtos_evt_t mask, uint8_t opt, rtos_tick_t timeout);

/**
 * @brief Posts events to a task.
 *
 * The events are ORed into the task's pending events. If that satisfies the task's
 * wait, it becomes ready and, if its priority is above the caller's, runs before this
 * function returns.
 *
 * @param taskId
 *     Receiving task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @param events
 *     The events to post.
 *
 * @par Context
 *     Task only. From an ISR body use rtos_postEventsFromIsr().
 *
 * @see rtos_waitEvents(), rtos_broadcastEvents()
 */
void rtos_postEvents(uint8_t taskId, rtos_evt_t events);

/**
 * @brief Posts events to a task from an ISR body.
 *
 * As rtos_postEvents(); a switch to the woken task happens at the end of the ISR.
 *
 * @param taskId
 *     Receiving task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @param events
 *     The events to post.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void rtos_postEventsFromIsr(uint8_t taskId, rtos_evt_t events);

/**
 * @brief Posts events to every created task.
 *
 * @param events
 *     The events to post to each task (not to the idle task).
 *
 * @par Context
 *     Task only.
 *
 * @see rtos_postEvents()
 */
void rtos_broadcastEvents(rtos_evt_t events);

/** @} */


/**
 * @defgroup rtos_sem Counting semaphores
 * @ingroup rtos_api
 * @brief Token counters with direct hand-over to the highest priority waiter.
 * @{
 */

/**
 * @brief Initializes a counting semaphore at run time.
 *
 * The alternative to RTOS_SEM_INIT(). The count is limited only by its type, 255.
 *
 * @param pSem
 *     The semaphore.
 *
 * @param initialCount
 *     Tokens available at the start.
 *
 * @pre No task waits on the semaphore.
 *
 * @see rtos_semInitMax()
 */
void rtos_semInit(rtos_sem_t *pSem, uint8_t initialCount);

/**
 * @brief Initializes a semaphore with an upper limit at run time.
 *
 * A give that finds the count at @p maxCount is dropped and counted in the semaphore's
 * lost field. @p maxCount 1 makes a binary semaphore.
 *
 * @param pSem
 *     The semaphore.
 *
 * @param initialCount
 *     Tokens available at the start, at most @p maxCount (asserted).
 *
 * @param maxCount
 *     Upper limit of the count, at least 1 (asserted).
 *
 * @pre No task waits on the semaphore.
 *
 * @see RTOS_SEM_INIT_MAX(), RTOS_SEM_INIT_BINARY()
 */
void rtos_semInitMax(rtos_sem_t *pSem, uint8_t initialCount, uint8_t maxCount);

/**
 * @brief Takes one token, waiting up to @p timeout ticks for one.
 *
 * @param pSem
 *     The semaphore.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return true if a token was taken, false on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_semGive()
 */
bool rtos_semTake(rtos_sem_t *pSem, rtos_tick_t timeout);

/**
 * @brief Gives one token.
 *
 * If tasks wait, the highest priority waiter gets the token directly (the count does not
 * change) and may run before this function returns. Otherwise the count goes up, or, at
 * the limit, the give is dropped and counted as lost.
 *
 * @param pSem
 *     The semaphore.
 *
 * @par Context
 *     Task only. From an ISR body use rtos_semGiveFromIsr().
 *
 * @see rtos_semTake()
 */
void rtos_semGive(rtos_sem_t *pSem);

/**
 * @brief Gives one token from an ISR body.
 *
 * As rtos_semGive(); a switch to the woken task happens at the end of the ISR.
 *
 * @param pSem
 *     The semaphore.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void rtos_semGiveFromIsr(rtos_sem_t *pSem);

/**
 * @brief Sets the count anew, handing the tokens to waiting tasks first.
 *
 * For a restart of the thing the semaphore counts (a buffer pool refilled, a device
 * reinitialized): the count becomes @p count, less one per task that was waiting and is
 * woken with a token, highest priority first; the lost count is cleared. Tasks that keep
 * waiting wait for the next give.
 *
 * @param count
 *     The new number of tokens, at most the limit of rtos_semInitMax() (checked).
 *
 * @par Context
 *     Task only.
 */
void rtos_semReset(rtos_sem_t *pSem, uint8_t count);

/** @} */


/**
 * @defgroup rtos_critical Critical sections
 * @ingroup rtos_api
 * @brief A few instructions with interrupts masked: the cheapest exclusion there is.
 *
 * For a shared variable that a task and an interrupt both touch, or two tasks without a
 * mutex, when the protected code is a handful of instructions: a counter, a flag pair, a
 * pointer swap. Nestable: the state found at entry is restored at exit, so a critical
 * section inside another does not enable interrupts early. There is no scheduler lock
 * besides this; the kernel's own operations use the same mask for a bounded number of
 * instructions, which is what sets the interrupt latency figures in the README.
 *
 * @code
 *     const rtos_cs_t cs = rtos_enterCritical();
 *     ++ _count;
 *     rtos_exitCritical(cs);
 * @endcode
 *
 * Keep the section short (tens of instructions at most; tc10 measures the pair itself at
 * 3 cycles on the Mega and 6 on the ESP32), never block inside it and do not call a
 * kernel function that may switch tasks (the *FromIsr variants and the plain readers
 * are fine). Interrupts stay masked until the matching rtos_exitCritical().
 * @{
 */

/** Interrupt state saved by rtos_enterCritical(), restored by rtos_exitCritical(). */
typedef rtos_port_cs_t rtos_cs_t;

/**
 * @brief Masks interrupts and returns the state to restore.
 *
 * @return The interrupt state at entry; pass it to rtos_exitCritical().
 *
 * @par Context
 *     Any: task, idle task or RTOS_ISR body.
 */
static inline rtos_cs_t rtos_enterCritical(void)
{
    return rtos_port_enterCritical();
}

/**
 * @brief Restores the interrupt state saved by rtos_enterCritical().
 *
 * @param cs
 *     The value rtos_enterCritical() returned for this section.
 *
 * @par Context
 *     Any; the same context as the matching rtos_enterCritical().
 */
static inline void rtos_exitCritical(rtos_cs_t cs)
{
    rtos_port_exitCritical(cs);
}

/** @} */


/**
 * @defgroup rtos_mutex Ceiling mutexes
 * @ingroup rtos_api
 * @brief Immediate priority ceiling protocol: no priority inversion, no deadlock.
 *
 * @par Scheduling order while a mutex is held
 *     Tasks are ordered by the key (effective priority, boosted, base priority), where
 *     boosted means the task holds a mutex with a ceiling above its own priority. The
 *     highest effective priority runs; at an equal effective priority the owner raised
 *     to the ceiling runs before the task whose base priority is the ceiling, and two
 *     owners raised to one ceiling (possible only if one blocked while holding) run in
 *     base priority order. A ready task preempts the running one only if its key is
 *     strictly greater. The order is total: no two tasks ever tie.
 * @{
 */

/**
 * @brief Initializes a mutex at run time.
 *
 * The alternative to RTOS_MUTEX_INIT().
 *
 * @param pMutex
 *     The mutex.
 *
 * @param ceilingPrio
 *     The highest priority of any task that will ever lock the mutex, or
 *     RTOS_MUTEX_CEILING_ANY when that is not known or any task may lock it. The recipe:
 *     start with RTOS_MUTEX_CEILING_ANY on every mutex, which is correct for any set of
 *     tasks including a library's and a dynamic task's and costs nothing (the ceiling is a
 *     number), and lower a ceiling only when a measurement shows the holder delaying a task
 *     with a deadline; a library takes its ceiling as a configuration macro with that
 *     default (STORE_CFG_MUTEX_CEILING in nornStore). A ceiling that is too low is caught
 *     by an assertion at the first lock from above it, and with assertions off by
 *     rtos_mutexContended().
 *
 * @pre The mutex is not locked.
 */
void rtos_mutexInit(rtos_mutex_t *pMutex, uint8_t ceilingPrio);

#if RTOS_CFG_RUNTIME_STATS >= 2 || defined(DOXYGEN)
/**
 * @brief How often a lock found this mutex held.
 *
 * Under the ceiling protocol a lock never finds the mutex held unless a ceiling is too
 * low or an owner blocked while holding it, so in a correct system the count stays 0;
 * a count that grows names the mutex to look at. Counts rtos_mutexLock, rtos_mutexTryLock
 * and rtos_mutexLockTimeout alike, saturating at 65535. Needs RTOS_CFG_RUNTIME_STATS 2,
 * the counting level, where the switch counts live too.
 *
 * @param pMutex
 *     The mutex.
 *
 * @return The count since rtos_mutexInit() or the static initializer.
 *
 * @see rtos_getMutexContended()
 */
uint16_t rtos_mutexContended(const rtos_mutex_t *pMutex);

/**
 * @brief The same count over every mutex of the system.
 *
 * What nosh `sched` prints as "mutex contended". Saturating at 65535. Needs
 * RTOS_CFG_RUNTIME_STATS 2.
 *
 * @return Locks that found a mutex held since start.
 */
uint16_t rtos_getMutexContended(void);
#endif

/**
 * @brief Locks the mutex.
 *
 * The caller's effective priority is raised to the ceiling while it holds the mutex, so
 * no other task that uses the mutex can run in between. With a correct ceiling the call
 * therefore never blocks. If the mutex is held anyway (ceiling too low, or the owner
 * blocked while holding it), the caller waits without a timeout and the unlock hands
 * the mutex over to the highest priority waiter.
 *
 * @param pMutex
 *     The mutex.
 *
 * @par Context
 *     Task only, not the idle task (asserted).
 *
 * @pre The caller does not hold the mutex already: it is not recursive (asserted).
 * @pre The caller's priority is at most the ceiling (asserted).
 *
 * @warning A wait that would close a cycle of tasks waiting for each other's mutexes
 *          panics with RTOS_PANIC_DEADLOCK (arg1: the caller, arg2: the owner).
 *
 * @see rtos_mutexUnlock(), rtos_mutexTryLock(), rtos_mutexLockTimeout()
 */
void rtos_mutexLock(rtos_mutex_t *pMutex);

/**
 * @brief Locks the mutex if it is free, never waits.
 *
 * For code that has something else to do when the mutex is held: a poll from a loop that
 * also serves other work, or a diagnostic that must not block. With a correct ceiling the
 * mutex is free whenever this is called from a task below the ceiling, so the false branch
 * is for the exceptions (an owner that blocked while holding it).
 *
 * @return true if the caller owns the mutex now, false if another task holds it.
 *
 * @par Context
 *     Task only, not the idle task (asserted).
 *
 * @pre As rtos_mutexLock(): not held by the caller, caller's priority at most the ceiling.
 */
bool rtos_mutexTryLock(rtos_mutex_t *pMutex);

/**
 * @brief Locks the mutex, waiting at most @p timeout ticks for a hand-over.
 *
 * rtos_mutexLock() with a bounded wait: if the mutex is held and not handed over within
 * @p timeout, the call returns false and the caller holds nothing. RTOS_FOREVER is
 * rtos_mutexLock(), RTOS_NO_WAIT is rtos_mutexTryLock(). A wait with a timeout is not
 * checked for a deadlock cycle: it ends by itself.
 *
 * @return true if the caller owns the mutex now, false on timeout.
 *
 * @warning Checked: @p timeout above RTOS_MAX_TIMEOUT (RTOS_PANIC_ARGUMENT).
 *
 * @par Context
 *     Task only, not the idle task (asserted).
 *
 * @pre As rtos_mutexLock().
 */
bool rtos_mutexLockTimeout(rtos_mutex_t *pMutex, rtos_tick_t timeout);

/**
 * @brief Unlocks the mutex.
 *
 * The caller's effective priority drops back to what it was before the lock. If a task
 * waits, it becomes the owner at once, at the ceiling. Either change may let another task
 * run before this function returns.
 *
 * @param pMutex
 *     The mutex.
 *
 * @par Context
 *     Task only.
 *
 * @pre The caller owns the mutex (asserted).
 * @pre When holding several mutexes, unlock in the reverse order of locking (asserted).
 *
 * @see rtos_mutexLock()
 */
void rtos_mutexUnlock(rtos_mutex_t *pMutex);

/** @} */


/**
 * @defgroup rtos_queue Message queues
 * @ingroup rtos_api
 * @brief FIFO of fixed size items copied by value, with direct hand-over.
 * @{
 */

/**
 * @brief Initializes a queue at run time.
 *
 * The alternative to RTOS_QUEUE_INIT().
 *
 * @param pQ
 *     The queue.
 *
 * @param storage
 *     @p itemSize * @p capacity bytes of storage. Must not be NULL and must stay valid
 *     for as long as the queue is used.
 *
 * @param itemSize
 *     Bytes per item, 1 to 255.
 *
 * @param capacity
 *     Number of items the queue holds, 1 to 255.
 *
 * @pre No task waits on the queue.
 *
 * @warning Checked: a NULL @p storage, an @p itemSize or a @p capacity of 0 panic with
 *          RTOS_PANIC_ARGUMENT.
 */
void rtos_queueInit(rtos_queue_t *pQ, uint8_t *storage, uint8_t itemSize, uint8_t capacity);

/**
 * @brief Copies an item into the queue, waiting up to @p timeout ticks for room.
 *
 * If a task is blocked in receive, the item is copied straight into that task's buffer
 * and the task may run before this function returns. A send that fails is counted in
 * the queue's dropped field.
 *
 * @param pQ
 *     The queue.
 *
 * @param pItem
 *     The item, itemSize bytes. While the caller waits, the item is read at the moment a
 *     receiver frees a slot: it must not change until the call returns.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return true if the item was queued, false if the queue stayed full.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_queueSendFromIsr(), rtos_queueReceive()
 */
bool rtos_queueSend(rtos_queue_t *pQ, const void *pItem, rtos_tick_t timeout);

/**
 * @brief Copies an item into the queue from an ISR body; never blocks.
 *
 * @param pQ
 *     The queue.
 *
 * @param pItem
 *     The item, itemSize bytes.
 *
 * @return true if the item was queued, false if the queue was full (counted as dropped).
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
bool rtos_queueSendFromIsr(rtos_queue_t *pQ, const void *pItem);

/**
 * @brief Copies the oldest item out of the queue, waiting up to @p timeout ticks for one.
 *
 * If a task is blocked in send because the queue was full, its item moves into the freed
 * slot and that task may run before this function returns.
 *
 * @param pQ
 *     The queue.
 *
 * @param pItem
 *     Receives the item, itemSize bytes.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return true if an item was received, false on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_queueSend(), rtos_queueReceiveFromIsr()
 */
bool rtos_queueReceive(rtos_queue_t *pQ, void *pItem, rtos_tick_t timeout);

/**
 * @brief Copies the oldest item out of the queue from an ISR body; never blocks.
 *
 * @param pQ
 *     The queue.
 *
 * @param pItem
 *     Receives the item, itemSize bytes.
 *
 * @return true if an item was received, false if the queue was empty.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
bool rtos_queueReceiveFromIsr(rtos_queue_t *pQ, void *pItem);

/**
 * @brief Returns the number of items in the queue.
 *
 * @param pQ
 *     The queue.
 *
 * @return Items currently stored; a snapshot that may change at once.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 */
uint8_t rtos_queueCount(const rtos_queue_t *pQ);

/** @brief Items the queue can still take. Any context. */
uint8_t rtos_queueSpace(const rtos_queue_t *pQ);

/**
 * @brief Copies the oldest item without taking it out.
 *
 * For a consumer that decides on the item before committing to it: a protocol header, a
 * command it may not be ready for. The next rtos_queueReceive() returns the same item.
 *
 * @return true and the item in @p pItem, false if the queue is empty.
 *
 * @par Context
 *     Any.
 */
bool rtos_queuePeek(const rtos_queue_t *pQ, void *pItem);

/**
 * @brief Drops every item in the queue.
 *
 * Tasks blocked in rtos_queueSend() put their items into the emptied queue, highest
 * priority first, and are woken, as a receive would do for one of them. Tasks blocked in
 * rtos_queueReceive() keep waiting. The statistics (high water mark, dropped sends) are
 * kept. For a restart of the consumer, between frames or modes.
 *
 * @par Context
 *     Task only.
 */
void rtos_queueReset(rtos_queue_t *pQ);

/** @} */


/**
 * @defgroup rtos_mbox Mailboxes
 * @ingroup rtos_api
 * @brief One slot holding the latest item; a post never blocks and overwrites.
 * @{
 */

/**
 * @brief Initializes a mailbox at run time.
 *
 * The alternative to RTOS_MBOX_INIT().
 *
 * @param pMb
 *     The mailbox.
 *
 * @param storage
 *     @p itemSize bytes of storage. Must not be NULL and must stay valid for as long as
 *     the mailbox is used.
 *
 * @param itemSize
 *     Bytes per item, 1 to 255.
 *
 * @pre No task waits on the mailbox.
 *
 * @warning Checked: a NULL @p storage or an @p itemSize of 0 panic with
 *          RTOS_PANIC_ARGUMENT.
 */
void rtos_mboxInit(rtos_mbox_t *pMb, uint8_t *storage, uint8_t itemSize);

/**
 * @brief Puts an item in the mailbox, replacing an unread one; never blocks.
 *
 * If a task is blocked in fetch, the item is copied straight into that task's buffer
 * (the mailbox stays empty) and the task may run before this function returns. A
 * replaced item is counted in the mailbox's overwrites field.
 *
 * @param pMb
 *     The mailbox.
 *
 * @param pItem
 *     The item, itemSize bytes.
 *
 * @return true if an unread item was overwritten.
 *
 * @par Context
 *     Task only. From an ISR body use rtos_mboxPostFromIsr().
 *
 * @see rtos_mboxFetch()
 */
bool rtos_mboxPost(rtos_mbox_t *pMb, const void *pItem);

/**
 * @brief Puts an item in the mailbox from an ISR body.
 *
 * As rtos_mboxPost(); a switch to the woken task happens at the end of the ISR.
 *
 * @param pMb
 *     The mailbox.
 *
 * @param pItem
 *     The item, itemSize bytes.
 *
 * @return true if an unread item was overwritten.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
bool rtos_mboxPostFromIsr(rtos_mbox_t *pMb, const void *pItem);

/**
 * @brief Takes the item out of the mailbox, waiting up to @p timeout ticks for one.
 *
 * @param pMb
 *     The mailbox.
 *
 * @param pItem
 *     Receives the item, itemSize bytes.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return true if an item was taken (the mailbox is empty afterwards), false on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_mboxPost()
 */
bool rtos_mboxFetch(rtos_mbox_t *pMb, void *pItem, rtos_tick_t timeout);

/**
 * @brief Tells whether the mailbox holds an unread item.
 *
 * @param pMb
 *     The mailbox.
 *
 * @return true if an item is waiting to be fetched.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 */
bool rtos_mboxIsFull(const rtos_mbox_t *pMb);

/** @} */


/**
 * @defgroup rtos_stream Stream buffers
 * @ingroup rtos_api
 * @brief Bytes of any length from one writer to one reader (RTOS_CFG_STREAMS).
 *
 * The object for a UART driver's receive path, a serial protocol or a network frame
 * queue: an interrupt or a task writes as many bytes as it has, a task reads as many as
 * it wants, and nothing is cut into fixed items. A ring of bytes the application
 * provides, 16 bit counts (a Wi-Fi frame is 1500 bytes), a trigger level that wakes the
 * reader once that many bytes are in, so that a line or a frame arrives in one wake-up
 * rather than one per byte.
 *
 * One writer and one reader at a time; the writer may be an interrupt. The rule is what
 * makes the copies cheap: the writer owns the tail and the free part of the ring, the
 * reader the head and the filled part, so the bytes are copied with interrupts enabled
 * and only the counts are updated under the critical section. A second task blocking as
 * reader or as writer is checked (RTOS_PANIC_ARGUMENT); two writers that never block
 * (an interrupt and a task) cannot be told apart and are the application's mistake.
 *
 * @code
 *     static uint8_t _rxBuf[128];
 *     static rtos_stream_t _rx;
 *     rtos_streamInit(&_rx, _rxBuf, sizeof _rxBuf, 1);       // wake the reader per byte
 *
 *     RTOS_ISR(UART_RX_vect) { uint8_t c = UDR0; (void)rtos_streamWriteFromIsr(&_rx, &c, 1); }
 *
 *     n = rtos_streamRead(&_rx, line, sizeof line, RTOS_MS(100));  // what is there, 0 on timeout
 * @endcode
 *
 * Cost: an object of 12 bytes on the AVR plus the ring; the functions in their own file,
 * nothing in the kernel's data paths, and with the option off nothing at all.
 * @{
 */

/**
 * @brief Initializes a stream buffer over the application's storage.
 *
 * @param pS
 *     The stream.
 * @param storage
 *     @p size bytes.
 * @param size
 *     Capacity in bytes, 1 to 65535.
 * @param trigger
 *     Bytes that must be in the stream before a blocked reader is woken, 1 to @p size.
 *     1 wakes it on every write; a line length or a frame size lets the reader sleep
 *     through the arrival.
 *
 * @pre No task waits on the stream.
 * @warning Checked: a NULL @p storage, a @p size of 0, a @p trigger of 0 or above @p size.
 */
void rtos_streamInit(rtos_stream_t *pS, uint8_t *storage, uint16_t size, uint16_t trigger);

/**
 * @brief Writes up to @p n bytes, waiting for room for the rest.
 *
 * Copies what fits. If bytes remain and @p timeout is not RTOS_NO_WAIT, waits for the
 * reader to make room and continues, until everything is written or the timeout, counted
 * from the call, runs out. The reader is woken when the stream holds its trigger level.
 *
 * @return The number of bytes written, @p n unless the stream was full and the wait
 *         ended first.
 *
 * @warning Checked: @p timeout above RTOS_MAX_TIMEOUT; a second task blocking as writer.
 *
 * @par Context
 *     Task; the idle task may only poll (RTOS_NO_WAIT).
 */
uint16_t rtos_streamWrite(rtos_stream_t *pS, const void *p, uint16_t n, rtos_tick_t timeout);

/**
 * @brief Writes what fits of @p n bytes from an interrupt.
 *
 * @return The number of bytes written; less than @p n means the stream is full and the
 *         rest is dropped: the caller counts that if it matters.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick; a switch to the woken reader happens
 *     at the end of the ISR.
 */
uint16_t rtos_streamWriteFromIsr(rtos_stream_t *pS, const void *p, uint16_t n);

/**
 * @brief Reads up to @p n bytes, waiting for the trigger level if the stream holds less.
 *
 * If fewer than the trigger level are in the stream and @p timeout is not RTOS_NO_WAIT,
 * waits until a write brings it to the level or the timeout runs out; then copies what is
 * there, at most @p n bytes. So a read after a timeout may return less than the trigger,
 * including 0, and a read of a stream above the level never waits. A waiting writer is
 * woken if room was made.
 *
 * @return The number of bytes copied; 0 when nothing arrived in time.
 *
 * @warning Checked: @p timeout above RTOS_MAX_TIMEOUT; a second task blocking as reader.
 *
 * @par Context
 *     Task; the idle task may only poll.
 */
uint16_t rtos_streamRead(rtos_stream_t *pS, void *p, uint16_t n, rtos_tick_t timeout);

/**
 * @brief Reads what is there, up to @p n bytes, from an interrupt (a transmit interrupt
 *        draining a stream).
 *
 * @return The number of bytes copied, possibly 0.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
uint16_t rtos_streamReadFromIsr(rtos_stream_t *pS, void *p, uint16_t n);

/** @brief Bytes in the stream. Any context. */
uint16_t rtos_streamCount(const rtos_stream_t *pS);

/** @brief Bytes of room in the stream. Any context. */
uint16_t rtos_streamSpace(const rtos_stream_t *pS);

/**
 * @brief Drops every byte in the stream.
 *
 * The writer's side is left as it is, so a write in progress from an interrupt is not
 * disturbed; a writer waiting for room is woken. A waiting reader keeps waiting for the
 * next bytes. For the reader's side of the stream, between frames.
 *
 * @par Context
 *     Task.
 */
void rtos_streamReset(rtos_stream_t *pS);

/** @} */


/**
 * @defgroup rtos_notify Task notifications
 * @ingroup rtos_api
 * @brief One 16 bit value per task: the cheapest signal, no object needed
 *        (RTOS_CFG_NOTIFY).
 * @{
 */

/**
 * @brief Updates a task's notification value and wakes the task if it waits for it.
 *
 * The task is woken if it waits in rtos_notifyWait(), or in rtos_notifyTake() and the
 * value is now non-zero; it may run before this function returns.
 *
 * @param taskId
 *     Receiving task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @param value
 *     Operand of @p op; ignored by RTOS_NOTIFY_INC.
 *
 * @param op
 *     RTOS_NOTIFY_SET: replace the value. RTOS_NOTIFY_OR: OR @p value in (flags).
 *     RTOS_NOTIFY_INC: add one, saturating at 0xFFFF (counting).
 *
 * @par Context
 *     Task only. From an ISR body use rtos_notifyFromIsr().
 *
 * @see rtos_notifyWait(), rtos_notifyTake()
 */
void rtos_notify(uint8_t taskId, uint16_t value, uint8_t op);

/**
 * @brief Updates a task's notification value from an ISR body.
 *
 * As rtos_notify(); a switch to the woken task happens at the end of the ISR.
 *
 * @param taskId
 *     Receiving task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @param value
 *     Operand of @p op; ignored by RTOS_NOTIFY_INC.
 *
 * @param op
 *     RTOS_NOTIFY_SET, RTOS_NOTIFY_OR or RTOS_NOTIFY_INC.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void rtos_notifyFromIsr(uint8_t taskId, uint16_t value, uint8_t op);

/**
 * @brief Waits for a notification and takes the whole value.
 *
 * Returns as soon as any notification has arrived since the last successful wait, even
 * one that set the value to 0. The value is then reset to 0.
 *
 * @param pValue
 *     Receives the value. May be NULL.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return true if a notification was taken, false on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_notify(), rtos_notifyTake()
 */
bool rtos_notifyWait(uint16_t *pValue, rtos_tick_t timeout);

/**
 * @brief Waits until the notification value is non-zero, then decrements it by one.
 *
 * With RTOS_NOTIFY_INC on the sending side this is a counting semaphore without an
 * object.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return true if the value was decremented, false on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @note rtos_notifyWait() and rtos_notifyTake() share the one value; a task should use
 *       one style.
 *
 * @see rtos_notify(), rtos_notifyWait()
 */
bool rtos_notifyTake(rtos_tick_t timeout);

/** @} */


/**
 * @defgroup rtos_timer Software timers
 * @ingroup rtos_api
 * @brief One shot and drift free periodic callbacks (RTOS_CFG_SW_TIMERS).
 * @{
 */

/**
 * @brief Prepares an inactive timer with its callback and argument.
 *
 * @param pT
 *     The timer.
 *
 * @param callback
 *     Called at every expiry. Must not be NULL (checked). Where it runs depends on
 *     RTOS_CFG_TIMER_TASK_PRIO, see rtos_timerFunc_t.
 *
 * @param arg
 *     Passed to @p callback unchanged. May be NULL.
 *
 * @pre The timer is not active.
 *
 * @see rtos_timerStart()
 */
void rtos_timerInit(rtos_timer_t *pT, rtos_timerFunc_t callback, void *arg);

/**
 * @brief Arms the timer; restarts it if it is already active.
 *
 * The first expiry is on the @p delay-th tick from now. Since part of the current tick
 * period is already gone, that is after @p delay - 1 to @p delay tick periods. A delay of
 * 0 means the next tick, as 1 does, and never the current one, so a callback may restart
 * its own timer with 0. After that the timer expires every @p period ticks exactly,
 * counted from the previous expiry, so it does not drift.
 *
 * @param pT
 *     The timer, prepared with rtos_timerInit() (checked).
 *
 * @param delay
 *     Ticks to the first expiry, 0 to RTOS_MAX_TIMEOUT (checked).
 *
 * @param period
 *     Ticks between later expiries, 0 for a one shot timer; at most RTOS_MAX_TIMEOUT
 *     (checked).
 *
 * @par Context
 *     Task, ISR body or timer callback.
 *
 * @note The active timers are kept sorted by expiry: the time this call spends with
 *       interrupts disabled grows with the number of active timers.
 *
 * @see rtos_timerStop(), rtos_timerRemaining()
 */
void rtos_timerStart(rtos_timer_t *pT, rtos_tick_t delay, rtos_tick_t period);

/**
 * @brief Disarms the timer.
 *
 * Nothing happens if it is not active. With a timer task (RTOS_CFG_TIMER_TASK_PRIO above
 * 0), an expiry that is still waiting for the timer task is dropped as well; a callback
 * that is already running finishes.
 *
 * @param pT
 *     The timer.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 *
 * @see rtos_timerStart()
 */
void rtos_timerStop(rtos_timer_t *pT);

/**
 * @brief Tells whether the timer is armed.
 *
 * @param pT
 *     The timer.
 *
 * @return true from rtos_timerStart() until a one shot timer expires or rtos_timerStop()
 *         is called; a periodic timer stays active until it is stopped.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 */
bool rtos_timerIsActive(const rtos_timer_t *pT);

/**
 * @brief Returns the ticks until the timer's next expiry.
 *
 * @param pT
 *     The timer.
 *
 * @return Ticks to the next expiry; 0 if the timer is not active or is due.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 */
rtos_tick_t rtos_timerRemaining(const rtos_timer_t *pT);

/** @} */


/**
 * @defgroup rtos_pool Memory pools
 * @ingroup rtos_api
 * @brief Fixed size blocks, O(1) allocation and release.
 * @{
 */

/**
 * @brief Builds the free list of a pool over the given storage.
 *
 * Also required after RTOS_POOL_INIT(). Any alignment of the storage and any block size
 * from sizeof(void *) up work. A block is returned at the alignment of its offset in the
 * storage, so storage meant for aligned objects must be aligned itself and have a block
 * size that is a multiple of that alignment.
 *
 * @param pPool
 *     The pool.
 *
 * @param storage
 *     @p blockSize * @p numBlocks bytes. Must not be NULL and must stay valid for as long
 *     as the pool is used.
 *
 * @param blockSize
 *     Bytes per block, at least sizeof(void *).
 *
 * @param numBlocks
 *     Number of blocks, 1 to 255.
 *
 * @pre No block of the pool is in use and no task waits on it.
 *
 * @warning Checked: a NULL @p storage, a @p blockSize below sizeof(void *) or a
 *          @p numBlocks of 0 panic with RTOS_PANIC_ARGUMENT.
 */
void rtos_poolInit(rtos_pool_t *pPool, uint8_t *storage, uint16_t blockSize, uint8_t numBlocks);

/**
 * @brief Gets a block, waiting up to @p timeout ticks for one to be freed.
 *
 * A failed allocation is counted in the pool's failed field.
 *
 * @param pPool
 *     The pool.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return The block, or NULL on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_poolFree(), rtos_poolAllocFromIsr()
 */
void *rtos_poolAlloc(rtos_pool_t *pPool, rtos_tick_t timeout);

/**
 * @brief Gets a block from an ISR body; never blocks.
 *
 * @param pPool
 *     The pool.
 *
 * @return The block, or NULL if none is free.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void *rtos_poolAllocFromIsr(rtos_pool_t *pPool);

/**
 * @brief Returns a block to its pool.
 *
 * If a task waits for a block, it gets this one directly and may run before this
 * function returns.
 *
 * @param pPool
 *     The pool the block came from.
 *
 * @param pBlock
 *     The block, as returned by rtos_poolAlloc() or rtos_poolAllocFromIsr().
 *
 * @par Context
 *     Task only. From an ISR body use rtos_poolFreeFromIsr().
 *
 * @warning Checked: a pointer outside the pool or not at the start of a block panics with
 *          RTOS_PANIC_ARGUMENT. A block freed twice is caught only when the pool would
 *          hold more free blocks than it has; otherwise it corrupts the free list.
 *
 * @see rtos_poolAlloc()
 */
void rtos_poolFree(rtos_pool_t *pPool, void *pBlock);

/**
 * @brief Returns a block to its pool from an ISR body.
 *
 * As rtos_poolFree(); a switch to a woken task happens at the end of the ISR.
 *
 * @param pPool
 *     The pool the block came from.
 *
 * @param pBlock
 *     The block.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void rtos_poolFreeFromIsr(rtos_pool_t *pPool, void *pBlock);

/**
 * @brief Returns the number of free blocks.
 *
 * @param pPool
 *     The pool.
 *
 * @return Free blocks now; a snapshot that may change at once.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 */
uint8_t rtos_poolFreeCount(const rtos_pool_t *pPool);

/** @} */


/**
 * @defgroup rtos_evgroup Event groups
 * @ingroup rtos_api
 * @brief Flags shared by any number of tasks (RTOS_CFG_EVGROUPS).
 * @{
 */

/**
 * @brief Initializes an event group at run time.
 *
 * The alternative to RTOS_EVGROUP_INIT().
 *
 * @param pG
 *     The event group.
 *
 * @param initialBits
 *     Flags set at the start.
 *
 * @pre No task waits on the group.
 */
void rtos_evgroupInit(rtos_evgroup_t *pG, rtos_evt_t initialBits);

/**
 * @brief Sets bits and wakes every waiter whose condition is now met.
 *
 * Waiters are served highest priority first. A waiter that asked for RTOS_WAIT_CLEAR
 * takes its bits out of the group as it is woken, so a lower waiter no longer sees them.
 * A woken task may run before this function returns.
 *
 * @param pG
 *     The event group.
 *
 * @param bits
 *     The bits to set.
 *
 * @par Context
 *     Task only. From an ISR body use rtos_evgroupSetFromIsr().
 *
 * @note With waiters present the call checks every task slot with interrupts disabled:
 *       its time grows with RTOS_CFG_NUM_TASKS.
 *
 * @see rtos_evgroupWait(), rtos_evgroupClear()
 */
void rtos_evgroupSet(rtos_evgroup_t *pG, rtos_evt_t bits);

/**
 * @brief Sets bits from an ISR body.
 *
 * As rtos_evgroupSet(); a switch to a woken task happens at the end of the ISR.
 *
 * @param pG
 *     The event group.
 *
 * @param bits
 *     The bits to set.
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void rtos_evgroupSetFromIsr(rtos_evgroup_t *pG, rtos_evt_t bits);

/**
 * @brief Clears bits; nobody is woken.
 *
 * @param pG
 *     The event group.
 *
 * @param bits
 *     The bits to clear.
 *
 * @par Context
 *     Task only.
 */
void rtos_evgroupClear(rtos_evgroup_t *pG, rtos_evt_t bits);

/**
 * @brief Returns the bits currently set.
 *
 * @param pG
 *     The event group.
 *
 * @return A consistent snapshot of the flags.
 *
 * @par Context
 *     Task, ISR body or timer callback.
 */
rtos_evt_t rtos_evgroupGet(const rtos_evgroup_t *pG);

/**
 * @brief Waits until bits of @p mask are set in the group.
 *
 * @param pG
 *     The event group.
 *
 * @param mask
 *     The bits to wait for. Must not be 0 (asserted).
 *
 * @param options
 *     RTOS_WAIT_ANY or RTOS_WAIT_ALL, optionally ORed with RTOS_WAIT_CLEAR to take the
 *     satisfying bits out of the group atomically. Without RTOS_WAIT_CLEAR the bits stay
 *     set for other tasks.
 *
 * @param timeout
 *     Ticks to wait; RTOS_NO_WAIT polls, RTOS_FOREVER waits for good.
 *
 * @return The bits that satisfied the wait (with RTOS_WAIT_ALL: @p mask), 0 on timeout.
 *
 * @par Context
 *     Task only; the idle task may only poll.
 *
 * @see rtos_evgroupSet(), rtos_waitEvents()
 */
rtos_evt_t rtos_evgroupWait(rtos_evgroup_t *pG, rtos_evt_t mask, uint8_t options, rtos_tick_t timeout);

/** @} */


/**
 * @defgroup rtos_suspend Suspend and resume
 * @ingroup rtos_api
 * @brief Take a task out of scheduling and back (RTOS_CFG_SUSPEND).
 * @{
 */

/**
 * @brief Keeps a task from running until rtos_resume().
 *
 * A blocked task stays in its wait. A wake up that arrives meanwhile (an event, a token,
 * a timeout) is kept and takes effect at the resume. Suspending a suspended task changes
 * nothing; there is no nesting count. A task may suspend itself; it stops at once.
 *
 * @param taskId
 *     The task, 1 to RTOS_CFG_NUM_TASKS (checked); not the idle task.
 *
 * @par Context
 *     Task only.
 *
 * @warning A suspended task keeps the mutexes it holds.
 *
 * @see rtos_resume()
 */
void rtos_suspend(uint8_t taskId);

/**
 * @brief Lets a suspended task run again.
 *
 * The task becomes ready if it is not still blocked, and may run before this function
 * returns. Nothing happens if the task is not suspended.
 *
 * @param taskId
 *     The task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @par Context
 *     Task only. From an ISR body use rtos_resumeFromIsr().
 *
 * @see rtos_suspend()
 */
void rtos_resume(uint8_t taskId);

/**
 * @brief Lets a suspended task run again, from an ISR body.
 *
 * As rtos_resume(); the switch happens at the end of the ISR.
 *
 * @param taskId
 *     The task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @par Context
 *     RTOS_ISR body or timer callback in the tick.
 */
void rtos_resumeFromIsr(uint8_t taskId);

/** @} */


/**
 * @defgroup rtos_dyn Dynamic tasks
 * @ingroup rtos_api
 * @brief Tasks that end and slots that are used again while the system runs
 *        (RTOS_CFG_DYNAMIC_TASKS).
 *
 * The static model stays: one task per priority, the priority is the id, the application
 * owns every stack, and nothing is ever killed from outside. What the option adds is a
 * life cycle at run time, in three calls that report an error instead of panicking:
 *
 * - rtos_exit() ends the running task, from any depth of its call chain, exactly as a
 *   return from the task function does.
 * - rtos_reapTask() turns a terminated slot into an unused one. The stack belongs to the
 *   caller again from that moment.
 * - rtos_startTask() is rtos_createTask() for a running system: it fills an unused slot,
 *   and the new task runs at once if it is the highest ready task.
 *
 * The one who created a task is the one who reaps it, when rtos_getTaskInfo() reports
 * RTOS_TASK_TERMINATED. A task that should end on request is told with a notification or
 * an event and calls rtos_exit() itself; there is no kill, since a task stopped from
 * outside may hold a mutex, sit in a waiter list or be mid-way through a hand-over.
 *
 * @code
 *     RTOS_STACK(_workStack, 256);
 *
 *     static void work(void *arg) { ...; rtos_exit(); }
 *
 *     if(rtos_startTask(2, work, NULL, _workStack, sizeof(_workStack)) == RTOS_OK)
 *     {
 *         rtos_taskInfo_t info;
 *         do rtos_delay(RTOS_MS(10)); while(rtos_getTaskInfo(2, &info)  &&  info.state != RTOS_TASK_TERMINATED);
 *         rtos_reapTask(2);          // the slot and _workStack are free again
 *     }
 * @endcode
 *
 * Cost: one byte per task (the mutexes held, so that an exit with a mutex is caught
 * whatever its ceiling) and an increment per mutex lock and unlock. Nothing else changes
 * in a system that never calls the three functions; with the option off they are not
 * compiled and the kernel is the static one, byte for byte.
 * @{
 */

/**
 * @brief Ends the running task.
 *
 * The task is terminated as if its function had returned: off the ready mask for good,
 * RTOS_TASK_TERMINATED in rtos_getTaskInfo(), deaf to every wake-up; its stack is in use
 * until the switch away from it, which this call makes. A watchdog monitor on the task
 * is dropped. Never returns.
 *
 * @warning Checked: the idle task and a task holding a mutex (any ceiling) panic with
 *          RTOS_PANIC_ARGUMENT (arg1: the task, arg2: the mutexes held); so does a call
 *          from an interrupt on a port that can tell (the ESP32 and the host).
 *
 * @par Context
 *     Task only.
 */
void rtos_exit(void) __attribute__((noreturn));

/**
 * @brief Frees the slot of a terminated task.
 *
 * The slot becomes unused (RTOS_TASK_NONE), with nothing of the old task left in it, and
 * its stack memory belongs to the caller again. A watchdog monitor on the task is
 * dropped. The port releases what it allocated for the context (nothing on the AVR and
 * the ESP32; the host port frees the ucontext and its stack).
 *
 * @param taskId
 *     The task, 1 to RTOS_CFG_NUM_TASKS, not the caller.
 *
 * @return RTOS_OK; RTOS_ERR_ARG for the idle task, an id out of range or the caller
 *         itself; RTOS_ERR_STATE if the slot is unused or the task is not terminated;
 *         RTOS_ERR_CONTEXT from an interrupt on a port that can tell.
 *
 * @par Context
 *     Task or the idle task.
 */
rtos_err_t rtos_reapTask(uint8_t taskId);

/**
 * @brief Creates a task while the scheduler runs.
 *
 * rtos_createTask() with the same arguments and the same rules for the stack, done under
 * a critical section and followed by a scheduling decision: a new task above the caller
 * runs before this call returns. The slot must be unused: a terminated task is reaped
 * first. With RTOS_CFG_STACK_CHECK the stack is filled with the pattern inside the
 * critical section, so a large stack keeps interrupts masked for its length (about 20 us
 * for 4 KB on the ESP32).
 *
 * @return RTOS_OK; RTOS_ERR_BUSY if the slot is in use; RTOS_ERR_ARG for a priority out
 *         of range, a NULL function or stack, or a stack below RTOS_STACK_MIN after
 *         alignment; RTOS_ERR_CONTEXT from an interrupt on a port that can tell (the
 *         stack fill is unbounded work for an interrupt: notify a task and let it create).
 *
 * @par Context
 *     Task or the idle task.
 *
 * @see rtos_createTask() for the parameters.
 */
rtos_err_t rtos_startTask( uint8_t prio
                         , rtos_taskFunc_t taskFunc
                         , void *arg
                         , uint8_t *stackArea
                         , size_t stackSize
                         );

/**
 * @brief Name of an error code, for a message.
 *
 * @return "ok", "busy", "state", "argument" or "context"; "?" for anything else.
 */
const char *rtos_errName(rtos_err_t e);

/** @} */


/**
 * @defgroup rtos_heap Heap
 * @ingroup rtos_api
 * @brief First fit allocator with coalescing, for systems that need one (RTOS_CFG_HEAP).
 * @{
 */

/**
 * @brief Gives the allocator its memory.
 *
 * The start of the area is aligned up to 2 * sizeof(void *) and the size rounded down.
 *
 * @param area
 *     The memory to allocate from. Must not be NULL and must stay valid for as long as
 *     the heap is used.
 *
 * @param size
 *     Size of @p area in bytes; at least a few dozen (four minimum blocks, checked).
 *
 * @pre Called once, before the first rtos_malloc(). A second call forgets every block.
 *
 * @warning Checked: a NULL @p area or a too small @p size panic with
 *          RTOS_PANIC_ARGUMENT.
 *
 * @see rtos_malloc(), rtos_heapStats()
 */
void rtos_heapInit(void *area, size_t size);

/**
 * @brief Allocates @p size bytes.
 *
 * First fit over the free list. The block is aligned to 2 * sizeof(void *) and costs a
 * few bytes of header and boundary tag on top of @p size. A failed allocation is counted
 * in the statistics.
 *
 * @param size
 *     Bytes wanted.
 *
 * @return The block, or NULL if no free block is large enough.
 *
 * @par Context
 *     Task only.
 *
 * @note The search runs with interrupts disabled and its time grows with the number of
 *       free blocks. For a bounded worst case use a memory pool (rtos_poolAlloc()).
 *
 * @warning Checked: a call before rtos_heapInit() panics with RTOS_PANIC_ARGUMENT.
 *
 * @see rtos_free()
 */
void *rtos_malloc(size_t size);

/**
 * @brief Returns a block from rtos_malloc(); NULL is ignored.
 *
 * The block merges with free neighbours on both sides.
 *
 * @param p
 *     The block, exactly as rtos_malloc() returned it, or NULL.
 *
 * @par Context
 *     Task only.
 *
 * @warning Checked: a pointer outside the heap or a block that is already free panics
 *          with RTOS_PANIC_ARGUMENT. A pointer into the middle of a block is not detected
 *          and corrupts the heap.
 *
 * @see rtos_malloc()
 */
void rtos_free(void *p);

/**
 * @brief Reads the heap statistics.
 *
 * @param pStats
 *     Receives the total size, the free bytes, the low water mark, the largest free
 *     block and the call counts.
 *
 * @par Context
 *     Task only.
 *
 * @note Finding the largest free block walks the free list with interrupts disabled.
 */
void rtos_heapStats(rtos_heapStats_t *pStats);

/** @} */


/**
 * @defgroup rtos_diag Diagnostics
 * @ingroup rtos_api
 * @brief Current task, overruns, stack use, run time and load.
 * @{
 */

/**
 * @brief Returns the id (= priority) of the running task.
 *
 * @return The calling task's id; 0 in the idle task; in an ISR body the interrupted
 *         task.
 */
uint8_t rtos_getCurrentTask(void);

/**
 * @brief Returns the number of period overruns of a task.
 *
 * Counted by rtos_waitPeriodic(); saturates at 255.
 *
 * @param taskId
 *     The task, 0 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @param reset
 *     true sets the counter back to 0 after reading it.
 *
 * @return The overruns since the start or the last reset.
 *
 * @par Context
 *     Task only.
 */
uint8_t rtos_getOverrunCount(uint8_t taskId, bool reset);

/**
 * @brief Returns how many bytes at the bottom of a task's stack were never used.
 *
 * The high water mark since the task was created: the fill pattern written by
 * rtos_createTask() is counted from the bottom up.
 *
 * @param taskId
 *     The task, 0 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @return Untouched bytes; 0 for the idle task, for a task that was never created and
 *         without RTOS_CFG_STACK_CHECK.
 *
 * @par Context
 *     Task only.
 *
 * @note The scan reads the stack byte by byte with interrupts enabled; its time grows
 *       with the stack size.
 *
 * @see rtos_stackWarnHook(), RTOS_STACK_MIN
 */
size_t rtos_getStackFree(uint8_t taskId);

/**
 * @brief Returns the microseconds since start.
 *
 * Read from the port's free running time base (the tick timer's count on the AVR, 4 us
 * steps at 16 MHz; the cycle counter on the ESP32), independent of the tick width. It
 * wraps at 2^32 us, after 71.6 minutes: use differences.
 *
 * @return Microseconds since rtos_start().
 *
 * @par Context
 *     Task, ISR body or timer callback.
 *
 * @see rtos_getTick()
 */
uint32_t rtos_getTimeUs(void);

/**
 * @brief Returns the accumulated run time of a task.
 *
 * Measured at every context switch, in counts of RTOS_RUNTIME_HZ; the running task's
 * time includes the current slice. Needs RTOS_CFG_RUNTIME_STATS.
 *
 * @param taskId
 *     The task, 0 (idle) to RTOS_CFG_NUM_TASKS (checked).
 *
 * @return The run time; 0 without RTOS_CFG_RUNTIME_STATS. The width of the count is the
 *         port's and it wraps.
 *
 * @par Context
 *     Task only.
 *
 * @see RTOS_RUNTIME_TO_MS(), rtos_getCpuLoad()
 */
rtos_runtime_t rtos_getTaskRunTime(uint8_t taskId);

/**
 * @brief Returns the CPU load since the previous call.
 *
 * The share of time not spent in the idle task, over the interval since the previous
 * call (since rtos_start() for the first one). Needs RTOS_CFG_RUNTIME_STATS. With
 * RTOS_CFG_SMP it is the share over all cores: 100 means every core was busy throughout,
 * 50 that half the cores' time went to tasks; rtos_getCoreLoad() has one core.
 *
 * @return Load in percent, 0 to 100; 0 without RTOS_CFG_RUNTIME_STATS.
 *
 * @par Context
 *     Task only. Every call starts a new interval, so one task should own the readings.
 */
uint8_t rtos_getCpuLoad(void);

#if RTOS_CFG_SMP
/**
 * @brief Returns the load of one core since the previous call for that core (RTOS_CFG_SMP).
 *
 * The share of the interval that core did not spend in its idle task. Its own interval
 * per core, independent of rtos_getCpuLoad(). Needs RTOS_CFG_RUNTIME_STATS.
 *
 * @param core
 *     0 to RTOS_PORT_CORES - 1 (checked).
 *
 * @par Context
 *     Task only.
 */
uint8_t rtos_getCoreLoad(uint8_t core);
#endif

#if RTOS_CFG_SMP_AFFINITY
/**
 * @brief Restricts a task to some of the cores (RTOS_CFG_SMP_AFFINITY).
 *
 * Bit n of @p coreMask set: the task may run on core n. Every task may run everywhere
 * at start. A task that runs on a core the new mask excludes is moved: that core picks
 * again and the task continues on an allowed core when one is free for its rank. A
 * ready task that may now run on an idle core is started there. For a driver whose
 * interrupt lives on one core (the ESP32 radio on core 0), or a task that shares a cache
 * with its peripheral. Nothing balances load: the scheduler still runs the highest
 * ranked ready tasks, each on a core its mask allows.
 *
 * @param taskId
 *     1 to RTOS_CFG_NUM_TASKS (checked).
 * @param coreMask
 *     At least one bit, none above RTOS_PORT_CORES - 1 (checked).
 *
 * @par Context
 *     Task only.
 */
void rtos_setAffinity(uint8_t taskId, uint8_t coreMask);

/** The mask rtos_setAffinity() set, all cores before any call. */
uint8_t rtos_getAffinity(uint8_t taskId);
#endif

/**
 * @brief Takes a snapshot of a task's state for diagnostics.
 *
 * @param taskId
 *     The task, 0 (idle) to RTOS_CFG_NUM_TASKS.
 *
 * @param pInfo
 *     Receives the state, priorities, what the task waits on, its timeout, stack and run
 *     time figures.
 *
 * @return true on success; false for an id out of range or a NULL @p pInfo (not a
 *         panic).
 *
 * @par Context
 *     Task only.
 *
 * @see rtos_taskInfo_t
 */
bool rtos_getTaskInfo(uint8_t taskId, rtos_taskInfo_t *pInfo);

/** @} */


/**
 * @defgroup rtos_panic Panic and reset cause
 * @ingroup rtos_api
 * @brief Stop the system with a record that survives a reset.
 * @{
 */

/**
 * @brief Stops the system.
 *
 * Disables interrupts, records the reason in memory that survives a reset, calls
 * rtos_panicHook() and then halts (RTOS_CFG_PANIC_ACTION 0) or resets (1).
 *
 * @param reason
 *     A rtos_panicReason_t; applications use RTOS_PANIC_APPLICATION.
 *
 * @param arg1
 *     First argument, stored in the record.
 *
 * @param arg2
 *     Second argument, stored in the record.
 *
 * @par Context
 *     Any: task, ISR body, timer callback, hook.
 *
 * @see rtos_getLastPanic(), rtos_panicHook()
 */
void rtos_panic(uint8_t reason, uint32_t arg1, uint32_t arg2) __attribute__((noreturn));

/**
 * @brief Reads the panic record left before the last reset, if any.
 *
 * rtos_init() takes the record out of the memory that survives a reset, so each record
 * is reported in one boot only. A power cycle loses it.
 *
 * @param pInfo
 *     Receives the record. May be NULL to only ask whether there is one.
 *
 * @return true if the previous run ended in a panic.
 *
 * @pre rtos_init() has been called.
 *
 * @see rtos_panic(), rtos_getResetCause()
 */
bool rtos_getLastPanic(rtos_panicInfo_t *pInfo);

/**
 * @brief Returns the cause of the last reset as the hardware reports it.
 *
 * @return One of RTOS_RESET_UNKNOWN, RTOS_RESET_POWER_ON, RTOS_RESET_EXTERNAL,
 *         RTOS_RESET_BROWN_OUT, RTOS_RESET_WATCHDOG, RTOS_RESET_SOFTWARE,
 *         RTOS_RESET_DEEP_SLEEP (the wake-up from power_deepSleep, hal/power.h).
 */
uint8_t rtos_getResetCause(void);

/** @} */


/**
 * @defgroup rtos_hooks Hooks
 * @ingroup rtos_api
 * @brief Weak functions the application may override.
 * @{
 */

/**
 * @brief Called when an assertion (RTOS_ASSERT) fails.
 *
 * The weak default stores @p file and @p line in the panic record and calls rtos_panic()
 * with RTOS_PANIC_ASSERT.
 *
 * @param file
 *     Source file of the assertion; on the AVR a string in program memory.
 *
 * @param line
 *     Source line of the assertion.
 *
 * @warning An override must not return either: the kernel is in an undefined state, and
 *          the compiler relies on it.
 */
void rtos_assertFailed(const char *file, uint16_t line) __attribute__((noreturn, cold));

/**
 * @brief Called by rtos_panic() before the halt or reset.
 *
 * The weak default does nothing; an application prints or logs the record here.
 *
 * @param pInfo
 *     The panic record.
 *
 * @par Context
 *     Interrupts disabled; must not block or call blocking kernel functions.
 */
void rtos_panicHook(const rtos_panicInfo_t *pInfo);

/**
 * @brief Called in every iteration of the idle loop.
 *
 * Used when RTOS_CFG_IDLE_HOOK is not defined (a binary kernel cannot take a macro). The
 * weak default does nothing; an application overrides it to drive sensors, feed a
 * console or sleep deeper.
 *
 * @par Context
 *     The idle task; must not block.
 */
void rtos_idleHook(void);

/**
 * @brief Called once per task when its unused stack drops below RTOS_PORT_STACK_WARN.
 *
 * The stack monitor (RTOS_CFG_STACK_WARN with RTOS_CFG_STACK_CHECK) reads the high water
 * mark of one task per idle loop. The weak default does nothing; an application prints
 * or logs here.
 *
 * @param task
 *     The task whose margin is gone.
 *
 * @param stackFree
 *     Its untouched stack bytes, as rtos_getStackFree() reports them.
 *
 * @par Context
 *     The idle task; must not block.
 */
void rtos_stackWarnHook(uint8_t task, size_t stackFree);

/** @} */


/**
 * @defgroup rtos_wdt Watchdog
 * @ingroup rtos_api
 * @brief Hardware watchdog kicked from the idle task, plus per task check-ins
 *        (RTOS_CFG_WATCHDOG).
 * @{
 */

/**
 * @brief Starts the hardware watchdog.
 *
 * The idle task kicks it, after checking that every monitored task checked in on time.
 * A late task causes RTOS_PANIC_WATCHDOG with a record; a starved idle task (a runaway
 * task or an interrupt storm) lets the hardware reset the system without one, which the
 * next boot sees as RTOS_RESET_WATCHDOG.
 *
 * @param timeoutMs
 *     Hardware timeout in milliseconds, above 0 (asserted); rounded up to what the
 *     hardware offers.
 *
 * @note The idle task kicks only after rtos_start(): called earlier, the start up must
 *       finish within the timeout.
 *
 * @see rtos_wdtMonitor(), rtos_wdtCheckIn()
 */
void rtos_wdtInit(uint32_t timeoutMs);

/**
 * @brief Requires a task to call rtos_wdtCheckIn() at least every @p maxInterval ticks.
 *
 * The first deadline is @p maxInterval from now. The check runs in the idle task, so a
 * late task is reported when the idle task next runs.
 *
 * @param task
 *     The task, 1 to RTOS_CFG_NUM_TASKS (checked).
 *
 * @param maxInterval
 *     Longest allowed time between check-ins in ticks, at most RTOS_MAX_TIMEOUT
 *     (checked); 0 stops monitoring the task.
 *
 * @par Context
 *     Task.
 */
void rtos_wdtMonitor(uint8_t task, rtos_tick_t maxInterval);

/**
 * @brief Tells the watchdog that the calling task is alive: its deadline starts over.
 *
 * @par Context
 *     Task only.
 */
void rtos_wdtCheckIn(void);

/** @} */


/**
 * @defgroup rtos_trace Trace
 * @ingroup rtos_api
 * @brief Kernel events delivered to a hook (RTOS_CFG_TRACE).
 * @{
 */

/** @name Trace events, the first argument of rtos_traceHook()
 *  @{ */
#define RTOS_TRACE_SWITCH   1u  /**< a: task leaving, b: task entering. */
#define RTOS_TRACE_BLOCK    2u  /**< a: task, b: RTOS_WAIT_* kind. */
#define RTOS_TRACE_READY    3u  /**< a: task made ready, b: RTOS_WAIT_* it was in. */
#define RTOS_TRACE_TIMER    4u  /**< a: timer address, b: 0 in ISR context, 1 in task. */
#define RTOS_TRACE_PANIC    5u  /**< a: reason, b: arg1. */
/** @} */

/**
 * @brief Called from inside the kernel at every traced event.
 *
 * The weak default does nothing; hal/trace.h has a ring buffer recorder to override it
 * with.
 *
 * @param event
 *     One of the RTOS_TRACE_* events.
 *
 * @param a
 *     First argument, see the event.
 *
 * @param b
 *     Second argument, see the event.
 *
 * @par Context
 *     Interrupts disabled; must be short and must not call the kernel.
 */
void rtos_traceHook(uint8_t event, uint32_t a, uint32_t b);

/** @} */


/** @cond RTOS_INTERNAL */
/* --- Called from the port layer, not by applications -------------------------------------- */

void rtos_tickFromIsr(void);
void rtos_isrTail(void);
void rtos_taskTrampoline(void);
/** @endcond */

#endif  /* RTOS_INCLUDED */
