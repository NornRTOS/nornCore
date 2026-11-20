#ifndef RTOS_TYPES_INCLUDED
#define RTOS_TYPES_INCLUDED
/**
 * @file rtos_types.h
 * Kernel scalar types derived from the application configuration. Included by rtos.h and
 * by the port layer, so it must not depend on either.
 */

#include <stdint.h>
#include "rtos_config.h"

/* The configuration and its defaults. The application's rtos_config.h must define
   RTOS_CFG_NUM_TASKS; every other option has the default below, so a one line
   rtos_config.h is a complete configuration, and the comment on each line says what
   the option costs, so that leaving it at the default is a choice. The same list, for
   copying, is config/rtos_config.template.h; the README ("Three configurations") has
   the measured small/default/large sets. */
#ifndef RTOS_CFG_NUM_TASKS
# error RTOS_CFG_NUM_TASKS must be defined in rtos_config.h: the tasks, 1..31, the idle task not counted
#endif
#ifndef RTOS_CFG_TICK_HZ
# define RTOS_CFG_TICK_HZ 1000ul            /* ticks per second: the unit of every timeout is 1 ms */
#endif
#ifndef RTOS_CFG_TICK_BITS
# define RTOS_CFG_TICK_BITS 32              /* 16 saves a few bytes per wait on the AVR; the tick then wraps in 65 s */
#endif
#ifndef RTOS_CFG_EVT_BITS
# define RTOS_CFG_EVT_BITS 16               /* per-task event flags: 8, 16 or 32 (nornWIFI needs 32) */
#endif
#ifndef RTOS_CFG_STACK_CHECK
# define RTOS_CFG_STACK_CHECK 1             /* 1: stacks filled with a pattern, rtos_getStackFree works; 2: plus a canary checked at every switch and tick; 0: neither */
#endif
#ifndef RTOS_CFG_STACK_GUARD
# define RTOS_CFG_STACK_GUARD 0             /* 1: a hardware watchpoint on the bottom of the running task's stack, set at every switch (ESP32: RTOS_PANIC_STACK_OVERFLOW at the first write, also inside a critical section); needs RTOS_CFG_STACK_CHECK; ports without one ignore it */
#endif
#ifndef RTOS_CFG_IDLE_SLEEP
# define RTOS_CFG_IDLE_SLEEP 1              /* the idle task sleeps until the next interrupt */
#endif
#ifndef RTOS_CFG_ASSERT
# define RTOS_CFG_ASSERT 1                  /* kernel assertions: ~20 bytes each, a compare on the way in */
#endif
#ifndef RTOS_CFG_SW_TIMERS
# define RTOS_CFG_SW_TIMERS 1               /* rtos_timer*: one list check per tick */
#endif
#ifndef RTOS_CFG_RUNTIME_STATS
# define RTOS_CFG_RUNTIME_STATS 1           /* run time per task (rtos_getTaskRunTime, rtos_getCpuLoad, nosh ps): a clock read per switch, about 60 cycles, 4 bytes per task; 0 is the leanest switch; 2 also counts the switches into each task (rtos_taskInfo_t.switches, nosh sched): one 16 bit increment per switch, 2 bytes per task, and the locks that found a mutex held (rtos_mutexContended): 2 bytes per mutex, nothing on the uncontended path */
#endif
#ifndef RTOS_CFG_PANIC_ACTION
# define RTOS_CFG_PANIC_ACTION 0            /* 0 = halt (development), 1 = reset, the reason survives (rtos_getLastPanic) */
#endif
#ifndef RTOS_CFG_WATCHDOG
# define RTOS_CFG_WATCHDOG 0                /* hardware watchdog fed by the idle task plus per task check-ins */
#endif
#ifndef RTOS_CFG_TRACE
# define RTOS_CFG_TRACE 0                   /* rtos_traceHook at every switch, block and wake-up: a call per event */
#endif
#ifndef RTOS_CFG_TIMER_TASK_PRIO
# define RTOS_CFG_TIMER_TASK_PRIO 0         /* 0 = timer callbacks run in the tick ISR (lowest latency, interrupts off meanwhile: keep them to a few lines and the *FromIsr calls); a task id instead gives the callbacks a kernel task at that priority, where they may block and use the whole API, at the cost of a slot and a switch per expiry */
#endif
#ifndef RTOS_CFG_TIMER_TASK_STACK
# define RTOS_CFG_TIMER_TASK_STACK (4u * RTOS_PORT_MIN_STACK)
#endif
#ifndef RTOS_CFG_HEAP
# define RTOS_CFG_HEAP 0                    /* rtos_heapInit/rtos_malloc/rtos_free (nornWIFI needs it) */
#endif
#ifndef RTOS_CFG_EVGROUPS
# define RTOS_CFG_EVGROUPS 1                /* event groups (nornWIFI needs them) */
#endif
#ifndef RTOS_CFG_SUSPEND
# define RTOS_CFG_SUSPEND 1                 /* rtos_suspend/rtos_resume */
#endif
#ifndef RTOS_CFG_TICKLESS
# define RTOS_CFG_TICKLESS 0                /* ESP32: the idle task sleeps until the next deadline, not every tick */
#endif
#ifndef RTOS_CFG_NOTIFY
# define RTOS_CFG_NOTIFY 1                  /* task notifications (rtos_notify*): 2 bytes per task */
#endif
#ifndef RTOS_CFG_STACK_WARN
# define RTOS_CFG_STACK_WARN 1              /* idle task stack monitor, rtos_stackWarnHook (needs STACK_CHECK): one task's high water mark per idle loop */
#endif
#ifndef RTOS_CFG_HAL_HOOKS
# define RTOS_CFG_HAL_HOOKS 0               /* 1: hal/hooks.c prints panics, failed assertions and stack warnings on the console UART (log.h), so the application needs no hooks of its own */
#endif
#ifndef RTOS_CFG_STREAMS
# define RTOS_CFG_STREAMS 0                 /* rtos_stream_t: a byte stream with variable length writes and reads, one writer (task or ISR) and one reader; own file, nothing elsewhere */
#endif
#ifndef RTOS_CFG_DYNAMIC_TASKS
# define RTOS_CFG_DYNAMIC_TASKS 0           /* rtos_exit, rtos_reapTask, rtos_startTask: tasks that end and slots reused at run time; 1 byte per task and an increment per mutex lock and unlock, nothing else on the hot path */
#endif

