#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of test_smp: the SMP kernel on 3 host cores (nornInternal/nornCore/smp_plan.md). */
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_RUNTIME_STATS  2
#define RTOS_CFG_IDLE_HOOK      test_idleHook
#define RTOS_HOST_CORES         3
#define RTOS_CFG_SMP            1
void test_idleHook(void);
#endif  /* RTOS_CONFIG_INCLUDED */
