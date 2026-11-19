/**
 * @file uart.c
 * The console of the host port: stdout, or a capture buffer for the unit tests (see
 * hal_arch.h). Nothing is ever received; uart_getc() says so, a receive callback is
 * never called and a receive stream stays empty.
 */

#include <stdio.h>
#include "uart.h"

static uint32_t _baud = UART_BAUD_DEFAULT;
static char *_capture = NULL;
static size_t _captureSize = 0, _captured = 0;

void uart_init(uint32_t baud)
{
    if(baud == 0u)
        baud = UART_BAUD_DEFAULT;
    _baud = baud;
}

uint32_t uart_currentBaud(void)
{
    return _baud;
}

void uart_putc(char c)
{
    if(_capture == NULL)
    {
        putchar(c);
        return;
    }
    if(_captured + 1u < _captureSize)
    {
        _capture[_captured] = c;
        _capture[_captured + 1u] = '\0';
    }
    ++ _captured;
}

int16_t uart_getc(void)
{
    return -1;
}

void uart_onReceive(void (*fn)(uint8_t c, void *arg), void *arg)
{
    (void)fn; (void)arg;
}

#if UART_CFG_RX_STREAM
void uart_setRxStream(struct rtos_stream *pS)
{
    (void)pS;
}
#endif

void uart_flush(void)
{
    if(_capture == NULL)
        fflush(stdout);
}

void uart_hostCapture(char *buf, size_t size)
{
    _capture = buf;
    _captureSize = size;
    _captured = 0;
    if(buf != NULL  &&  size > 0)
        buf[0] = '\0';
}

size_t uart_hostCaptured(void)
{
    return _captured;
}
