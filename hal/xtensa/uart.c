/**
 * @file uart.c
 * Driver for UART0 of the ESP32: polled transmit, polled or interrupt driven receive. The ROM has already configured the pins and the
 * UART (8N1, 115200 from the crystal), so uart_init only sets the divider. The UART is
 * clocked from the APB clock, which equals the crystal until a PLL is configured.
 */

#include "uart.h"
#include "rtos.h"

#define UART_FIFO       PORT_REG(DR_REG_UART_BASE + 0x00)
#define UART_INT_ST     PORT_REG(DR_REG_UART_BASE + 0x08)
#define UART_INT_ENA    PORT_REG(DR_REG_UART_BASE + 0x0C)
#define UART_INT_CLR    PORT_REG(DR_REG_UART_BASE + 0x10)
#define UART_CLKDIV     PORT_REG(DR_REG_UART_BASE + 0x14)
#define UART_STATUS     PORT_REG(DR_REG_UART_BASE + 0x1C)
#define UART_CONF0      PORT_REG(DR_REG_UART_BASE + 0x20)
#define UART_CONF1      PORT_REG(DR_REG_UART_BASE + 0x24)
/* The receive FIFO is read through the AHB mirror of the UART: reads through the DPORT
   address can return stale bytes at high rates (ESP-IDF does the same). */
#define UART_FIFO_AHB   PORT_REG(0x60000000)

#define UART_INT_RXFIFO_FULL    (1ul << 0)
#define UART_INT_RXFIFO_TOUT    (1ul << 8)
#define UART_RX_FULL_THRESHOLD  32ul    /* bytes in the FIFO before an interrupt */
#define UART_RX_TIMEOUT         2ul     /* byte times of silence before an interrupt */
#define SRC_UART0               34u     /* ETS_UART0_INTR_SOURCE */
#define CPU_INT_UART0           18u     /* a free level 1 interrupt */

#define UART_TXFIFO_CNT(status)   (((status) >> 16) & 0xFFu)
#define UART_RXFIFO_CNT(status)   ((status) & 0xFFu)
#define UART_TICK_REF_ALWAYS_ON   (1ul << 27)
#define UART_FIFO_DEPTH           128u

#ifndef HAL_APB_HZ
# define HAL_APB_HZ     F_CPU
#endif

static uint32_t _baud = 0;

uint32_t uart_currentBaud(void)
{
    return _baud != 0 ? _baud : 115200ul;
}

void uart_init(uint32_t baud)
{
    /* Divider in 1/16 steps: integer part bits 0..19, fraction bits 20..23. The ROM left
       8N1 at roughly 115200 (divider 341.5) and its last log line may still be in the
       FIFO, so let that drain before the divider changes. */
    const uint32_t div16 = (HAL_APB_HZ * 16ul + baud / 2ul) / baud;

    _baud = baud;
    uart_flush();
    UART_CONF0 |= UART_TICK_REF_ALWAYS_ON;
    UART_CLKDIV = (div16 >> 4) | ((div16 & 0x0Fu) << 20);
}

void uart_putc(char c)
{
    while(UART_TXFIFO_CNT(UART_STATUS) >= UART_FIFO_DEPTH - 1u)
        ;
    UART_FIFO = (uint8_t)c;
}

int16_t uart_getc(void)
{
    if(UART_RXFIFO_CNT(UART_STATUS) != 0)
        return (int16_t)(UART_FIFO & 0xFFu);
    return -1;
}

static void (*_rxFn)(uint8_t c, void *arg);
static void *_rxArg;

static void rxIsr(void)
{
    const uint32_t st = UART_INT_ST;
    while(UART_RXFIFO_CNT(UART_STATUS) != 0)
    {
        const uint8_t c = (uint8_t)(UART_FIFO_AHB & 0xFFu);
        if(_rxFn != NULL)
            _rxFn(c, _rxArg);
    }
    UART_INT_CLR = st;
}

void uart_onReceive(void (*fn)(uint8_t c, void *arg), void *arg)
{
    /* Callback and argument change together: a receive interrupt between the two stores
       would call the old function with the new argument (or NULL). */
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    _rxArg = arg;
    _rxFn = fn;
    if(fn != NULL)
    {
        UART_CONF1 = (UART_CONF1 & ~(0x7Ful | (0x7Ful << 24)))
                     | UART_RX_FULL_THRESHOLD | (UART_RX_TIMEOUT << 24) | (1ul << 31);
        UART_INT_CLR = UART_INT_RXFIFO_FULL | UART_INT_RXFIFO_TOUT;
        UART_INT_ENA |= UART_INT_RXFIFO_FULL | UART_INT_RXFIFO_TOUT;
        rtos_port_attachInterrupt(CPU_INT_UART0, SRC_UART0, rxIsr);
    }
    else
        UART_INT_ENA &= ~(UART_INT_RXFIFO_FULL | UART_INT_RXFIFO_TOUT);
    rtos_port_exitCritical(cs);
}

/** Wait until every queued byte has left the FIFO, e.g. before changing the baud rate. */
void uart_flush(void)
{
    while(UART_TXFIFO_CNT(UART_STATUS) != 0)
        ;
}
