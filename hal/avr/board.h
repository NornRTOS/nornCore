#ifndef BOARD_INCLUDED
#define BOARD_INCLUDED
/**
 * @file board.h
 * Board support: maps Arduino pin numbers to AVR ports for the boards nornOS runs on. The
 * board is chosen by the MCU the compiler targets (-mmcu, set from BOARD in the Makefile):
 *
 *   BOARD=mega2560   ATmega2560 / ATmega1280, Arduino Mega
 *   BOARD=uno        ATmega328P / ATmega168,  Arduino UNO, Nano, Pro Mini
 *
 * Applications use BOARD_PIN_Dn in place of GPIO_PIN(port, bit) to stay portable. Timer 2
 * (8 bit, present on both) is offered as the application timer; the kernel tick uses a 16
 * bit timer (RTOS_CFG_TIMER, default 3 on the Mega and 1 on the UNO).
 */

#include "gpio.h"

#if defined(__AVR_ATmega2560__) || defined(__AVR_ATmega1280__)
# define BOARD_NAME     "Mega"
# define BOARD_IS_MEGA  1
# define BOARD_PIN_D0   GPIO_PIN(E, 0)
# define BOARD_PIN_D1   GPIO_PIN(E, 1)
# define BOARD_PIN_D2   GPIO_PIN(E, 4)
# define BOARD_PIN_D3   GPIO_PIN(E, 5)
# define BOARD_PIN_D4   GPIO_PIN(G, 5)
# define BOARD_PIN_D5   GPIO_PIN(E, 3)
# define BOARD_PIN_D6   GPIO_PIN(H, 3)
# define BOARD_PIN_D7   GPIO_PIN(H, 4)
# define BOARD_PIN_D8   GPIO_PIN(H, 5)
# define BOARD_PIN_D9   GPIO_PIN(H, 6)
# define BOARD_PIN_D10  GPIO_PIN(B, 4)
# define BOARD_PIN_D11  GPIO_PIN(B, 5)
# define BOARD_PIN_D12  GPIO_PIN(B, 6)
# define BOARD_PIN_D13  GPIO_PIN(B, 7)
#elif defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__) || defined(__AVR_ATmega168P__)
# define BOARD_NAME     "UNO"
# define BOARD_IS_UNO   1
# define BOARD_PIN_D0   GPIO_PIN(D, 0)
# define BOARD_PIN_D1   GPIO_PIN(D, 1)
# define BOARD_PIN_D2   GPIO_PIN(D, 2)
# define BOARD_PIN_D3   GPIO_PIN(D, 3)
# define BOARD_PIN_D4   GPIO_PIN(D, 4)
# define BOARD_PIN_D5   GPIO_PIN(D, 5)
# define BOARD_PIN_D6   GPIO_PIN(D, 6)
# define BOARD_PIN_D7   GPIO_PIN(D, 7)
# define BOARD_PIN_D8   GPIO_PIN(B, 0)
# define BOARD_PIN_D9   GPIO_PIN(B, 1)
# define BOARD_PIN_D10  GPIO_PIN(B, 2)
# define BOARD_PIN_D11  GPIO_PIN(B, 3)
# define BOARD_PIN_D12  GPIO_PIN(B, 4)
# define BOARD_PIN_D13  GPIO_PIN(B, 5)
#else
# error Unsupported MCU: add its pin map to hal/board.h
#endif

/** The on-board LED, Arduino pin 13 on both boards. */
#define BOARD_PIN_LED   BOARD_PIN_D13

/** Timer 2 in CTC mode runs at F_CPU / BOARD_TIMER2_PRESCALER counts per second. */
#define BOARD_TIMER2_PRESCALER  128ul

/** Compare value for a timer 2 interrupt rate of hz; the application should check with
    BOARD_TIMER2_HZ_IS_EXACT that the division has no remainder. */
#define BOARD_TIMER2_TOP(hz)          (F_CPU / BOARD_TIMER2_PRESCALER / (hz) - 1ul)
#define BOARD_TIMER2_HZ_IS_EXACT(hz)  ((F_CPU / BOARD_TIMER2_PRESCALER) % (hz) == 0 \
                                       && BOARD_TIMER2_TOP(hz) <= 255ul)

