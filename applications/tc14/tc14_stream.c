/**
 * @file tc14_stream.c
 * Test case 14: stream buffers (RTOS_CFG_STREAMS) on a board or in the simulator. The
 * application timer interrupt writes a burst of 8 sequence bytes into stream A at 500 Hz;
 * the consumer sleeps until 32 bytes are in (the trigger), reads them and checks the
 * sequence. Between two tasks, a producer writes 100 byte blocks into the 64 byte stream
 * B, blocking for room, and a slow reader takes 7 bytes every 2 ms and checks them: the
 * writer's wait and the hand-over of room run thousands of times.
 *
 *   3  consumer of stream A (woken by the interrupt at the trigger level)
 *   2  producer into stream B (blocks on the full stream)
 *   1  slow reader of stream B
 *
 * Stops after TC14_STOP_AFTER_MS (3 s) and expects: every byte the interrupt wrote read
 * in order, no byte dropped (the consumer keeps up), the task stream in order with the
 * writer blocked and woken at least a few hundred times.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_CONSUMER   3
#define TASK_PRODUCER   2
#define TASK_SLOW       1

#define APPL_IRQ_HZ     500ul
#if !BOARD_APP_TIMER_HZ_IS_EXACT(APPL_IRQ_HZ)
# error APPL_IRQ_HZ is not an exact rate of the application timer
#endif

#ifndef TC14_STOP_AFTER_MS
# define TC14_STOP_AFTER_MS  3000ul
#endif

#if RTOS_PORT_MIN_STACK > 192u
# define STACK_SIZE     (2u * RTOS_PORT_MIN_STACK)
#else
# define STACK_SIZE     192u
#endif

RTOS_STACK(_stackConsumer, STACK_SIZE);
RTOS_STACK(_stackProducer, STACK_SIZE);
RTOS_STACK(_stackSlow, STACK_SIZE);

static uint8_t _ringA[128], _ringB[64];
static rtos_stream_t _a, _b;

static volatile uint32_t _isrWrites, _isrBytes, _isrDropped;
static volatile uint32_t _aBytes, _aErrors, _aReads;
static volatile uint32_t _bWritten, _bRead, _bErrors, _bWaits;
static uint8_t _isrNext = 0;


RTOS_ISR(BOARD_APP_TIMER_VECT)
{
    uint8_t burst[8];
    uint8_t i, n;
    for(i = 0; i < 8; ++i)
        burst[i] = (uint8_t)(_isrNext + i);
    n = (uint8_t)rtos_streamWriteFromIsr(&_a, burst, 8);
    ++ _isrWrites;
    _isrBytes += n;
    _isrDropped += (uint32_t)(8u - n);
    _isrNext = (uint8_t)(_isrNext + n);         /* the sequence continues from what went in */
}


static void consumer(void *arg)
{
    uint8_t expect = 0;
    (void)arg;
    for(;;)
    {
        uint8_t buf[64];
        const uint16_t n = rtos_streamRead(&_a, buf, sizeof buf, RTOS_MS(100));
        uint16_t i;
        ++ _aReads;
        for(i = 0; i < n; ++i)
            if(buf[i] != (uint8_t)(expect + i))
                ++ _aErrors;
        expect = (uint8_t)(expect + n);
        _aBytes += n;
    }
}


static void producer(void *arg)
{
    uint8_t next = 0;
    (void)arg;
    for(;;)
    {
        uint8_t block[100];
        uint16_t i, n;
        for(i = 0; i < sizeof block; ++i)
            block[i] = (uint8_t)(next + i);
        if(rtos_streamSpace(&_b) < sizeof block)
            ++ _bWaits;                             /* this write will block at least once */
        n = rtos_streamWrite(&_b, block, sizeof block, RTOS_FOREVER);
        if(n != sizeof block)
            ++ _bErrors;
        next = (uint8_t)(next + n);
        _bWritten += n;
    }
}


static void slowReader(void *arg)
{
    uint8_t expect = 0;
    const rtos_tick_t start = rtos_getTick();
    (void)arg;
    for(;;)
    {
        uint8_t buf[7];
        uint16_t i, n;
        (void)rtos_waitPeriodic(RTOS_MS(2));
        n = rtos_streamRead(&_b, buf, sizeof buf, RTOS_NO_WAIT);
        for(i = 0; i < n; ++i)
            if(buf[i] != (uint8_t)(expect + i))
                ++ _bErrors;
        expect = (uint8_t)(expect + n);
        _bRead += n;

        if((uint32_t)(rtos_getTick() - start) >= RTOS_MS(TC14_STOP_AFTER_MS))
        {
            const uint32_t ms = TC14_STOP_AFTER_MS;
            const uint32_t isrWrites = _isrWrites, isrBytes = _isrBytes, aBytes = _aBytes;
            uart_puts("tc14 "); uart_putU32(ms); uart_puts(" ms: isr writes "); uart_putU32(isrWrites);
            uart_puts(" bytes "); uart_putU32(isrBytes); uart_puts(" dropped "); uart_putU32(_isrDropped);
            uart_puts(" consumer reads "); uart_putU32(_aReads); uart_puts(" bytes "); uart_putU32(aBytes);
            uart_puts(" errors "); uart_putU32(_aErrors);
            uart_puts("\r\n  task stream written "); uart_putU32(_bWritten); uart_puts(" read "); uart_putU32(_bRead);
            uart_puts(" writer waits "); uart_putU32(_bWaits); uart_puts(" errors "); uart_putU32(_bErrors);
            uart_puts("\r\n");
            tc_expectNear("isrWrites", isrWrites, ms * APPL_IRQ_HZ / 1000ul);
            tc_expectRange("isrDropped", _isrDropped, 0, 0);
            tc_expectRange("aErrors", _aErrors, 0, 0);
            tc_expect("consumer read everything but the last burst", isrBytes - aBytes <= 8u * 4u);
            tc_expect("consumer woken per trigger, not per burst", _aReads <= isrWrites / 3u);
            tc_expectRange("bErrors", _bErrors, 0, 0);
            tc_expectRange("bRead", _bRead, ms / 2u * 7u / 2u, ms / 2u * 7u);
            /* The writer is higher than this reader: it refilled the room this read made
               before the read returned, and sits blocked on the full stream again. */
            tc_expect("writer kept the stream full", rtos_streamCount(&_b) >= 50u);
            tc_expectRange("bWaits", _bWaits, 10, 0xFFFFFFFFul);
            tc_finish("tc14");
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
    uart_puts("\r\nnornOS " RTOS_VERSION " tc14_stream\r\n");

    rtos_init();
    rtos_streamInit(&_a, _ringA, sizeof _ringA, 32);
    rtos_streamInit(&_b, _ringB, sizeof _ringB, 1);
    rtos_createTask(TASK_CONSUMER, consumer,   NULL, _stackConsumer, sizeof(_stackConsumer));
    rtos_createTask(TASK_PRODUCER, producer,   NULL, _stackProducer, sizeof(_stackProducer));
    rtos_createTask(TASK_SLOW,     slowReader, NULL, _stackSlow,     sizeof(_stackSlow));

    board_appTimerStart(APPL_IRQ_HZ);
    rtos_start();
    return 0;
}
