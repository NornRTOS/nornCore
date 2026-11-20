/**
 * @file tc17_smp.c
 * Test case 17: the SMP kernel on the two cores of the ESP32 (RTOS_CFG_SMP, nornInternal/nornCore/smp_plan.md
 * step 7). Three tasks and the application timer interrupt on core 0:
 *
 *   3  high    woken by the interrupt (200 Hz); pinned to core 0 for the first half of the
 *              run, to core 1 for the second, so that every wake-up of the second half
 *              crosses the cores (IPI); it measures the time from the interrupt to its run
 *   2  mid     periodic every 10 ms; ends the test after TC17_STOP_AFTER_MS
 *   1  low     runs whenever a core is free, so it is the one that gets preempted and
 *              moves between the cores
 *
 * Each task notes the cores it ran on, how many tasks were inside their work at once (a
 * count no single core can bring above 1), and takes a ceiling mutex for a moment. At the
 * end the mid task suspends the two others from whatever core they run on, which must
 * stop them there, and checks: the interrupt count, every wake-up delivered, the high
 * task's cores per phase, the low task on both cores, at least two tasks at work at the
 * same time, the mutex counter adding up, the loads within range, and the wake-up across
 * the cores within a reasonable time.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_LOW        1
#define TASK_MID        2
#define TASK_HIGH       3

#define APPL_IRQ_HZ     200ul
#if !BOARD_APP_TIMER_HZ_IS_EXACT(APPL_IRQ_HZ)
# error APPL_IRQ_HZ is not an exact rate of the application timer
#endif

#ifndef TC17_STOP_AFTER_MS
# define TC17_STOP_AFTER_MS  3000ul
#endif
#define PHASE_MS        (TC17_STOP_AFTER_MS / 2ul)

#if !RTOS_CFG_SMP || !RTOS_CFG_SMP_AFFINITY
# error tc17 needs RTOS_CFG_SMP and RTOS_CFG_SMP_AFFINITY (rtos_config.h)
#endif

RTOS_STACK(_stackLow, RTOS_STACK_DEFAULT);
RTOS_STACK(_stackMid, RTOS_STACK_DEFAULT);
RTOS_STACK(_stackHigh, RTOS_STACK_DEFAULT);

static rtos_mutex_t _mtx;
static volatile uint32_t _irqs, _irqUs;                 /* the interrupt: count, time of the last */
static volatile uint32_t _wakes, _wakesCross, _latMin = 0xFFFFFFFFul, _latMax, _latSum;
static volatile uint32_t _coreMask[RTOS_NUM_TASKS];     /* cores each task ran on */
static volatile uint32_t _coreMaskHighPhase1;           /* the high task's cores in the first half */
static volatile uint32_t _inside, _maxInside;
static volatile uint32_t _mtxLocks[RTOS_NUM_TASKS], _mtxViolations;
static volatile uint8_t _mtxHolder = RTOS_NO_TASK;
static uint32_t _mtxCounter;                            /* under the mutex */
static volatile uint8_t _phase = 1;
static volatile uint32_t _maskedInWork[RTOS_PORT_CORES];    /* diagnostics: PS.INTLEVEL != 0 in task context */