/**
 * Start timer 2 in CTC mode with prescaler 128 and enable its compare A interrupt. The
 * application provides RTOS_ISR(TIMER2_COMPA_vect).
 *   @param top Compare value, normally BOARD_TIMER2_TOP(hz).
 */
static inline void board_timer2StartCtc(uint8_t top)
{
    TCCR2A = _BV(WGM21);
    TCCR2B = 0;
    TCNT2  = 0;
    OCR2A  = top;
    TIFR2  = _BV(OCF2A);
    TIMSK2 = _BV(OCIE2A);
    TCCR2B = _BV(CS22) | _BV(CS20);
}

/* Application timer with the same names on every architecture: timer 2 here. */
#define BOARD_APP_TIMER_VECT            TIMER2_COMPA_vect
#define BOARD_APP_TIMER_HZ_IS_EXACT(hz) BOARD_TIMER2_HZ_IS_EXACT(hz)

static inline void board_appTimerStart(uint32_t hz)
{
    board_timer2StartCtc((uint8_t)BOARD_TIMER2_TOP(hz));
}

/** Counts of the application timer since its last interrupt, for latency measurements;
    BOARD_APP_TIMER_COUNT_HZ counts per second. */
#define BOARD_APP_TIMER_COUNT_HZ    (F_CPU / BOARD_TIMER2_PRESCALER)
static inline uint32_t board_appTimerElapsed(void)
{
    return TCNT2;
}

/*
 * Cycle counter and high resolution probe timer, used by the kernel benchmark (tc10).
 * Only on the Mega: timer 1 counts CPU cycles (prescaler 1, free running, 16 bits, so a
 * difference is valid for 4 ms) and timer 4 fires BOARD_HIRES_TIMER_VECT at a chosen
 * rate, its counter telling how many cycles ago the compare match happened. On the UNO
 * timer 1 is the tick and there is no timer 4.
 */
#if defined(BOARD_IS_MEGA)  &&  BOARD_IS_MEGA
# define BOARD_HAS_CYCLE_COUNTER    1
# define BOARD_CYCLES_HZ            F_CPU
# define BOARD_CYCLES_MASK          0xFFFFul

static inline void board_cycleCounterStart(void)
{
    TCCR1A = 0;
    TCCR1B = _BV(CS10);
    TIMSK1 = 0;
    TCNT1  = 0;
}

/** Cycle count since board_cycleCounterStart, modulo BOARD_CYCLES_MASK + 1. Safe in
    tasks and interrupts (the 16 bit read is done with interrupts disabled). */
static inline uint32_t board_cycles(void)
{
    uint16_t c;
    const uint8_t s = SREG;
    cli();
    c = TCNT1;
    SREG = s;
    return c;
}

# define BOARD_HIRES_TIMER_VECT             TIMER4_COMPA_vect
# define BOARD_HIRES_TIMER_COUNT_HZ         F_CPU
# define BOARD_HIRES_TIMER_HZ_IS_EXACT(hz)  (F_CPU % (hz) == 0  &&  F_CPU / (hz) - 1ul <= 65535ul)

/** Timer 4 in CTC mode at prescaler 1: BOARD_HIRES_TIMER_VECT fires hz times per second;
    the application provides RTOS_ISR(BOARD_HIRES_TIMER_VECT). */
static inline void board_hiresTimerStart(uint32_t hz)
{
    TCCR4A = 0;
    TCCR4B = 0;
    TCNT4  = 0;
    OCR4A  = (uint16_t)(F_CPU / hz - 1ul);
    TIFR4  = _BV(OCF4A);
    TIMSK4 = _BV(OCIE4A);
    TCCR4B = _BV(WGM42) | _BV(CS40);
}

/** Cycles since the last compare match of the probe timer; call it first in the ISR. */
static inline uint32_t board_hiresTimerElapsed(void)
{
    return TCNT4;
}

/** Change the probe period to \a counts (cycles) from the next match on. Meant to be
    called in the probe ISR to dither the period, so that the probes sweep every phase of
    the tick and of the other periodic activity instead of hitting the same few. */
static inline void board_hiresTimerSetPeriod(uint32_t counts)
{
    OCR4A = (uint16_t)(counts - 1ul);
}
#else
# define BOARD_HAS_CYCLE_COUNTER    0
#endif

#endif  /* BOARD_INCLUDED */
