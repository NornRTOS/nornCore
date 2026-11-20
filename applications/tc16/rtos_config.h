#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/**
 * @file rtos_config.h
 * Kernel configuration of test case tc16: tickless idle at 100 Hz (audit finding R3) with a
 * wake-up source that is not the tick.
 */
#define RTOS_CFG_NUM_TASKS      2
#define RTOS_CFG_TICK_HZ        100ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_STACK_CHECK    2
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#define RTOS_CFG_TICKLESS       1
#define RTOS_CFG_RUNTIME_STATS  1
#endif  /* RTOS_CONFIG_INCLUDED */
