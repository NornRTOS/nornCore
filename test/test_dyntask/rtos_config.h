#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of test_dyntask: the dynamic task API with the watchdog, so that
   an exit and a reap are seen to drop the monitor. */

#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_DYNAMIC_TASKS  1
#define RTOS_CFG_WATCHDOG       1
#define RTOS_CFG_IDLE_HOOK      test_idleHook

void test_idleHook(void);

#endif  /* RTOS_CONFIG_INCLUDED */
