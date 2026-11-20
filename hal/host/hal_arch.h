#ifndef HAL_ARCH_INCLUDED
#define HAL_ARCH_INCLUDED
/**
 * @file hal_arch.h
 * The host (Linux/PC) behind the generic HAL headers, for the unit tests of the HAL and
 * of modules that log: strings are ordinary strings, so HAL_FSTR and PSTR are identity
 * macros and HAL_PROGMEM is empty; the delays return at once (the host port has no real
 * time, the tests drive the tick). The console (hal/host/uart.c) writes to stdout, or
 * into a buffer the test gives with uart_hostCapture().
 */

#include <stdint.h>
#include <stddef.h>

#define HAL_FSTR(s)         (s)
#define HAL_PROGMEM
#ifndef PSTR
# define PSTR(s)            (s)
#endif

static inline char hal_flashReadByte(const char *p)
{
    return *p;
}

static inline void hal_delayUs(uint32_t us) { (void)us; }
static inline void hal_delayMs(uint32_t ms) { (void)ms; }

/** From now on uart_putc() appends to \a buf (NUL terminated, at most \a size - 1
    characters kept; the rest is dropped) instead of writing to stdout. NULL goes back to
    stdout. uart_hostCaptured() is the number of characters written since the call,
    dropped ones included. */
void uart_hostCapture(char *buf, size_t size);
size_t uart_hostCaptured(void);

#endif  /* HAL_ARCH_INCLUDED */
