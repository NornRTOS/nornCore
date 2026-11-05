#ifndef RTOS_TYPES_INCLUDED
#define RTOS_TYPES_INCLUDED
/**
 * @file rtos_types.h
 * Kernel scalar types derived from the application configuration. Included by rtos.h and
 * by the port layer, so it must not depend on either.
 */

#include <stdint.h>
#include "rtos_config.h"

#ifndef RTOS_CFG_NUM_TASKS
# error RTOS_CFG_NUM_TASKS must be defined in rtos_config.h
#endif
#ifndef RTOS_CFG_TICK_HZ
# define RTOS_CFG_TICK_HZ 1000ul
#endif
#ifndef RTOS_CFG_TICK_BITS
# define RTOS_CFG_TICK_BITS 32
#endif
#ifndef RTOS_CFG_EVT_BITS
# define RTOS_CFG_EVT_BITS 16
#endif
#ifndef RTOS_CFG_STACK_CHECK
# define RTOS_CFG_STACK_CHECK 1
#endif
#ifndef RTOS_CFG_IDLE_SLEEP
# define RTOS_CFG_IDLE_SLEEP 1
#endif
#ifndef RTOS_CFG_ASSERT
# define RTOS_CFG_ASSERT 1
#endif
#ifndef RTOS_CFG_SW_TIMERS
# define RTOS_CFG_SW_TIMERS 1
#endif
#ifndef RTOS_CFG_RUNTIME_STATS
# define RTOS_CFG_RUNTIME_STATS 1
#endif
#ifndef RTOS_CFG_PANIC_ACTION
# define RTOS_CFG_PANIC_ACTION 0            /* 0 = halt, 1 = reset */
#endif
#ifndef RTOS_CFG_WATCHDOG
# define RTOS_CFG_WATCHDOG 0
#endif
#ifndef RTOS_CFG_TRACE
# define RTOS_CFG_TRACE 0
#endif
#ifndef RTOS_CFG_TIMER_TASK_PRIO
# define RTOS_CFG_TIMER_TASK_PRIO 0         /* 0 = timer callbacks run in the tick ISR */
#endif
#ifndef RTOS_CFG_TIMER_TASK_STACK
# define RTOS_CFG_TIMER_TASK_STACK (4u * RTOS_PORT_MIN_STACK)
#endif
#ifndef RTOS_CFG_HEAP
# define RTOS_CFG_HEAP 0
#endif
#ifndef RTOS_CFG_EVGROUPS
# define RTOS_CFG_EVGROUPS 1
#endif
#ifndef RTOS_CFG_SUSPEND
# define RTOS_CFG_SUSPEND 1
#endif
#ifndef RTOS_CFG_TICKLESS
# define RTOS_CFG_TICKLESS 0
#endif
#ifndef RTOS_CFG_NOTIFY
# define RTOS_CFG_NOTIFY 1                  /* task notifications (rtos_notify*) */
#endif
#ifndef RTOS_CFG_STACK_WARN
# define RTOS_CFG_STACK_WARN 1              /* idle task stack monitor (needs STACK_CHECK) */
#endif

/** Every option that changes the kernel's data layout or behaviour, folded into one
    number. rtos_init passes the application's value to the kernel, which compares it with
    its own: a kernel library built for another configuration profile is caught at start
    (RTOS_PANIC_CONFIG) instead of corrupting memory. */
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
    | ((uint32_t)((RTOS_CFG_TICK_HZ / 100ul) & 15ul) << 28) )

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
