#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/**
 * @file rtos_config.h
 * Kernel configuration of the blink application: the "small" set from the README with
 * everything an application of one task does not use switched off as well, so that the
 * image is the smallest a running nornOS can be.
 */

#define RTOS_CFG_NUM_TASKS      1
#define RTOS_CFG_TICK_HZ        1000ul
#if defined(__AVR__)
# define RTOS_CFG_TICK_BITS     16
# define RTOS_CFG_EVT_BITS      8
#else
# define RTOS_CFG_TICK_BITS     32
# define RTOS_CFG_EVT_BITS      16
#endif
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_STACK_WARN     0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         0
#define RTOS_CFG_RUNTIME_STATS  0
#define RTOS_CFG_SW_TIMERS      0
#define RTOS_CFG_NOTIFY         0
#define RTOS_CFG_EVGROUPS       0
#define RTOS_CFG_SUSPEND        0
#define RTOS_CFG_HEAP           0
#define RTOS_CFG_WATCHDOG       0
#define RTOS_CFG_TRACE          0
#define RTOS_CFG_TICKLESS       0

#endif  /* RTOS_CONFIG_INCLUDED */
