/**
 * @file tc09_flash.c
 * Test case 9 (ESP32): code and constants in flash, run through the cache. The whole
 * object is placed in flash by FLASH_OBJS in app.mk, so main and the tasks run from the
 * instruction window and the tables are read through the data window, while the kernel,
 * the tick and the UART driver stay in IRAM. Checks: the mapping exists and covers the
 * image, the addresses are in the windows, a table in flash reads back right, a task in
 * flash is preempted by the tick like any other, interrupts arrive while a flash loop
 * runs, and a pass over the table costs a sane number of cycles. The RAM image is padded
 * past 64 KB so that the ROM loader's own use of the cache is covered too.
 */

#include "rtos.h"
#include "uart.h"
#include "port.h"
#include "testresult.h"

#define TASK_WORKER     1
#define TASK_TICKER     2
#define STACK_SIZE      (2u * RTOS_PORT_MIN_STACK)

static uint8_t _stackWorker[STACK_SIZE];
static uint8_t _stackTicker[STACK_SIZE];

/* 4 KB of constants: in flash with the rest of this object. */
#define TABLE_WORDS     1024u
static const uint32_t _table[TABLE_WORDS] = {
#define R4(i)   (i) * 2654435761ul, ((i) + 1) * 2654435761ul, ((i) + 2) * 2654435761ul, ((i) + 3) * 2654435761ul
#define R16(i)  R4(i), R4((i) + 4), R4((i) + 8), R4((i) + 12)
#define R64(i)  R16(i), R16((i) + 16), R16((i) + 32), R16((i) + 48)
#define R256(i) R64(i), R64((i) + 64), R64((i) + 128), R64((i) + 192)
    R256(0), R256(256), R256(512), R256(768)
};

/* A constant placed in flash by the attribute alone, from an object that would
   otherwise stay in RAM (the attribute is the point; this object is in flash anyway). */
static const uint32_t _marker RTOS_PORT_FLASH_RODATA = 0x5EC7F1A5ul;

static volatile uint32_t _tickerCount, _workerRounds;

static bool inRange(uint32_t a, uint32_t lo, uint32_t hi) { return a >= lo && a < hi; }

static uint32_t checksum(const uint32_t *p, uint32_t n)
{
    uint32_t s = 0, i;
    for(i=0; i<n; ++i)
        s = (s << 1 | s >> 31) ^ p[i];
    return s;
}

static void tickerTask(void *arg)
{
    (void)arg;
    for(;;)
    {
        rtos_waitPeriodic(RTOS_MS(10));
        ++ _tickerCount;
    }
}

static void workerTask(void *arg)
{
    uint32_t expected, i, t0, t1, cycles;
    const uint32_t addrMain = (uint32_t)&workerTask, addrTable = (uint32_t)_table;
    (void)arg;

    uart_puts("tc09: code from flash\r\n");
    tc_expect("flash pages mapped", rtos_port_flashPages() >= 1  &&  rtos_port_flashPages() <= 4);
    tc_expect("task code in the instruction window", inRange(addrMain, 0x400D0000ul, 0x40400000ul));
    tc_expect("table in the data window", inRange(addrTable, 0x3F400000ul, 0x3F800000ul));
    tc_expect("marker in the data window", inRange((uint32_t)&_marker, 0x3F400000ul, 0x3F800000ul));
    tc_expect("marker value", _marker == 0x5EC7F1A5ul);
    tc_expect("kernel in IRAM", inRange((uint32_t)&rtos_delay, 0x40080000ul, 0x400A0000ul));

    expected = 0;
    for(i=0; i<TABLE_WORDS; ++i)
        tc_expect("table word", _table[i] == i * 2654435761ul), expected = (expected << 1 | expected >> 31) ^ (i * 2654435761ul);
    tc_expect("table checksum", checksum(_table, TABLE_WORDS) == expected);

    /* Passes over the 4 KB table through the cache for at least 30 ms, the loop code in
       flash too and the table read through a volatile pointer so that every pass really
       reads it. On the board a pass costs a few cycles per word once the lines are
       cached; a miss on every word would take milliseconds per pass. */
    {
        const uint32_t * volatile pTable = _table;
        uint32_t passes = 0;
        t0 = rtos_port_ccount();
        do
        {
            tc_expect("checksum pass", checksum(pTable, TABLE_WORDS) == expected);
            ++ passes;
            t1 = rtos_port_ccount();
        }
        while(t1 - t0 < 30ul * (F_CPU / 1000ul));
        cycles = (t1 - t0) / passes;
        uart_puts("4 KB pass from flash: "); uart_putU32(cycles); uart_puts(" cycles, ");
        uart_putU32(passes); uart_puts(" passes\r\n");
        tc_expectRange("4 KB pass in 1 us..2 ms", cycles / (F_CPU / 1000000ul), 1, 2000);
    }

    /* The ticker ran meanwhile: the tick interrupt and the context switch work while
       the running task is in flash. */
    tc_expect("ticker preempted the flash loop", _tickerCount >= 1);
    _tickerCount = 0;
    rtos_delay(RTOS_MS(105));
    tc_expectRange("ticker rate", _tickerCount, 9, 11);

    uart_puts("RESULT tc09 ");
    uart_puts(tc_failures == 0 ? "PASS" : "FAIL");
    uart_puts("\r\n");
    uart_flush();
    rtos_port_halt();
}

extern const uint32_t tc09_pad[];

int main(void)
{
    uart_init(115200);
    if(*(volatile const uint32_t *)tc09_pad != 1)
        uart_puts("pad?\r\n");
    rtos_init();
    rtos_createTask(TASK_WORKER, workerTask, NULL, _stackWorker, sizeof(_stackWorker));
    rtos_createTask(TASK_TICKER, tickerTask, NULL, _stackTicker, sizeof(_stackTicker));
    rtos_start();
    return 0;
}

/* 60 KB of padding in IRAM: the RAM image then crosses the 64 KB flash boundary and the
   ROM loader maps flash through the cache while loading ("mmu set" in its log), which
   rtos_port_flashInit must undo before it maps ours. Read as a word: IRAM has no byte
   access. */
const uint32_t tc09_pad[15000] __attribute__((section(".iram1.tc09pad"), used)) = { 1 };
