#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/**
 * @file rtos_config.h
 * Kernel configuration of the benchmark tc10: the defaults an ordinary application gets
 * (stack fill, run time statistics, software timers, event groups), so that the figures
 * are what an application sees. TC10_LEAN=1 from EXTRA_CFLAGS measures the kernel with
 * the optional bookkeeping switched off.
 */

#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_IDLE_SLEEP     1
#ifndef RTOS_CFG_ASSERT
# define RTOS_CFG_ASSERT        1
#endif

/* Each option can be overridden from EXTRA_CFLAGS to measure its cost alone. */
#if defined(TC10_LEAN) && TC10_LEAN
# define TC10_DEFAULT   0
#else
# define TC10_DEFAULT   1
#endif
#ifndef RTOS_CFG_STACK_CHECK
# define RTOS_CFG_STACK_CHECK    TC10_DEFAULT
#endif
#ifndef RTOS_CFG_RUNTIME_STATS
# define RTOS_CFG_RUNTIME_STATS  TC10_DEFAULT
#endif
#ifndef RTOS_CFG_SW_TIMERS
# define RTOS_CFG_SW_TIMERS      TC10_DEFAULT
#endif
#ifndef RTOS_CFG_EVGROUPS
# define RTOS_CFG_EVGROUPS       TC10_DEFAULT
#endif
#ifndef RTOS_CFG_SUSPEND
# define RTOS_CFG_SUSPEND        TC10_DEFAULT
#endif

#endif  /* RTOS_CONFIG_INCLUDED */
