/**
 * @file tc11_stack.c
 * Test case 11: how much stack the kernel and the port need per task, measured with the
 * fill pattern (high water mark) on tasks that do nothing but one thing each:
 *
 *   block    the highest task waits on a semaphore that the script gives it 1000 times:
 *            the cost of a blocking call and the voluntary context switch, nothing else
 *            (its own frame is one call level with no locals).
 *   wake     woken from the application timer interrupt by a notification 1000 times:
 *            the blocking path plus whatever the wake-up leaves on this stack.
 *   busy     the lowest task spins; the interrupts land on its stack, and when the
 *            interrupt wakes "wake" the switch happens from the interrupt: the cost of an
 *            interrupt frame, the dispatch, the handler and the switch out of it.
 *   script   gives the semaphore, then prints; its figure includes the UART driver.
 *
 * The numbers are what RTOS_PORT_MIN_STACK has to cover (block or wake, plus busy for a
 * task that can be interrupted, which is every task) and go into the README per board.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_BLOCK      4
#define TASK_WAKE       3
#define TASK_SCRIPT     2
#define TASK_BUSY       1

#define IRQ_HZ          1000ul
#if !BOARD_APP_TIMER_HZ_IS_EXACT(IRQ_HZ)
# error IRQ_HZ is not an exact rate of the application timer
#endif

#define ROUNDS          1000u
#define STACK_SIZE      (2u * RTOS_PORT_MIN_STACK)

static uint8_t _stackBlock[STACK_SIZE];
static uint8_t _stackWake[STACK_SIZE];
static uint8_t _stackScript[STACK_SIZE];
static uint8_t _stackBusy[STACK_SIZE];

static rtos_sem_t _sem = RTOS_SEM_INIT(0);
static volatile uint32_t _blocked, _woken, _irqs, _spins;
static volatile uint8_t _armed;

RTOS_ISR(BOARD_APP_TIMER_VECT)
{
    ++ _irqs;
    if(_armed  &&  _woken < ROUNDS)
        rtos_notifyFromIsr(TASK_WAKE, 1, RTOS_NOTIFY_INC);
}

static void taskBlock(void *arg)
{
    (void)arg;
    for(;;)
    {
        (void)rtos_semTake(&_sem, RTOS_FOREVER);
        ++ _blocked;
    }
}

static void taskWake(void *arg)
{
    (void)arg;
    _armed = 1;
    for(;;)
    {
        (void)rtos_notifyTake(RTOS_FOREVER);
        ++ _woken;
    }
}

static void taskBusy(void *arg)
{
    (void)arg;
    for(;;)
        ++ _spins;
}

static void putUsed(const char *name, uint8_t task)
{
    uart_puts(name);
    uart_putU32((uint32_t)(STACK_SIZE - rtos_getStackFree(task)));
}

static void taskScript(void *arg)
{
    unsigned i;
    (void)arg;
    rtos_delay(RTOS_MS(20));
    for(i=0; i<ROUNDS; ++i)
        rtos_semGive(&_sem);
    while(_woken < ROUNDS)
        rtos_delay(RTOS_MS(10));
    rtos_delay(RTOS_MS(50));

    uart_puts("tc11 " BOARD_NAME " stack used (bytes) of ");
    uart_putU32(STACK_SIZE);
    uart_puts(", min stack ");
    uart_putU32(RTOS_PORT_MIN_STACK);
    putUsed("\r\n  block  ", TASK_BLOCK);
    putUsed("\r\n  wake   ", TASK_WAKE);
    putUsed("\r\n  busy   ", TASK_BUSY);
    putUsed("\r\n  script ", TASK_SCRIPT);
    uart_puts("\r\n  irqs ");
    uart_putU32(_irqs);
    uart_puts(" spins ");
    uart_putU32(_spins);
    uart_puts("\r\n");

#ifdef TC11_DUMP
    /* The used part of the block task's stack, top down, 8 words per line: the saved
       return addresses tell which frames the blocking path leaves on the stack. */
    {
        const uint32_t *top = (const uint32_t *)(_stackBlock + STACK_SIZE);
        const uint32_t used = (uint32_t)(STACK_SIZE - rtos_getStackFree(TASK_BLOCK));
        uint32_t i;
        for(i=0; i<(used + 3) / 4; ++i)
        {
            if(i % 8 == 0) { uart_puts("\r\n  -"); uart_putU32((i + 8) * 4); uart_puts(": "); }
            {
                const uint32_t w = top[-(int32_t)i - 1];
                uart_putHex8((uint8_t)(w >> 24)); uart_putHex8((uint8_t)(w >> 16));
                uart_putHex8((uint8_t)(w >> 8));  uart_putHex8((uint8_t)w);
                uart_putc(' ');
            }
        }
        uart_puts("\r\n");
    }
#endif
    tc_expectRange("blocked", _blocked, ROUNDS, ROUNDS);
    tc_expectRange("woken", _woken, ROUNDS, ROUNDS);
    tc_expectRange("irqs", _irqs, ROUNDS, 0xFFFFFFFFul);
    tc_expectRange("busyUsed", STACK_SIZE - rtos_getStackFree(TASK_BUSY), 1, RTOS_PORT_MIN_STACK);
    tc_expectRange("blockUsed", STACK_SIZE - rtos_getStackFree(TASK_BLOCK), 1, RTOS_PORT_MIN_STACK);
    tc_expectRange("wakeUsed", STACK_SIZE - rtos_getStackFree(TASK_WAKE), 1, RTOS_PORT_MIN_STACK);
    tc_finish("tc11");
}

/** The stack monitor's warning, printed here as the applications are meant to do it. The
    stacks of this test are twice the minimum, so it stays silent unless the port's
    figures are off; tc_finish would then still report the high water marks. */
void rtos_stackWarnHook(uint8_t task, size_t stackFree)
{
    uart_puts("tc11: stack warning, task ");
    uart_putU32(task);
    uart_puts(" has ");
    uart_putU32((uint32_t)stackFree);
    uart_puts(" bytes left\r\n");
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
    uart_init(115200);
    uart_puts("\r\nnornOS " RTOS_VERSION " tc11_stack\r\n");
    rtos_init();
    rtos_createTask(TASK_BLOCK,  taskBlock,  NULL, _stackBlock,  sizeof(_stackBlock));
    rtos_createTask(TASK_WAKE,   taskWake,   NULL, _stackWake,   sizeof(_stackWake));
    rtos_createTask(TASK_SCRIPT, taskScript, NULL, _stackScript, sizeof(_stackScript));
    rtos_createTask(TASK_BUSY,   taskBusy,   NULL, _stackBusy,   sizeof(_stackBusy));
    board_appTimerStart(IRQ_HZ);
    rtos_start();
    return 0;
}
