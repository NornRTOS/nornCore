/**
 * @file uart.c
 * Driver for UART0 of the ATmega2560/328P: init, one byte out, one byte in by polling,
 * and with UART_CFG_RX_STREAM a receive interrupt into a stream buffer. The formatting
 * functions are in hal/uart_fmt.c.
 */

#include <avr/io.h>
#include "uart.h"
#if UART_CFG_RX_STREAM
#include "rtos.h"
#endif

void uart_init(uint32_t baud)
{
    if(baud == 0u)
        baud = UART_BAUD_DEFAULT;

    const uint16_t ubrr = (uint16_t)((F_CPU + 8ul * baud) / (16ul * baud) - 1ul);

    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)ubrr;
    UCSR0A = 0;
    UCSR0B = _BV(RXEN0) | _BV(TXEN0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
}

void uart_putc(char c)
{
    while((UCSR0A & _BV(UDRE0)) == 0)
        ;
    UDR0 = (uint8_t)c;
}

void uart_flush(void)
{
    while((UCSR0A & _BV(UDRE0)) == 0)
        ;
}

int16_t uart_getc(void)
{
    if(UCSR0A & _BV(RXC0))
        return (int16_t)UDR0;
    return -1;
}

#if UART_CFG_RX_STREAM
/* The ATmega2560 numbers its USARTs, the ATmega328P has one. */
#ifdef USART0_RX_vect
# define UART0_RX_VECT  USART0_RX_vect
#else
# define UART0_RX_VECT  USART_RX_vect
#endif

static struct rtos_stream *_rxStream;

/** One byte per interrupt; the stream wakes its reader at the trigger level and the
    RTOS_ISR tail switches task if that reader is the higher one. */
RTOS_ISR(UART0_RX_VECT)
{
    const uint8_t c = UDR0;
    if(_rxStream != NULL)
        (void)rtos_streamWriteFromIsr(_rxStream, &c, 1);
}

void uart_setRxStream(struct rtos_stream *pS)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    _rxStream = pS;
    if(pS != NULL)
        UCSR0B |= _BV(RXCIE0);
    else
        UCSR0B &= (uint8_t)~_BV(RXCIE0);
    rtos_port_exitCritical(cs);
}
#endif  /* UART_CFG_RX_STREAM */
