/**
 * @file tc19_sleep.c
 * Test case 19: deep sleep on the ESP32 (hal/power.h). The board sleeps three times for
 * two seconds each and wakes by the RTC timer; a counter and the RTC time of the last
 * sleep live in RTC slow memory (POWER_RTC_DATA) and survive. Checked at every wake-up:
 * the reset cause is RTOS_RESET_DEEP_SLEEP, the record is intact, and the RTC clock says
 * the sleep took 2 s within 5 % (the timer group's run time counter restarts at every
 * reset, so the RTC counter is the clock here; its 150 kHz RC is what was calibrated).
 * A power-on or other reset starts the sequence from the beginning.
 *
 *     RESULT tc19 PASS       after the third wake-up
 */

#include "rtos.h"
#include "uart.h"
#include "power.h"
#include "testresult.h"
#include "port_regs.h"

#define SLEEPS          3
#define SLEEP_MS        2000ul
#define MAGIC           0x5EEB1234ul

typedef struct { uint32_t magic; uint32_t wakes; uint32_t failures; uint64_t sleptAt; uint32_t period; } record_t;
static POWER_RTC_DATA record_t _rec;

static uint64_t rtcTime(void)
{
    volatile uint32_t * const update = (volatile uint32_t *)(DR_REG_RTCCNTL_BASE + 0x0C);
    *update = 1ul << 31;
    while((*update & (1ul << 30)) == 0)
        ;
    return ((uint64_t)(PORT_REG(DR_REG_RTCCNTL_BASE + 0x14) & 0xFFFFul) << 32) | PORT_REG(DR_REG_RTCCNTL_BASE + 0x10);
}

static void task(void *arg)
{
    const uint8_t cause = rtos_getResetCause();
    (void)arg;

    if(cause != RTOS_RESET_DEEP_SLEEP  ||  _rec.magic != MAGIC)
    {
        uart_puts("tc19 ESP32 deep sleep: fresh start (reset cause ");
        uart_putU32(cause);
        uart_puts("), sleeping 3 x 2 s\r\n");
        _rec.magic = MAGIC;
        _rec.wakes = 0;
        _rec.failures = 0;
    }
    else
    {
        /* Woken: how long did the sleep take by the RTC clock? The slow clock period is
           the one calibrated before the sleep, ~6.67 us in Q13.19. */
        const uint64_t cycles = rtcTime() - _rec.sleptAt;
        const uint32_t us = (uint32_t)((cycles * _rec.period) >> 19);
        ++ _rec.wakes;
        uart_puts("tc19: wake ");  uart_putU32(_rec.wakes);
        uart_puts(" of ");         uart_putU32(SLEEPS);
        uart_puts(", reset cause deep sleep, slept ");  uart_putU32(us / 1000ul);
        uart_puts(" ms by the RTC clock\r\n");
        tc_expect("slept 2 s within 5 %", us > SLEEP_MS * 950ul  &&  us < SLEEP_MS * 1050ul);
        _rec.failures += tc_failures;
        if(_rec.wakes >= SLEEPS)
        {
            tc_failures = (uint8_t)_rec.failures;
            _rec.magic = 0;                     /* the next run starts fresh */
            tc_finish("tc19");
        }
    }
    rtos_delay(RTOS_MS(100));                   /* a little scheduler life before the sleep */
    _rec.sleptAt = rtcTime();
    _rec.period = power_slowClockPeriod();
    uart_puts("tc19: sleeping\r\n");
    power_deepSleep(SLEEP_MS);
}

RTOS_STACK(_stack, 1024);

int main(void)
{
    uart_init(UART_BAUD_DEFAULT);
    uart_puts("\r\nnornOS " RTOS_VERSION " tc19_sleep\r\n");
    rtos_init();
    rtos_createTask(1, task, NULL, _stack, sizeof(_stack));
    rtos_start();
    return 0;
}
