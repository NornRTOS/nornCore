/**
 * @file tc12_depth.c
 * Test case 12: the kernel's and the port's stack cost on top of a task that is deep in a
 * call chain when it is preempted, switched out or trapped, in four phases on four tasks
 * with a stack each, run one after the other, every phase at a call depth of 0, 4, 8, 16
 * and 32 frames:
 *
 *   A  recursion alone, inside a critical section: no interrupt, no switch. The natural
 *      cost of the chain (register window spills included on the ESP32); its slope is
 *      the frame size the other phases are measured against.
 *   B  recursion, then a spin at the bottom while the probe interrupt (2.5 kHz) and the
 *      tick land on the task; the probe wakes a higher task, so every probe is a switch
 *      out of the interrupt on this stack and back.
 *   C  recursion, then a blocking call at the bottom: the voluntary switch with every
 *      live window spilled.
 *   D  as B with floating point in the spin: the FPU trap after every switch back on the
 *      ESP32 (software floats, i.e. plain calls, on the AVR).
 *
 * For every phase and depth the high water mark is read and the chain's own frames are
 * subtracted; what remains is what the kernel and the port left under the chain, and it
 * must not grow with the depth: windows live at the interrupt are spilled into frames
 * the task already owns. Interrupts do not nest on either port (the AVR keeps them off
 * in an RTOS_ISR, the ESP32 port masks every level it uses), so a second source that
 * fires meanwhile runs after the first on the same spot; the probe and the tick coincide
 * often enough during the spins to include that case.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#define TASK_WAKE       6
#define TASK_D          5
#define TASK_C          4
#define TASK_B          3
#define TASK_A          2
#define TASK_SCRIPT     1

#define EVT_GO          0x1u
#define EVT_DONE        0x2u

#define IRQ_HZ          2500ul
#if !BOARD_APP_TIMER_HZ_IS_EXACT(IRQ_HZ)
# error IRQ_HZ is not an exact rate of the application timer
#endif

#define SPIN_MS         20u
#define NUM_DEPTHS      5u
#define MAX_DEPTH       32u
static const uint8_t _depths[NUM_DEPTHS] = { 0, 4, 8, 16, 32 };

/* Room for MAX_DEPTH frames of 64 bytes (a call8 frame with locals) plus the minimum. */
#if RTOS_PORT_MIN_STACK >= 512u
# define STACK_DEEP     (RTOS_PORT_MIN_STACK + 64u * (MAX_DEPTH + 8u))
# define STACK_OTHER    (RTOS_PORT_MIN_STACK + 64u)
#else
# define STACK_DEEP     (RTOS_PORT_MIN_STACK + 16u * (MAX_DEPTH + 8u))
# define STACK_OTHER    192u
#endif

enum { PHASE_A, PHASE_B, PHASE_C, PHASE_D, NUM_PHASES };

static uint8_t _stacks[NUM_PHASES][STACK_DEEP];
static uint8_t _stackWake[STACK_OTHER];
static uint8_t _stackScript[STACK_OTHER];

static rtos_sem_t _sem = RTOS_SEM_INIT(0);
static volatile uint32_t _woken, _irqs;
static volatile uint8_t _armed;
static volatile uint32_t _sink;
static volatile float _fsink;
static uint32_t _used[NUM_PHASES][NUM_DEPTHS];

RTOS_ISR(BOARD_APP_TIMER_VECT)
{
    ++ _irqs;
    if(_armed)
        rtos_notifyFromIsr(TASK_WAKE, 1, RTOS_NOTIFY_INC);
}

static void taskWake(void *arg)
{
    (void)arg;
    for(;;)
    {
        (void)rtos_notifyTake(RTOS_FOREVER);
        ++ _woken;
    }
}

/** What a phase does at the bottom of the chain. */
static void atBottom(uint8_t phase)
{
    switch(phase)
    {
    case PHASE_A:
        ++ _sink;                           /* nothing: back up at once */
        break;
    case PHASE_B:
    case PHASE_D:
    {
        const rtos_tick_t t0 = rtos_getTick();
        float f = 1.5f;
        while(rtos_getTick() - t0 < RTOS_MS(SPIN_MS))
        {
            ++ _sink;                       /* the probe and the tick land here */
            if(phase == PHASE_D)
                f = f * 1.0001f + 0.5f;     /* the FPU trap after every switch back */
        }
        _fsink = f;
        break;
    }
    default:
        (void)rtos_semTake(&_sem, 2);       /* switched out from here, windows spilled */
        break;
    }
}

/** Recursion with a value live across the call, so that no level is a tail call and
    every level owns a frame with a register the callee must preserve. */
static uint32_t deep(uint32_t n, uint8_t phase) __attribute__((noinline));
static uint32_t deep(uint32_t n, uint8_t phase)
{
    uint32_t r;
    if(n == 0)
    {
        atBottom(phase);
        return 1;
    }
    r = deep(n - 1, phase);
    _sink += r;
    return r + n;
}

