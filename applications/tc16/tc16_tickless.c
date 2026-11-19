/**
 * @file tc16_tickless.c
 * Test case 16: tickless idle at a slow tick (100 Hz, audit finding R3) with a wake-up
 * source that is not the tick. The application timer interrupts at 5 Hz and notifies the
 * waker task; between those interrupts the only timed wake-up is the periodic task once a
 * second, so the idle task sleeps up to 100 ticks and every timer interrupt lands in a
 * sleep with up to 20 ticks not yet counted.
 *
 *   2  waker     woken by the interrupt; delays 100 ms and measures how long that took
 *   1  periodic  once a second; stops the test after TC16_STOP_AFTER_MS
 *
 * Expected: the tick count the interrupt reads is never behind the cycle counter by more
 * than one tick (the missed ticks are counted before any handler or task runs), a 100 ms
 * delay started right after such a wake-up lasts 100 ms (not shortened by the ticks that
 * were missed), the periodic task kept its period, the tick count, rtos_getTimeUs and the
 * cycle counter agree at the end, and the tick interrupt fired far fewer times than there
 * were ticks (the sleep really skipped them).
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_WAKER      2
#define TASK_PERIODIC   1

#define APPL_IRQ_HZ     5ul
#if !BOARD_APP_TIMER_HZ_IS_EXACT(APPL_IRQ_HZ)
# error APPL_IRQ_HZ is not an exact rate of the application timer
#endif

#ifndef TC16_STOP_AFTER_MS
# define TC16_STOP_AFTER_MS  10000ul
#endif

#define CYCLES_PER_TICK     (F_CPU / RTOS_CFG_TICK_HZ)
#define DELAY_TICKS         RTOS_MS(100)
#define DELAY_CYCLES        (DELAY_TICKS * CYCLES_PER_TICK)

RTOS_STACK(_stackWaker, 2u * RTOS_PORT_MIN_STACK);
RTOS_STACK(_stackPeriodic, 2u * RTOS_PORT_MIN_STACK);

static uint32_t _startCycles;
static volatile uint32_t _irqs, _isrTickLagMax;
static volatile uint32_t _wakes, _shortDelays, _delayMinCycles = 0xFFFFFFFFul, _delayMaxCycles;
static volatile uint32_t _periods;


RTOS_ISR(BOARD_APP_TIMER_VECT)
{
    /* The tick count against the cycle counter: how many ticks is the kernel behind? */
    const uint32_t elapsed = (board_cycles() - _startCycles) / CYCLES_PER_TICK;
    const uint32_t tick = (uint32_t)rtos_getTick();
    const uint32_t lag = elapsed > tick ? elapsed - tick : 0;
    if(lag > _isrTickLagMax)
        _isrTickLagMax = lag;
    ++ _irqs;
    rtos_notifyFromIsr(TASK_WAKER, 1, RTOS_NOTIFY_INC);
}


static void waker(void *arg)
{
    (void)arg;
    for(;;)
    {
        uint32_t t0, dt;
        if(rtos_notifyTake(RTOS_FOREVER) == 0)
            continue;
        ++ _wakes;
        /* Started right after a wake-up from a tickless sleep: the delay must still last
           its full length. A stale tick count would make it end at the catch-up. */
        t0 = board_cycles();
        rtos_delay(DELAY_TICKS);
        dt = board_cycles() - t0;
        if(dt < _delayMinCycles) _delayMinCycles = dt;
        if(dt > _delayMaxCycles) _delayMaxCycles = dt;
        if(dt < DELAY_CYCLES - CYCLES_PER_TICK)        /* more than a tick short */
            ++ _shortDelays;
    }
}


static void periodic(void *arg)
{
    (void)arg;
    for(;;)
    {
        (void)rtos_waitPeriodic(RTOS_MS(1000));
        ++ _periods;
        if(rtos_getTick() >= RTOS_MS(TC16_STOP_AFTER_MS))
        {
            const uint32_t ms = TC16_STOP_AFTER_MS;
            const uint32_t tickMs = (uint32_t)rtos_getTick() * (1000ul / RTOS_CFG_TICK_HZ);
            const uint32_t usMs = rtos_getTimeUs() / 1000ul;
            const uint32_t cycMs = (board_cycles() - _startCycles) / (F_CPU / 1000ul);
            const uint32_t tickIrqs = rtos_port_tickInterrupts();

            uart_puts("tc16 "); uart_putU32(ms); uart_puts(" ms: ticks "); uart_putU32(tickMs);
            uart_puts(" ms, getTimeUs "); uart_putU32(usMs); uart_puts(" ms, cycles "); uart_putU32(cycMs);
            uart_puts(" ms, tick irqs "); uart_putU32(tickIrqs);
            uart_puts("\r\n  app irqs "); uart_putU32(_irqs); uart_puts(" isr tick lag max "); uart_putU32(_isrTickLagMax);
            uart_puts(" wakes "); uart_putU32(_wakes); uart_puts(" delay min/max us "); uart_putU32(_delayMinCycles / (F_CPU / 1000000ul));
            uart_puts("/"); uart_putU32(_delayMaxCycles / (F_CPU / 1000000ul)); uart_puts(" short "); uart_putU32(_shortDelays);
            uart_puts(" periods "); uart_putU32(_periods);
            uart_puts("\r\n");
            tc_expectNear("tickMs", tickMs, ms);
            tc_expectNear("usMs", usMs, ms);
            tc_expectNear("cycMs", cycMs, ms);
            tc_expectNear("irqs", _irqs, ms * APPL_IRQ_HZ / 1000ul);
            tc_expectRange("isrTickLagMax", _isrTickLagMax, 0, 1);
            tc_expectRange("shortDelays", _shortDelays, 0, 0);
            tc_expectRange("delayMinUs", _delayMinCycles / (F_CPU / 1000000ul), 90000, 120000);
            tc_expectRange("periods", _periods, ms / 1000ul - 1u, ms / 1000ul + 1u);
            tc_expect("tick interrupts far fewer than ticks (tickless worked)",
                      tickIrqs < (uint32_t)rtos_getTick() / 2u);
            tc_finish("tc16");
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
    uart_puts("\r\nnornOS " RTOS_VERSION " tc16_tickless\r\n");

    rtos_init();
    rtos_createTask(TASK_WAKER,    waker,    NULL, _stackWaker,    sizeof(_stackWaker));
    rtos_createTask(TASK_PERIODIC, periodic, NULL, _stackPeriodic, sizeof(_stackPeriodic));
    board_cycleCounterStart();
    _startCycles = board_cycles();
    board_appTimerStart(APPL_IRQ_HZ);
    rtos_start();
    return 0;
}
