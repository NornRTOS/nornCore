#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/* Kernel configuration of test_smpport: the host port with four cores (threads). The kernel
   itself is the one core kernel (RTOS_CFG_SMP 0): only the port's primitives are tested. */
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_EVT_BITS       16
#define RTOS_CFG_STACK_CHECK    0
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_IDLE_HOOK      test_idleHook
#define RTOS_HOST_CORES         4
#define RTOS_CFG_SMP            0       /* four cores would default to SMP; the kernel is not there yet */
void test_idleHook(void);
#endif  /* RTOS_CONFIG_INCLUDED */
