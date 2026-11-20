#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/**
 * @file rtos_config.h
 * Kernel configuration of test case tc17: the SMP kernel with affinity on the ESP32's two
 * cores, run time statistics with the switch count, the stack canary.
 */
#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_STACK_CHECK    2
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_RUNTIME_STATS  2
#define RTOS_CFG_SMP            1
#define RTOS_CFG_SMP_AFFINITY   1
#endif  /* RTOS_CONFIG_INCLUDED */
