/**
 * @file hooks.c
 * The kernel's hooks with a console: a panic, a failed assertion and a stack warning are
 * printed on UART0 through log.h, so that an application sees what stopped it without
 * writing a hook of its own. In the image only with RTOS_CFG_HAL_HOOKS 1 in
 * rtos_config.h; otherwise this file is empty and the kernel's silent weak defaults
 * stay, for the application that defines its own hooks. Nothing here runs before the
 * system stops or a stack runs low, so the option costs no time in operation.
 *
 * The lines (the prefix is log.h's class, time in ms):
 *
 *     P (1203) PANIC assert task 2 arg1 57 arg2 0 at app.c line 57
 *     P (0) PANIC config task 0 arg1 2721 arg2 2737 (built against another rtos_config.h than the kernel)
 *     W (88) stack: task 3 has 40 bytes left
 */

#include "rtos.h"

typedef int hal_hooks_unused_t;         /* ISO C wants a declaration in every file */

#if RTOS_CFG_HAL_HOOKS

#include "uart.h"
#include "log.h"

static void putReason(uint8_t reason)
{
    switch(reason)
    {
    case RTOS_PANIC_ASSERT:         uart_putsP(HAL_FSTR("assert")); break;
    case RTOS_PANIC_STACK_OVERFLOW: uart_putsP(HAL_FSTR("stack overflow")); break;
    case RTOS_PANIC_WATCHDOG:       uart_putsP(HAL_FSTR("watchdog")); break;
    case RTOS_PANIC_DEADLOCK:       uart_putsP(HAL_FSTR("deadlock")); break;
    case RTOS_PANIC_FAULT:          uart_putsP(HAL_FSTR("fault")); break;
    case RTOS_PANIC_CONFIG:         uart_putsP(HAL_FSTR("config")); break;
    case RTOS_PANIC_ARGUMENT:       uart_putsP(HAL_FSTR("argument")); break;
    case RTOS_PANIC_APPLICATION:    uart_putsP(HAL_FSTR("application")); break;
    default:                        uart_putU32(reason); break;
    }
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    LOG_P(uart_putsP(HAL_FSTR("PANIC ")); putReason(pInfo->reason);
          uart_putsP(HAL_FSTR(" task ")); uart_putU32(pInfo->task);
          uart_putsP(HAL_FSTR(" arg1 ")); uart_putU32(pInfo->arg1);
          uart_putsP(HAL_FSTR(" arg2 ")); uart_putU32(pInfo->arg2);
          if(pInfo->reason == RTOS_PANIC_ASSERT  &&  pInfo->pFile != NULL)
          {
              uart_putsP(HAL_FSTR(" at ")); uart_putsP(pInfo->pFile);
              uart_putsP(HAL_FSTR(" line ")); uart_putU32(pInfo->line);
          }
          if(pInfo->reason == RTOS_PANIC_CONFIG)
              uart_putsP(HAL_FSTR(" (built against another rtos_config.h than the kernel)")));
    uart_flush();
}

/** Called at a switch or a tick, so without the line lock (LOG_CFG_LOCK). */
void rtos_stackWarnHook(uint8_t task, size_t stackFree)
{
    LOG_AT_ISR(LOG_WARNING,
          uart_putsP(HAL_FSTR("stack: task ")); uart_putU32(task);
          uart_putsP(HAL_FSTR(" has ")); uart_putU32((uint32_t)stackFree);
          uart_putsP(HAL_FSTR(" bytes left")));
}

#endif  /* RTOS_CFG_HAL_HOOKS */
