/**
 * @file tc07_amp.c
 * Test case 7: two nornOS instances, one per ESP32 core. Core 0 (this image) loads and
 * starts core 1 (core1/), then sends it 200 requests per second over the shared memory
 * link; core 1 answers each one. Core 0 checks every answer, the order, and that core 1's
 * own tick advances, and measures the round trip.
 *
 * Phase 2, two receivers on one core (audit F4): receivers A and B both wait in
 * amp_receive on an empty ring. The sender asks core 1 for a burst of two replies and
 * keeps interrupts masked until both are in the ring, so the two cross-core signals
 * merge into one interrupt and one semaphore give. B (the higher one) gets the first
 * reply and then stays busy for 30 ms; the second reply must reach A at once, passed on
 * by amp_receive because the ring was not empty after B's take. Checked 10 ms after each
 * burst: both replies handled, nothing left in the ring.
 */

#include "rtos.h"
#include "uart.h"
#include "board.h"
#include "amp.h"
#include "testresult.h"

#define TASK_SENDER     3
#define TASK_RECV_B     2
#define TASK_RECV_A     1

#ifndef TC07_STOP_AFTER_MS
# define TC07_STOP_AFTER_MS  3000ul
#endif
#define BURSTS          20u
#define BURST_MARK      0xB0000000ul        /* w[0] of a burst request and its replies */

#define STACK_SIZE      (2u * RTOS_PORT_MIN_STACK)

static uint8_t _stackSender[STACK_SIZE];
static uint8_t _stackRecvA[STACK_SIZE];
static uint8_t _stackRecvB[STACK_SIZE];

static volatile uint32_t _sent = 0, _received = 0, _errors = 0, _sendFull = 0;
static volatile uint32_t _core1Beats = 0, _core1Tick = 0;
static volatile uint32_t _rttSum = 0, _rttMax = 0;
static volatile uint32_t _sentAt[256];
static volatile uint32_t _expectSeq = 1;
static volatile uint32_t _burstByA = 0, _burstByB = 0, _burstLate = 0, _burstSpin = 0;
static rtos_sem_t _phase2 = RTOS_SEM_INIT_BINARY(0);


/* Both receivers: a reply of phase 1 (only A gets those) or of a burst. */
static void handle(const amp_msg_t *pR, uint8_t task)
{
    if((pR->w[0] & 0xF0000000ul) == BURST_MARK)
    {
        if(task == TASK_RECV_A) ++ _burstByA; else ++ _burstByB;
        return;
    }
    {
        const uint32_t seq = pR->w[0];
        const uint32_t rtt = rtos_port_ccount() - _sentAt[seq % 256u];
        ++ _received;
        if(seq != _expectSeq  ||  pR->w[1] != (seq % 1000u) * (7u + seq % 13u) + seq)
            ++ _errors;
        _expectSeq = seq + 1;
        _core1Beats = pR->w[2];
        _core1Tick = pR->w[3];
        _rttSum += rtt;
        if(rtt > _rttMax)
            _rttMax = rtt;
    }
}

static bool blockedInReceive(uint8_t task)
{
    rtos_taskInfo_t info;
    (void)rtos_getTaskInfo(task, &info);
    return info.state == RTOS_TASK_BLOCKED  &&  info.waitKind == RTOS_WAIT_SEM;
}

static void burstPhase(void)
{
    unsigned i;

    rtos_semGive(&_phase2);                 /* B joins */
    for(i=0; i<BURSTS; ++i)
    {
        amp_msg_t m;
        uint32_t before, t0;

        while(!blockedInReceive(TASK_RECV_A)  ||  !blockedInReceive(TASK_RECV_B))
            rtos_delay(1);
        before = _burstByA + _burstByB;
        m.w[0] = BURST_MARK | i;
        m.w[1] = m.w[2] = m.w[3] = 0;
        {
            /* Masked until both replies are in the ring: one interrupt for two. */
            const rtos_port_cs_t cs = rtos_port_enterCritical();
            (void)amp_send(&m);
            t0 = rtos_port_ccount();
            while(amp_pending() < 2u  &&  rtos_port_ccount() - t0 < F_CPU / 500u)
                ;
            if(amp_pending() < 2u)
                ++ _burstSpin;
            rtos_port_exitCritical(cs);
        }
        rtos_delay(RTOS_MS(10));
        if(_burstByA + _burstByB - before != 2u  ||  amp_pending() != 0u)
            ++ _burstLate;
        rtos_delay(RTOS_MS(40));            /* B is busy for 30 ms after its reply */
    }
}

