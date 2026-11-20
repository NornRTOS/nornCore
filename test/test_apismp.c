/**
 * @file test_apismp.c
 * test_api, the host API contract test, built for the SMP kernel on four host cores
 * (test_apismp/rtos_config.h: RTOS_HOST_CORES 4, RTOS_CFG_SMP 1). Every check of the one
 * core kernel must hold while the other three cores sit in their idle loops and every
 * critical section takes the kernel lock (nornInternal/nornCore/smp_plan.md, step 3).
 */
#include "test_api.c"
