#ifndef RTOS_CONFIG_INCLUDED
#define RTOS_CONFIG_INCLUDED
/**
 * @file rtos_config.h
 * Kernel configuration of test case tc18: the hardware stack guard with the canary under
 * it; a panic halts, so the report comes from the panic hook.
 */
#define RTOS_CFG_NUM_TASKS      2
#define RTOS_CFG_TICK_HZ        1000ul
#define RTOS_CFG_TICK_BITS      32
#define RTOS_CFG_STACK_CHECK    2
#define RTOS_CFG_STACK_GUARD    1
#define RTOS_CFG_IDLE_SLEEP     1
#define RTOS_CFG_ASSERT         1
#endif  /* RTOS_CONFIG_INCLUDED */