static void taskPhase(void *arg)
{
    const uint8_t phase = (uint8_t)(uintptr_t)arg;
    const uint8_t me = (uint8_t)(TASK_A + phase);
    unsigned i;

    (void)rtos_waitEvents(EVT_GO, RTOS_WAIT_ANY, RTOS_FOREVER);
    _armed = (phase == PHASE_B  ||  phase == PHASE_D);
    for(i=0; i<NUM_DEPTHS; ++i)
    {
        if(phase == PHASE_A)
        {
            const rtos_port_cs_t cs = rtos_port_enterCritical();
            (void)deep(_depths[i], phase);
            rtos_port_exitCritical(cs);
        }
        else
            (void)deep(_depths[i], phase);
        _used[phase][i] = (uint32_t)(STACK_DEEP - rtos_getStackFree(me));
    }
    _armed = 0;
    rtos_postEvents(TASK_SCRIPT, EVT_DONE);
    for(;;)
        rtos_delay(RTOS_MS(100));
}

static void taskScript(void *arg)
{
    static const char * const name[NUM_PHASES] =
    {
        "A recursion only       ", "B + probe/tick preempt ", "C + block at bottom    ",
        "D + float, preempt     ",
    };
    unsigned p, i;
    uint32_t perFrame, overheadMax = 0;
    (void)arg;

    rtos_delay(RTOS_MS(10));
    for(p=0; p<NUM_PHASES; ++p)
    {
        rtos_postEvents((uint8_t)(TASK_A + p), EVT_GO);
        (void)rtos_waitEvents(EVT_DONE, RTOS_WAIT_ANY, RTOS_FOREVER);
    }
    rtos_delay(RTOS_MS(20));

    /* The chain's own cost per level from phase A's two largest depths. */
    perFrame = (_used[PHASE_A][NUM_DEPTHS - 1] - _used[PHASE_A][NUM_DEPTHS - 2])
               / (_depths[NUM_DEPTHS - 1] - _depths[NUM_DEPTHS - 2]);
    uart_puts("tc12 " BOARD_NAME " stack used (bytes) and kernel+port share at depth 0/4/8/16/32,"
              " frame per level ");
    uart_putU32(perFrame);
    uart_puts(", min stack ");
    uart_putU32(RTOS_PORT_MIN_STACK);
    for(p=0; p<NUM_PHASES; ++p)
    {
        uart_puts("\r\n  ");
        uart_puts(name[p]);
        uart_puts(" used");
        for(i=0; i<NUM_DEPTHS; ++i)
        {
            uart_putc(' ');
            uart_putU32(_used[p][i]);
        }
        if(p == PHASE_A)
        {
            /* No kernel share to show: the register file holds the first frames, so
               the chain costs nothing until it is deeper than the file (ESP32). */
            uart_puts("  (the chain alone)");
            continue;
        }
        uart_puts("  kernel+port");
        for(i=0; i<NUM_DEPTHS; ++i)
        {
            const uint32_t own = (uint32_t)_depths[i] * perFrame;
            const uint32_t overhead = _used[p][i] > own ? _used[p][i] - own : 0;
            if(overhead > overheadMax)
                overheadMax = overhead;
            uart_putc(' ');
            uart_putU32(overhead);
        }
    }
    uart_puts("\r\n  irqs ");
    uart_putU32(_irqs);
    uart_puts(" woken ");
    uart_putU32(_woken);
    uart_puts(", worst kernel+port ");
    uart_putU32(overheadMax);
    uart_puts(" (incl. the task's two bottom frames)\r\n");

    tc_expectRange("woken", _woken, 100, 0xFFFFFFFFul);
    tc_expectRange("perFrame", perFrame, 1, 128);
    tc_expectRange("overheadMax", overheadMax, 1, RTOS_PORT_MIN_STACK);
    tc_finish("tc12");
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
    uart_puts("\r\nnornOS " RTOS_VERSION " tc12_depth\r\n");
    rtos_init();
    rtos_createTask(TASK_WAKE,   taskWake,   NULL, _stackWake, sizeof(_stackWake));
    rtos_createTask(TASK_A, taskPhase, (void *)(uintptr_t)PHASE_A, _stacks[PHASE_A], STACK_DEEP);
    rtos_createTask(TASK_B, taskPhase, (void *)(uintptr_t)PHASE_B, _stacks[PHASE_B], STACK_DEEP);
    rtos_createTask(TASK_C, taskPhase, (void *)(uintptr_t)PHASE_C, _stacks[PHASE_C], STACK_DEEP);
    rtos_createTask(TASK_D, taskPhase, (void *)(uintptr_t)PHASE_D, _stacks[PHASE_D], STACK_DEEP);
    rtos_createTask(TASK_SCRIPT, taskScript, NULL, _stackScript, sizeof(_stackScript));
    board_appTimerStart(IRQ_HZ);
    rtos_start();
    return 0;
}
