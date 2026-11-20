#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of test_log: the host tests' usual one plus the log line lock. */
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_IDLE_HOOK      test_idleHook
#define LOG_CFG_LOCK            1       /* whole lines, one logger at a time (hal/log.h) */
void test_idleHook(void);
#endif  /* RTOS_CONFIG_INCLUDED */