static void atomicInc(volatile uint32_t *p) { (void)__atomic_fetch_add(p, 1u, __ATOMIC_RELAXED); }
static void atomicOr(volatile uint32_t *p, uint32_t v) { (void)__atomic_fetch_or(p, v, __ATOMIC_RELAXED); }
static void atomicMax(volatile uint32_t *p, uint32_t v)
{
    uint32_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while(v > cur  &&  !__atomic_compare_exchange_n(p, &cur, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

/** The work of every task: note the core, the overlap, hold the mutex a moment. */
static void work(uint8_t me, uint32_t spin)
{
    volatile uint32_t n = spin;
    const uint32_t inside = __atomic_add_fetch(&_inside, 1u, __ATOMIC_RELAXED);
    uint32_t ps;
    __asm__ __volatile__ ("rsr %0, ps" : "=a"(ps));
    if((ps & 0xFu) != 0)
        atomicInc(&_maskedInWork[rtos_port_coreId()]);
    atomicMax(&_maxInside, inside);
    atomicOr(&_coreMask[me], 1ul << rtos_port_coreId());
    while(n-- > 0)
        ;
    rtos_mutexLock(&_mtx);
    if(_mtxHolder != RTOS_NO_TASK)
        atomicInc(&_mtxViolations);
    _mtxHolder = me;
    ++ _mtxCounter;
    n = spin / 8u;
    while(n-- > 0)
        ;
    if(_mtxHolder != me)
        atomicInc(&_mtxViolations);
    _mtxHolder = RTOS_NO_TASK;
    rtos_mutexUnlock(&_mtx);
    atomicInc(&_mtxLocks[me]);
    (void)__atomic_sub_fetch(&_inside, 1u, __ATOMIC_RELAXED);
}


RTOS_ISR(BOARD_APP_TIMER_VECT)
{
    _irqUs = rtos_getTimeUs();
    ++ _irqs;
    rtos_notifyFromIsr(TASK_HIGH, 1, RTOS_NOTIFY_INC);
}


static void high(void *arg)
{
    (void)arg;
    for(;;)
    {
        uint32_t lat;
        if(rtos_notifyTake(RTOS_FOREVER) == 0)
            continue;
        lat = rtos_getTimeUs() - _irqUs;            /* the interrupt is on core 0 */
        ++ _wakes;
        if(rtos_port_coreId() != 0)
        {
            ++ _wakesCross;
            if(lat < _latMin) _latMin = lat;
            if(lat > _latMax) _latMax = lat;
            _latSum += lat;
        }
        work(TASK_HIGH, 2000);
    }
}


static void low(void *arg)
{
    (void)arg;
    for(;;)
        work(TASK_LOW, 3000);
}


static void mid(void *arg)
{
    rtos_taskInfo_t info;
    (void)arg;
    for(;;)
    {
        (void)rtos_waitPeriodic(RTOS_MS(10));
        work(TASK_MID, 4000);

        if(_phase == 1  &&  rtos_getTick() >= RTOS_MS(PHASE_MS))
        {
            _coreMaskHighPhase1 = _coreMask[TASK_HIGH];
            _coreMask[TASK_HIGH] = 0;
            _phase = 2;
            rtos_setAffinity(TASK_HIGH, 1u << 1);          /* from now on: core 1 only */
        }
        if(rtos_getTick() >= RTOS_MS(TC17_STOP_AFTER_MS))
        {
            const uint32_t ms = TC17_STOP_AFTER_MS;
            uint32_t locks, load, load0, load1;
            uint8_t t;

            const uint32_t irqsAtStop = _irqs;
            rtos_suspend(TASK_LOW);                     /* wherever they run */
            rtos_suspend(TASK_HIGH);
            rtos_delay(RTOS_MS(20));
            locks = 0;
            for(t = 1; t < RTOS_NUM_TASKS; ++t)
                locks += _mtxLocks[t];
            load = rtos_getCpuLoad();
            load0 = rtos_getCoreLoad(0);
            load1 = rtos_getCoreLoad(1);

            uart_puts("tc17 "); uart_putU32(ms); uart_puts(" ms: irqs "); uart_putU32(_irqs);
            uart_puts(" wakes "); uart_putU32(_wakes); uart_puts(" cross "); uart_putU32(_wakesCross);
            uart_puts(" cross latency us min/avg/max "); uart_putU32(_latMin); uart_putc('/');
            uart_putU32(_wakesCross ? _latSum / _wakesCross : 0); uart_putc('/'); uart_putU32(_latMax);
            uart_puts("\r\n  cores: low "); uart_putU32(_coreMask[TASK_LOW]); uart_puts(" mid "); uart_putU32(_coreMask[TASK_MID]);
            uart_puts(" high phase 1 "); uart_putU32(_coreMaskHighPhase1); uart_puts(" phase 2 "); uart_putU32(_coreMask[TASK_HIGH]);
            uart_puts(" max inside "); uart_putU32(_maxInside);
            uart_puts("\r\n  mutex locks "); uart_putU32(locks); uart_puts(" counter "); uart_putU32(_mtxCounter);
            uart_puts(" violations "); uart_putU32(_mtxViolations);
            uart_puts("; load "); uart_putU32(load); uart_puts(" % cores "); uart_putU32(load0); uart_putc(' '); uart_putU32(load1);
            uart_puts(" %; ipi sent "); uart_putU32(rtos_port_ipiSent[0]); uart_putc('/'); uart_putU32(rtos_port_ipiSent[1]);
            uart_puts(" taken "); uart_putU32(rtos_port_ipiTaken[0]); uart_putc('/'); uart_putU32(rtos_port_ipiTaken[1]);
            uart_puts("\r\n");

            tc_expectNear("irqs", irqsAtStop, ms * APPL_IRQ_HZ / 1000ul);
            tc_expect("every wake-up before the stop delivered", _wakes + 1u >= irqsAtStop  &&  _wakes <= irqsAtStop);
            tc_expectRange("highPhase1", _coreMaskHighPhase1, 1, 1);          /* core 0 only */
            tc_expectRange("highPhase2", _coreMask[TASK_HIGH], 2, 2);         /* core 1 only */
            tc_expectRange("lowCores", _coreMask[TASK_LOW], 3, 3);            /* both */
            tc_expectRange("maxInside", _maxInside, 2, 3);
            tc_expectRange("mtxViolations", _mtxViolations, 0, 0);
            /* A task suspended between its increment of the counter and of its own count
               leaves a difference of at most one per suspended task. */
            tc_expect("mutex counter adds up", _mtxCounter >= locks  &&  _mtxCounter <= locks + 2u);
            tc_expectRange("wakesCross", _wakesCross, _irqs / 2u - 10u, _irqs / 2u + 2u);
            /* 16 us on the board; an emulator's time slice for the other core can be
               hundreds of microseconds, so its limit is wider (as tc10 and tc12 do). */
            tc_expectRange("crossLatencyMaxUs", _latMax, 1, TC_TOLERANCE_PERMILLE > 10u ? 5000 : 500);
            tc_expectRange("load", load, 1, 100);
            tc_expect("core loads within range", load0 <= 100u  &&  load1 <= 100u);
            tc_expect("tasks never ran with interrupts masked", _maskedInWork[0] == 0  &&  _maskedInWork[1] == 0);
            tc_expect("IPIs reached core 1", rtos_port_ipiTaken[1] >= rtos_port_ipiSent[1] / 2u  &&  rtos_port_ipiTaken[1] <= rtos_port_ipiSent[1]);
            tc_expect("suspended tasks stopped on their cores",
                      rtos_getTaskInfo(TASK_LOW, &info)  &&  info.state == RTOS_TASK_SUSPENDED  &&  info.core == RTOS_NO_CORE
                      &&  rtos_getTaskInfo(TASK_HIGH, &info)  &&  info.state == RTOS_TASK_SUSPENDED  &&  info.core == RTOS_NO_CORE);
            tc_expect("this task runs on a core", rtos_getTaskInfo(TASK_MID, &info)  &&  info.core == rtos_port_coreId());
            tc_finish("tc17");
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
    uart_puts("\r\nnornOS " RTOS_VERSION " tc17_smp\r\n");

    rtos_init();
    rtos_mutexInit(&_mtx, TASK_HIGH);
    rtos_createTask(TASK_LOW,  low,  NULL, _stackLow,  sizeof(_stackLow));
    rtos_createTask(TASK_MID,  mid,  NULL, _stackMid,  sizeof(_stackMid));
    rtos_createTask(TASK_HIGH, high, NULL, _stackHigh, sizeof(_stackHigh));
    rtos_setAffinity(TASK_HIGH, 1u);                    /* phase 1: core 0, with its interrupt */

    board_appTimerStart(APPL_IRQ_HZ);
    rtos_start();
    return 0;
}
