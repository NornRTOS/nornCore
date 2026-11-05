/**
 * @file sdk01_lib.c
 * An application built against the kernel library of a configuration profile, as a user
 * of the binary SDK builds: no rtos_config.h of its own (app.mk: PROFILE, USE_LIB), the
 * hooks overridden as functions, the stack sizes from the profile's constants. Two tasks
 * and a software timer; checks that the library's configuration is the one it was
 * compiled against (rtos_init would otherwise panic), that the idle hook and the stack
 * monitor hook are the ones defined here, and that the primitives work.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_WORK   2
#define TASK_PACE   1

#ifndef SDK01_STOP_AFTER_MS
# define SDK01_STOP_AFTER_MS  2000ul
#endif

static uint8_t _stackWork[RTOS_STACK_DEFAULT];
static uint8_t _stackPace[RTOS_STACK_DEFAULT];
static rtos_sem_t _sem = RTOS_SEM_INIT(0);
static rtos_timer_t _timer;
static volatile uint32_t _idleLoops, _timerFires, _paces, _works, _notified;

void rtos_idleHook(void)                /* overrides the library's weak default */
{
    ++ _idleLoops;
}

void rtos_stackWarnHook(uint8_t task, size_t stackFree)
{
    uart_puts("sdk01: stack warning task "); uart_putU32(task);
    uart_puts(" free "); uart_putU32((uint32_t)stackFree); uart_puts("\r\n");
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    uart_puts("sdk01: PANIC reason "); uart_putU32(pInfo->reason);
    uart_puts(" arg1 "); uart_putU32(pInfo->arg1);
    uart_puts(" arg2 "); uart_putU32(pInfo->arg2); uart_puts("\r\n");
    uart_puts("RESULT sdk01 FAIL\r\n");
    uart_flush();
}

static void timerCb(void *arg)
{
    (void)arg;
    ++ _timerFires;
    rtos_notifyFromIsr(TASK_WORK, 1, RTOS_NOTIFY_INC);
}

static void taskWork(void *arg)
{
    (void)arg;
    for(;;)
    {
        if(rtos_notifyTake(RTOS_MS(50)))
            ++ _notified;
        if(rtos_semTake(&_sem, RTOS_NO_WAIT))
            ++ _works;
    }
}

static void taskPace(void *arg)
{
    (void)arg;
    rtos_timerInit(&_timer, timerCb, NULL);
    rtos_timerStart(&_timer, RTOS_MS(10), RTOS_MS(10));
    for(;;)
    {
        (void)rtos_waitPeriodic(RTOS_MS(20));
        ++ _paces;
        rtos_semGive(&_sem);
        if(rtos_getTick() >= RTOS_MS(SDK01_STOP_AFTER_MS))
        {
            uart_puts("sdk01 " BOARD_NAME ": signature 0x");
            {
                const uint32_t s = RTOS_CONFIG_SIGNATURE;
                uint8_t i;
                for(i=0; i<4; ++i)
                    uart_putHex8((uint8_t)(s >> (24 - 8 * i)));
            }
            uart_puts(" tasks "); uart_putU32(RTOS_CFG_NUM_TASKS);
            uart_puts(" idle "); uart_putU32(_idleLoops);
            uart_puts(" timer "); uart_putU32(_timerFires);
            uart_puts(" paces "); uart_putU32(_paces);
            uart_puts(" works "); uart_putU32(_works);
            uart_puts(" notified "); uart_putU32(_notified);
            uart_puts("\r\n");
            tc_expectRange("idleLoops", _idleLoops, 1, 0xFFFFFFFFul);
            tc_expectNear("timerFires", _timerFires, SDK01_STOP_AFTER_MS / 10ul);
            tc_expectNear("paces", _paces, SDK01_STOP_AFTER_MS / 20ul);
            tc_expectNear("works", _works, SDK01_STOP_AFTER_MS / 20ul);
            tc_expectNear("notified", _notified, SDK01_STOP_AFTER_MS / 10ul);
            tc_finish("sdk01");
        }
    }
}

void rtos_assertFailed(const char *file, uint16_t line)
{
    (void)rtos_port_enterCritical();
    uart_puts("\r\nASSERTION FAILED: ");
    uart_putsP(file);
    uart_puts(", line ");
    uart_putU32(line);
    uart_puts("\r\n");
    rtos_port_halt();
}

int main(void)
{
    uart_init(115200);
    uart_puts("\r\nnornOS " RTOS_VERSION " sdk01_lib\r\n");
    rtos_init();
    rtos_createTask(TASK_WORK, taskWork, NULL, _stackWork, sizeof(_stackWork));
    rtos_createTask(TASK_PACE, taskPace, NULL, _stackPace, sizeof(_stackPace));
    rtos_start();
    return 0;
}
