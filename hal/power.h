#ifndef POWER_INCLUDED
#define POWER_INCLUDED
/**
 * @file power.h
 * Deep sleep: the chip off except its real time clock, woken by that clock after a given
 * time. Waking is a reset: RAM and every task are gone, the application starts from main()
 * and sees RTOS_RESET_DEEP_SLEEP in rtos_getResetCause(). Variables that must survive are
 * declared POWER_RTC_DATA and live in memory the sleep keeps powered; after a power-on
 * reset their content is whatever the memory held, so the application validates them (a
 * magic number). This is how a battery powered node is built: wake, measure, send, sleep.
 *
 *   ESP32    the RTC timer on the 150 kHz RC slow clock, calibrated against the crystal
 *            before each sleep (about 7 ms), so the sleep time is right to a tenth of a
 *            percent; 8 KB of RTC slow memory for POWER_RTC_DATA; the digital domain, the
 *            radio, the fast RTC memory and VDD_SDIO (the flash) are powered down, about
 *            10 uA on the module. Implemented in hal/xtensa/power.c after ESP-IDF's
 *            rtc_sleep.c. The APP CPU (AMP) is off too. Wake-up by a pin is not here yet.
 *   AVR      not available: the kernel's tick runs on a timer the sleep modes stop, and a
 *            tick source that keeps running (Timer2 on a 32 kHz crystal, or an external
 *            RTC) is planned, not built (nornInternal/nornCore/kernel_plan.md step 9).
 *
 *   power_deepSleep(ms)      flushes the console, stops the kernel and sleeps; never returns
 *   power_slowClockPeriod()  the calibrated period of the clock that keeps time in the sleep
 *   POWER_RTC_DATA           attribute for a variable kept through the sleep (ESP32)
 *
 * Light sleep (CPU and clocks stopped, RAM kept, execution continues) is not here either;
 * RTOS_CFG_TICKLESS on the ESP32 skips ticks but keeps the CPU clocked. README "Low power".
 */

#include <stdint.h>

#if defined(__XTENSA__)
# define POWER_RTC_DATA     __attribute__((section(".rtc.data")))
#endif

/** Deep sleep for @p ms milliseconds, then a reset with RTOS_RESET_DEEP_SLEEP. Call from a
    task or from main() before rtos_start(); interrupts are masked and nothing runs
    afterwards. The console output is flushed first. ESP32 only. */
void power_deepSleep(uint32_t ms) __attribute__((noreturn));

/** ESP32: the period of the RTC slow clock in microseconds as Q13.19 (about 6.67 us for
    the 150 kHz RC oscillator), measured against the crystal over 1024 cycles, which takes
    about 7 ms. power_deepSleep() calls it before every sleep; an application that reads the
    RTC counter itself (to know how long it slept) uses the same value. */
uint32_t power_slowClockPeriod(void);

#endif  /* POWER_INCLUDED */