static void taskSender(void *arg)
{
    uint32_t seq = 0;
    (void)arg;
    while(rtos_getTick() < RTOS_MS(TC07_STOP_AFTER_MS))
    {
        amp_msg_t m;
        (void)rtos_waitPeriodic(RTOS_MS(5));
        ++ seq;
        m.w[0] = seq;
        m.w[1] = seq % 1000u;
        m.w[2] = 7u + seq % 13u;
        m.w[3] = 0;
        _sentAt[seq % 256u] = rtos_port_ccount();
        if(amp_send(&m))
            ++ _sent;
        else
            ++ _sendFull;
    }
    rtos_delay(RTOS_MS(20));                /* the last answer */
    burstPhase();

    {
        const uint32_t n = _received ? _received : 1;
        uart_puts("tc07 sent=");          uart_putU32(_sent);
        uart_puts(" received=");          uart_putU32(_received);
        uart_puts(" errors=");            uart_putU32(_errors);
        uart_puts(" full=");              uart_putU32(_sendFull);
        uart_puts(" core1Tick=");         uart_putU32(_core1Tick);
        uart_puts(" core1Beats=");        uart_putU32(_core1Beats);
        uart_puts(" rtt avg=");           uart_putU32(_rttSum / n / (uint32_t)(F_CPU / 1000000ul));
        uart_puts(" us max=");            uart_putU32(_rttMax / (uint32_t)(F_CPU / 1000000ul));
        uart_puts(" us\r\n");
        uart_puts("tc07 bursts=");        uart_putU32(BURSTS);
        uart_puts(" byA=");               uart_putU32(_burstByA);
        uart_puts(" byB=");               uart_putU32(_burstByB);
        uart_puts(" late=");              uart_putU32(_burstLate);
        uart_puts(" spinTimeouts=");      uart_putU32(_burstSpin);
        uart_puts("\r\n");
        tc_expectNear("sent", _sent, TC07_STOP_AFTER_MS / 5);
        tc_expectRange("received == sent", _sent - _received, 0, 0);
        tc_expectRange("errors", _errors, 0, 0);
        tc_expectRange("full", _sendFull, 0, 0);
        tc_expectNear("core1Tick", _core1Tick, TC07_STOP_AFTER_MS);
        tc_expectNear("core1Beats", _core1Beats, TC07_STOP_AFTER_MS / 10);
        tc_expectRange("burst spin timeouts", _burstSpin, 0, 0);
        tc_expectRange("burst replies to B", _burstByB, BURSTS, BURSTS);
        tc_expectRange("burst replies to A (passed on)", _burstByA, BURSTS, BURSTS);
        tc_expectRange("bursts not handled within 10 ms", _burstLate, 0, 0);
        tc_finish("tc07");
    }
}

static void taskRecvA(void *arg)
{
    (void)arg;
    for(;;)
    {
        amp_msg_t r;
        if(amp_receive(&r, RTOS_FOREVER))
            handle(&r, TASK_RECV_A);
    }
}

static void taskRecvB(void *arg)
{
    (void)arg;
    (void)rtos_semTake(&_phase2, RTOS_FOREVER);
    for(;;)
    {
        amp_msg_t r;
        if(amp_receive(&r, RTOS_FOREVER))
        {
            handle(&r, TASK_RECV_B);
            rtos_delay(RTOS_MS(30));        /* busy: the other reply must not wait for us */
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
    uart_init(115200);
    uart_puts("\r\nnornOS " RTOS_VERSION " tc07_amp: core 1 image ");
    uart_putU32(amp_core1_image_size);
    uart_puts(" bytes\r\n");

    rtos_init();
    rtos_createTask(TASK_SENDER, taskSender, NULL, _stackSender, sizeof(_stackSender));
    rtos_createTask(TASK_RECV_B, taskRecvB,  NULL, _stackRecvB,  sizeof(_stackRecvB));
    rtos_createTask(TASK_RECV_A, taskRecvA,  NULL, _stackRecvA,  sizeof(_stackRecvA));
    amp_init();                         /* starts core 1 */
    rtos_start();
    return 0;
}
