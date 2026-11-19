#ifndef BOARD_INCLUDED
#define BOARD_INCLUDED
/**
 * @file board.h
 * Board support for the ESP32 boards nornOS runs on. BOARD=esp32 is a 38 pin
 * NodeMCU-32S style devkit with an ESP32-S module, CP2102 on UART0 and the LED on GPIO2.
 * Pins are plain GPIO numbers: GPIO_PIN(2).
 *
 * Application timer: timer group 0, timer 0, routed to CPU interrupt 17. The application
 * defines RTOS_ISR(BOARD_APP_TIMER_VECT) and calls board_appTimerStart(hz).
 */

#include "gpio.h"
#include "port.h"

#define BOARD_NAME      "ESP32"
#define BOARD_IS_ESP32  1

#define BOARD_PIN_LED   GPIO_PIN(2)
#define BOARD_PIN_BOOT  GPIO_PIN(0)     /* the IO0 button, active low */

#ifndef HAL_APB_HZ
# define HAL_APB_HZ     F_CPU
#endif

#define BOARD_APP_TIMER_VECT            board_appTimer
#define BOARD_APP_TIMER_PRESCALER       2ul
#define BOARD_APP_TIMER_HZ_IS_EXACT(hz) ((HAL_APB_HZ / BOARD_APP_TIMER_PRESCALER) % (hz) == 0)

void board_appTimerStart(uint32_t hz);

/** Counts of the application timer since its last interrupt, for latency measurements;
    BOARD_APP_TIMER_COUNT_HZ counts per second. */
#define BOARD_APP_TIMER_COUNT_HZ    (HAL_APB_HZ / BOARD_APP_TIMER_PRESCALER)
uint32_t board_appTimerElapsed(void);

/* Cycle counter and high resolution probe timer for the kernel benchmark (tc10): the
   CPU's CCOUNT register, and the application timer, whose 40 MHz counter is fine enough
   to serve as the probe as well. The two must not be used together with the app timer. */
#define BOARD_HAS_CYCLE_COUNTER     1
#define BOARD_CYCLES_HZ             F_CPU
#define BOARD_CYCLES_MASK           0xFFFFFFFFul
static inline void board_cycleCounterStart(void) { }
static inline uint32_t board_cycles(void) { return rtos_port_ccount(); }

#define BOARD_HIRES_TIMER_VECT              BOARD_APP_TIMER_VECT
#define BOARD_HIRES_TIMER_COUNT_HZ          BOARD_APP_TIMER_COUNT_HZ
#define BOARD_HIRES_TIMER_HZ_IS_EXACT(hz)   BOARD_APP_TIMER_HZ_IS_EXACT(hz)
#define board_hiresTimerStart(hz)           board_appTimerStart(hz)
#define board_hiresTimerElapsed()           board_appTimerElapsed()
void board_hiresTimerSetPeriod(uint32_t counts);

#endif  /* BOARD_INCLUDED */
