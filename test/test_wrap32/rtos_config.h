#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of the host tick wrap test with 32 bit ticks: the start is placed 50 ticks before 2^32,
   so the run crosses the wrap once. */
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_SW_TIMERS      1
#define RTOS_CFG_RUNTIME_STATS  1
#define RTOS_CFG_WATCHDOG       1
#define RTOS_CFG_IDLE_HOOK      test_idleHook
void test_idleHook(void);
#endif  /* RTOS_CONFIG_INCLUDED */
