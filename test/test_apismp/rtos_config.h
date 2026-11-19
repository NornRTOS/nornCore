#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* test_api on the SMP kernel (nornInternal/nornCore/smp_plan.md step 3): the same configuration as test_api
   plus four host cores and RTOS_CFG_SMP; tasks still run on core 0, the other cores idle. */
#define RTOS_HOST_CORES         4
#define RTOS_CFG_SMP            1
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_SW_TIMERS      1
#define RTOS_CFG_RUNTIME_STATS  2       /* run time and the switch count */
#define RTOS_CFG_HEAP           1
#define RTOS_CFG_EVGROUPS       1
#define RTOS_CFG_SUSPEND        1
#define RTOS_CFG_WATCHDOG       1
#define RTOS_CFG_IDLE_HOOK      test_idleHook
void test_idleHook(void);
#endif  /* RTOS_CONFIG_INCLUDED */
