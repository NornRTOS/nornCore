#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of the host test of the mandatory checks: assertions OFF, so that
   only RTOS_CHECK stands between a bad argument and memory corruption. */
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         0
#define RTOS_CFG_SW_TIMERS      1
#define RTOS_CFG_HEAP           1
#define RTOS_CFG_WATCHDOG       1
#define RTOS_CFG_IDLE_HOOK      test_idleHook
void test_idleHook(void);
#endif  /* RTOS_CONFIG_INCLUDED */
