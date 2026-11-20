/**
 * @file tc18_guard.c
 * Test case 18: the hardware stack guard (RTOS_CFG_STACK_GUARD) on the ESP32. The guard is
 * data breakpoint 0 on the lowest 32 bytes of the running task's stack, armed at every
 * context switch; the first store into them must be RTOS_PANIC_STACK_OVERFLOW at that
 * instruction, not at the next switch or tick as with the canary.
 *
 * Two tasks. The watcher (1) writes into the bottom of the victim's stack while the victim
 * sleeps: not caught, since the guard follows the running task, which shows the re-arm at
 * the switch. The victim (2) then writes just above its own window (not caught: a store
 * into the task's own unused stack is legal) and then into the window, with a flag set
 * right after the store. The panic hook is the report: the reason, the task, a non zero
 * PC in arg2 and the flag still clear, which proves the panic came from the store itself.
 * Had the guard not fired, the victim finishes the test as a failure after a tick.
 *
 *     RESULT tc18 PASS
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_WATCHER    1
#define TASK_VICTIM     2
#define STACK_SIZE      1024

RTOS_STACK(_stackVictim, STACK_SIZE);           /* file scope: the victim addresses its own bottom */
RTOS_STACK(_stackWatcher, STACK_SIZE);

static volatile uint8_t _afterStore = 0;        /* set by the instruction after the guarded store */
static volatile uint8_t _watcherWrote = 0;


void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    uint8_t i;
    uart_puts("tc18: panic reason=");  uart_putU32(pInfo->reason);
    uart_puts(" task=");               uart_putU32(pInfo->task);
    uart_puts(" arg1=");               uart_putU32(pInfo->arg1);
    uart_puts(" pc=0x");
    for(i=0; i<4; ++i)
        uart_putHex8((uint8_t)(pInfo->arg2 >> (24 - 8*i)));
    uart_puts("\r\n");
    tc_expect("reason is RTOS_PANIC_STACK_OVERFLOW", pInfo->reason == RTOS_PANIC_STACK_OVERFLOW);
    tc_expect("the victim is named", pInfo->arg1 == TASK_VICTIM);
    tc_expect("the PC of the store is reported", pInfo->arg2 != 0);
    tc_expect("the watcher's write into the victim's bottom was not caught", _watcherWrote == 1);
    tc_expect("caught at the store, not later", _afterStore == 0);
    tc_finish("tc18");
}


static void watcher(void *arg)
{
    (void)arg;
    rtos_delay(RTOS_MS(2));                     /* the victim has run and sleeps */
    *(volatile uint32_t *)(_stackVictim + 16) = 0x12345678ul;   /* another task's window: not ours */
    _watcherWrote = 1;
    for(;;)
        rtos_delay(RTOS_MS(100));
}


static void victim(void *arg)
{
    volatile uint32_t *p;
    (void)arg;
    uart_puts("tc18 ESP32 stack guard: 32 byte window at the bottom of the running task's stack\r\n");
    rtos_delay(RTOS_MS(5));                     /* let the watcher write into our bottom */
    tc_expect("watcher wrote while we slept", _watcherWrote == 1);

    p = (volatile uint32_t *)(_stackVictim + 32);
    *p = 0xA5A5A5A5ul;                          /* just above the window: legal, our own stack */
    uart_puts("tc18: store above the window passed\r\n");

    p = (volatile uint32_t *)(_stackVictim + 16);
    *p = 0xDEADBEEFul;                          /* into the window: the guard fires here */
    _afterStore = 1;                            /* never reached when the guard works */

    rtos_delay(RTOS_MS(5));                     /* a switch and ticks: the canary is intact (offset 16) */
    tc_expect("the guard fired", false);
    tc_finish("tc18");
}


int main(void)
{
    uart_init(UART_BAUD_DEFAULT);
    uart_puts("\r\nnornOS " RTOS_VERSION " tc18_guard\r\n");
    rtos_init();
    rtos_createTask(TASK_WATCHER, watcher, NULL, _stackWatcher, STACK_SIZE);
    rtos_createTask(TASK_VICTIM, victim, NULL, _stackVictim, STACK_SIZE);
    rtos_start();
    return 0;
}
