#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/**
 * @file rtos_config.h
 * Kernel configuration of test case tc13: the dynamic task API with the watchdog and
 * the stack monitor on.
 */

#define RTOS_CFG_NUM_TASKS      3
#define RTOS_CFG_STACK_CHECK    2
#define RTOS_CFG_DYNAMIC_TASKS  1
#define RTOS_CFG_WATCHDOG       1

#endif  /* RTOS_CONFIG_INCLUDED */
