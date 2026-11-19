/**
 * @file tc13_dyntask.c
 * Test case 13: the dynamic task API (RTOS_CFG_DYNAMIC_TASKS) under the watchdog and the
 * stack monitor, on a board or in the simulator. A supervisor starts a worker in slot 2
 * over and over: the worker takes a mutex, blocks a few ticks while holding it, releases
 * it and ends, with rtos_exit on even cycles and by returning on odd ones (after dropping
 * its own watchdog monitor, which a return does not do). The supervisor waits for
 * RTOS_TASK_TERMINATED, checks the worker's stack use, reaps the slot, checks it is unused
 * and starts the next cycle. A contender locks the same mutex all the time, so the
 * hand-over path of the held count runs in both directions. The supervisor is monitored
 * by the watchdog as well; the stack monitor is on and must stay quiet.
 *
 *   3  supervisor: the cycle; reports and finishes after TC13_STOP_AFTER_MS
 *   2  worker: created and reaped every cycle
 *   1  contender: locks the mutex every 7 ms and holds it for 1 ms
 *
 * Stops after TC13_STOP_AFTER_MS (3 s; -DTC13_STOP_AFTER_MS=3600000ul is the soak) and
 * expects at least one cycle per 20 ms, no error code, no stack warning and no panic.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_SUPER      3
#define TASK_WORKER     2
#define TASK_CONTENDER  1

#ifndef TC13_STOP_AFTER_MS
# define TC13_STOP_AFTER_MS  3000ul
#endif

#if RTOS_PORT_MIN_STACK > 192u
# define STACK_SIZE     (2u * RTOS_PORT_MIN_STACK)
#else
# define STACK_SIZE     192u
#endif

RTOS_STACK(_stackSuper, STACK_SIZE);
RTOS_STACK(_stackContender, STACK_SIZE);
RTOS_STACK(_stackWorker, STACK_SIZE);      /* owned by the supervisor, lent to each worker */

static rtos_mutex_t _m = RTOS_MUTEX_INIT(TASK_WORKER);   /* the two lockers are 1 and 2 */
static volatile uint32_t _cycles, _workerRuns, _contenderLocks, _errors, _stackWarnings;
static volatile size_t _workerStackFreeMin = (size_t)-1;


static void error(const char *what)
{
    ++ _errors;
    uart_puts("tc13 error: ");
    uart_puts(what);
    uart_puts("\r\n");
}


void rtos_stackWarnHook(uint8_t task, size_t stackFree)
{
    ++ _stackWarnings;
    uart_puts("tc13 stack warning task "); uart_putU32(task);
    uart_puts(" free "); uart_putU32((uint32_t)stackFree); uart_puts("\r\n");
}


/* ---- the worker: one life per cycle ---------------------------------------------------- */

static void worker(void *arg)
{
    const uint32_t cycle = (uint32_t)(uintptr_t)arg;

    ++ _workerRuns;
    rtos_mutexLock(&_m);
    rtos_delay(1u + (rtos_tick_t)(cycle % 3u));     /* blocked while holding: the contender waits */
    rtos_mutexUnlock(&_m);
    if(cycle & 1u)
    {
        rtos_wdtMonitor(TASK_WORKER, 0);            /* a return keeps the monitor; drop it ourselves */
        return;
    }
    rtos_exit();                                    /* drops the monitor itself */
}


/* ---- the contender --------------------------------------------------------------------- */

static void contender(void *arg)
{
    (void)arg;
    for(;;)
    {
        rtos_delay(RTOS_MS(7));
        rtos_mutexLock(&_m);
        ++ _contenderLocks;
        rtos_delay(RTOS_MS(1));
        rtos_mutexUnlock(&_m);
    }
}


/* ---- the supervisor -------------------------------------------------------------------- */

static void supervisor(void *arg)
{
    const rtos_tick_t start = rtos_getTick();
    (void)arg;

    rtos_wdtInit(1000);
    rtos_wdtMonitor(TASK_SUPER, RTOS_MS(500));

    for(;;)
    {
        rtos_taskInfo_t info;
        rtos_err_t e;

        e = rtos_startTask(TASK_WORKER, worker, (void *)(uintptr_t)_cycles, _stackWorker, sizeof(_stackWorker));
        if(e != RTOS_OK)
        {
            uart_puts("tc13 startTask: "); uart_puts(rtos_errName(e)); uart_puts("\r\n");
            error("startTask failed");
        }
        rtos_wdtMonitor(TASK_WORKER, RTOS_MS(200));
        if(rtos_startTask(TASK_WORKER, worker, NULL, _stackWorker, sizeof(_stackWorker)) != RTOS_ERR_BUSY)
            error("a second start into the live slot was not refused");

        /* Wait for the end, then reap. */
        do
        {
            rtos_delay(1);
            if(!rtos_getTaskInfo(TASK_WORKER, &info))
                error("getTaskInfo");
        }
        while(info.state != RTOS_TASK_TERMINATED);

        {
            const size_t free = rtos_getStackFree(TASK_WORKER);
            if(free < _workerStackFreeMin)
                _workerStackFreeMin = free;
        }
        if(rtos_reapTask(TASK_WORKER) != RTOS_OK)
            error("reap failed");
        if(!rtos_getTaskInfo(TASK_WORKER, &info)  ||  info.state != RTOS_TASK_NONE)
            error("the reaped slot is not unused");
        if(rtos_reapTask(TASK_WORKER) != RTOS_ERR_STATE)
            error("a second reap was not refused");

        ++ _cycles;
        rtos_wdtCheckIn();

        if((_cycles & 63u) == 0)
        {
            uart_puts("tc13 cycles "); uart_putU32(_cycles);
            uart_puts(" contender locks "); uart_putU32(_contenderLocks);
            uart_puts(" worker stack free min "); uart_putU32((uint32_t)_workerStackFreeMin);
            uart_puts(" errors "); uart_putU32(_errors); uart_puts("\r\n");
        }

        if((uint32_t)(rtos_getTick() - start) >= RTOS_MS(TC13_STOP_AFTER_MS))
        {
            const uint32_t ms = TC13_STOP_AFTER_MS;
            uart_puts("tc13 "); uart_putU32(ms); uart_puts(" ms: cycles "); uart_putU32(_cycles);
            uart_puts(" worker runs "); uart_putU32(_workerRuns);
            uart_puts(" contender locks "); uart_putU32(_contenderLocks);
            uart_puts(" worker stack free min "); uart_putU32((uint32_t)_workerStackFreeMin);
            uart_puts("\r\n");
            tc_expectRange("cycles", _cycles, ms / 20u, 0xFFFFFFFFul);
            tc_expectRange("workerRuns", _workerRuns, _cycles, _cycles);
            tc_expectRange("contenderLocks", _contenderLocks, ms / 20u, 0xFFFFFFFFul);
            tc_expectRange("errors", _errors, 0, 0);
            tc_expectRange("stackWarnings", _stackWarnings, 0, 0);
            tc_expect("worker stack never exhausted", _workerStackFreeMin > 0  &&  _workerStackFreeMin < STACK_SIZE);
            tc_finish("tc13");
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
    uart_init(UART_BAUD_DEFAULT);
    uart_puts("\r\nnornOS " RTOS_VERSION " tc13_dyntask\r\n");

    rtos_init();
    rtos_createTask(TASK_SUPER,     supervisor, NULL, _stackSuper,     sizeof(_stackSuper));
    rtos_createTask(TASK_CONTENDER, contender,  NULL, _stackContender, sizeof(_stackContender));
    rtos_start();
    return 0;
}
