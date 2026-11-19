#ifndef UART_INCLUDED
#define UART_INCLUDED
/**
 * @file uart.h
 * Minimal driver for the console UART (UART0), for logging from applications and test
 * cases. Implemented per architecture in hal/<arch>/uart.c. Transmit is polled: a call
 * blocks until the byte is in the transmit register, so it is cheap for short messages
 * and simple to use from any task. Shared use from several tasks needs a mutex around
 * whole messages. Receive is polled (uart_getc) or, on request, interrupt driven into a
 * stream buffer (uart_setRxStream, UART_CFG_RX_STREAM) or a callback (uart_onReceive,
 * ESP32).
 */

#include <stdint.h>
#include "hal_arch.h"
#include "rtos_config.h"

/** 1: receive by interrupt into a stream buffer, uart_setRxStream() below. Needs
    RTOS_CFG_STREAMS. Off, the driver has no receive interrupt (AVR) and the function is
    not there; on, the AVR image carries the receive interrupt whether it is used or not,
    since the vector table refers to it. */
#ifndef UART_CFG_RX_STREAM
# define UART_CFG_RX_STREAM 0
#endif
#if UART_CFG_RX_STREAM && !RTOS_CFG_STREAMS
# error UART_CFG_RX_STREAM needs RTOS_CFG_STREAMS 1 in rtos_config.h
#endif

/** The baud rate uart_init(0) means: what the boards' monitors and the ROM use. */
#define UART_BAUD_DEFAULT   115200ul

/** Initialize UART0 for 8N1 at \a baud; 0 means UART_BAUD_DEFAULT. */
void uart_init(uint32_t baud);

/** The baud rate uart_init set, or UART_BAUD_DEFAULT before the first call. */
uint32_t uart_currentBaud(void);

void uart_putc(char c);

/** Write a RAM string. */
void uart_puts(const char *s);

/** Write a string in program memory, e.g. uart_putsP(HAL_FSTR("hello")). */
void uart_putsP(const char *s);

/** Write an unsigned number in decimal. */
void uart_putU32(uint32_t n);

/** Write a signed number in decimal. */
void uart_putI32(int32_t n);

/** Write a byte as two hex digits. */
void uart_putHex8(uint8_t b);

/** Read a byte if one is available, else return -1. */
int16_t uart_getc(void);

/** Receive by interrupt instead of polling: \a fn is called in interrupt context with
    every byte that arrives, from the moment it is set; NULL goes back to polling.
    ESP32 (UART0) only so far: AVR and the host port keep polling. */
void uart_onReceive(void (*fn)(uint8_t c, void *arg), void *arg);

#if UART_CFG_RX_STREAM
struct rtos_stream;
/** Receive by interrupt into a stream buffer (rtos_stream_t, initialized by the caller):
    every byte that arrives is written to it with rtos_streamWriteFromIsr from the moment
    it is set, and a task reading the stream is woken at its trigger level. NULL goes back
    to polling. A byte that arrives when the stream is full is lost. AVR and ESP32; the
    host port receives nothing. Replaces uart_onReceive on the ESP32: the last call wins.

        static uint8_t _rxBuf[64];
        static rtos_stream_t _rx;
        rtos_streamInit(&_rx, _rxBuf, sizeof _rxBuf, 1);
        uart_setRxStream(&_rx);
        ...
        n = rtos_streamRead(&_rx, line, sizeof line, RTOS_FOREVER);   // at least 1 byte */
void uart_setRxStream(struct rtos_stream *pS);
#endif

/** Block until every queued byte has been sent. */
void uart_flush(void);

#endif  /* UART_INCLUDED */