/* The tick rate as a two bit code for the signature: 100, 200, 500 or 1000 Hz. Another
   rate needs a code here first. */
#if RTOS_CFG_TICK_HZ == 100ul
# define RTOS_CFG_TICK_HZ_CODE 0ul
#elif RTOS_CFG_TICK_HZ == 200ul
# define RTOS_CFG_TICK_HZ_CODE 1ul
#elif RTOS_CFG_TICK_HZ == 500ul
# define RTOS_CFG_TICK_HZ_CODE 2ul
#elif RTOS_CFG_TICK_HZ == 1000ul
# define RTOS_CFG_TICK_HZ_CODE 3ul
#else
# error RTOS_CFG_TICK_HZ must be 100, 200, 500 or 1000
#endif

/** Every option that changes the kernel's data layout or behaviour, folded into one
    number. rtos_init passes the application's value to the kernel, which compares it with
    its own: a kernel built from another configuration (stale objects, a library) is caught
    at start (RTOS_PANIC_CONFIG) instead of corrupting memory. */
#define RTOS_CONFIG_SIGNATURE                                                       \
    ( (uint32_t)(RTOS_CFG_NUM_TASKS & 31u)                                          \
    | ((uint32_t)(RTOS_CFG_TICK_BITS / 16u) << 5)                                   \
    | ((uint32_t)(RTOS_CFG_EVT_BITS / 8u) << 7)                                     \
    | ((uint32_t)(RTOS_CFG_STACK_CHECK & 3u) << 10)                                 \
    | ((uint32_t)(RTOS_CFG_RUNTIME_STATS != 0) << 12)                               \
    | ((uint32_t)(RTOS_CFG_SW_TIMERS != 0) << 13)                                   \
    | ((uint32_t)(RTOS_CFG_NOTIFY != 0) << 14)                                      \
    | ((uint32_t)(RTOS_CFG_EVGROUPS != 0) << 15)                                    \
    | ((uint32_t)(RTOS_CFG_SUSPEND != 0) << 16)                                     \
    | ((uint32_t)(RTOS_CFG_HEAP != 0) << 17)                                        \
    | ((uint32_t)(RTOS_CFG_WATCHDOG != 0) << 18)                                    \
    | ((uint32_t)(RTOS_CFG_TIMER_TASK_PRIO & 31u) << 19)                            \
    | ((uint32_t)(RTOS_CFG_TRACE != 0) << 24)                                       \
    | ((uint32_t)(RTOS_CFG_STACK_WARN != 0) << 25)                                  \
    | ((uint32_t)(RTOS_CFG_TICKLESS != 0) << 26)                                    \
    | ((uint32_t)(RTOS_CFG_PANIC_ACTION != 0) << 27)                                \
    | ((uint32_t)(RTOS_CFG_RUNTIME_STATS >= 2) << 31)                               \
    | ((uint32_t)RTOS_CFG_TICK_HZ_CODE << 28)                                      \
    | ((uint32_t)(RTOS_CFG_DYNAMIC_TASKS != 0) << 30) )

/** Bitmask with one bit per task. Sized to the smallest integer that fits. */
#if RTOS_CFG_NUM_TASKS + 1 <= 8
typedef uint8_t rtos_taskmask_t;
#elif RTOS_CFG_NUM_TASKS + 1 <= 16
typedef uint16_t rtos_taskmask_t;
#elif RTOS_CFG_NUM_TASKS + 1 <= 32
typedef uint32_t rtos_taskmask_t;
#else
# error At most 31 application tasks are supported
#endif

/** The system time in ticks. Wraps around; the kernel only ever compares differences. */
#if RTOS_CFG_TICK_BITS == 16
typedef uint16_t rtos_tick_t;
typedef int16_t  rtos_tickdiff_t;
#elif RTOS_CFG_TICK_BITS == 32
typedef uint32_t rtos_tick_t;
typedef int32_t  rtos_tickdiff_t;
#else
# error RTOS_CFG_TICK_BITS must be 16 or 32
#endif

/** Event flag vector, one bit per event, private to each task. */
#if RTOS_CFG_EVT_BITS == 8
typedef uint8_t rtos_evt_t;
#elif RTOS_CFG_EVT_BITS == 16
typedef uint16_t rtos_evt_t;
#elif RTOS_CFG_EVT_BITS == 32
typedef uint32_t rtos_evt_t;
#else
# error RTOS_CFG_EVT_BITS must be 8, 16 or 32
#endif

#endif  /* RTOS_TYPES_INCLUDED */
