/**
 * @file test_stream.c
 * Stream buffers (RTOS_CFG_STREAMS) on the host port. One child process per scenario, as
 * test_lifecycle does, because the last two end in a panic.
 *
 *   1. Wrap-around: for every ring size 1..64 and every trigger, bytes written in random
 *      chunk sizes with RTOS_NO_WAIT and read back in other chunk sizes come out in order
 *      and complete; count and space agree at every step.
 *   2. The trigger: T2 (reader, trigger 8) blocks; T1 writes 3, 4, then 1 byte: T2 wakes
 *      only at the eighth and reads all 8. A read above the level never waits; a read
 *      below the level with a timeout returns what is there when the time runs out, 0
 *      when nothing came.
 *   3. The writer blocks on a full stream and is woken as the reader makes room, the
 *      timeout counts from the call across several waits, a reset wakes a waiting writer
 *      and leaves a waiting reader waiting.
 *   4. A simulated interrupt writes into the stream while T2 reads: the reader is woken
 *      by the ISR tail, everything arrives in order, the ISR sees the full stream as a
 *      short write, and a simulated transmit interrupt drains a stream with ReadFromIsr.
 *   5. A second reader blocking on a stream with a waiting reader: RTOS_PANIC_ARGUMENT.
 *   6. A second writer blocking on a full stream with a waiting writer: RTOS_PANIC_ARGUMENT.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "rtos.h"
#include "rtos_kernel.h"

#define T1 1u
#define T2 2u
#define T3 3u

static uint8_t _stack1[1], _stack2[1], _stack3[1];
static int _scenario;
static uint32_t _idleLoops;
static uint8_t _ring[64];
static rtos_stream_t _s;
static volatile unsigned _t2Wakes, _t2Got, _phase;
static volatile unsigned _isrWrites, _isrShort, _isrBytes;

static void fail(const char *what)
{
    printf("scenario %d FAIL: %s\n", _scenario, what);
    exit(1);
}

static void pass(void)
{
    printf("scenario %d PASS\n", _scenario);
    exit(0);
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    const bool ok = (_scenario == 5  ||  _scenario == 6)  &&  pInfo->reason == RTOS_PANIC_ARGUMENT
                    &&  pInfo->arg1 == T3;
    printf("scenario %d %s: panic reason %u (%lu, %lu)\n", _scenario, ok ? "PASS" : "FAIL",
           pInfo->reason, (unsigned long)pInfo->arg1, (unsigned long)pInfo->arg2);
    exit(ok ? 0 : 1);
}

/* Simulated interrupts of scenario 4. */
static uint8_t _isrNext = 0;
static void isrProducer(void)
{
    uint8_t burst[5];
    uint16_t i, n;
    for(i = 0; i < 5; ++i)
        burst[i] = (uint8_t)(_isrNext + i);
    n = rtos_streamWriteFromIsr(&_s, burst, 5);
    ++ _isrWrites;
    _isrBytes += n;
    _isrNext = (uint8_t)(_isrNext + n);
    if(n < 5)
        ++ _isrShort;
}

static uint8_t _txOut[16];
static uint16_t _txGot;
static void isrDrain(void)
{
    _txGot = rtos_streamReadFromIsr(&_s, _txOut, sizeof _txOut);
}

void test_idleHook(void)
{
    if(++_idleLoops > 200000ul)
        fail("runaway");
    if(_scenario == 4  &&  _phase == 1  &&  (_idleLoops % 3u) == 0)
        rtos_host_simulateIsr(isrProducer);
    rtos_host_tick();
}

/* ---- scenario 1: wrap-around, single task, no waiting ----------------------------------- */

static uint32_t _seed = 12345;
static unsigned rnd(unsigned n)
{
    _seed = (uint32_t)(_seed * 1103515245ul + 12345ul);
    return (unsigned)((_seed >> 16) % n);
}

static void wrapAround(void)
{
    uint16_t size;
    for(size = 1; size <= 64; ++size)
    {
        uint16_t trigger;
        for(trigger = 1; trigger <= size; trigger = (uint16_t)(trigger * 2u + 1u))
        {
            uint8_t next = 0, expect = 0;
            unsigned total = 0, written = 0, read = 0;
            uint8_t tmp[70];
            rtos_streamInit(&_s, _ring, size, trigger > size ? size : trigger);
            while(read < 1000)
            {
                unsigned i, n;
                if(written < 1000  &&  (rnd(2) == 0  ||  rtos_streamCount(&_s) == 0))
                {
                    n = 1 + rnd(size + 5);
                    for(i = 0; i < n; ++i)
                        tmp[i] = (uint8_t)(next + i);
                    n = rtos_streamWrite(&_s, tmp, (uint16_t)n, RTOS_NO_WAIT);
                    next = (uint8_t)(next + n);
                    written += n;
                    total += n;
                }
                else
                {
                    n = 1 + rnd(size + 5);
                    n = rtos_streamRead(&_s, tmp, (uint16_t)n, RTOS_NO_WAIT);
                    for(i = 0; i < n; ++i)
                        if(tmp[i] != (uint8_t)(expect + i))
                            fail("bytes out of order");
                    expect = (uint8_t)(expect + n);
                    read += n;
                    total -= n;
                }
                if(rtos_streamCount(&_s) != total  ||  rtos_streamSpace(&_s) != size - total)
                    fail("count or space wrong");
                if(total > size)
                    fail("more than the size stored");
            }
        }
    }
}

/* ---- tasks ------------------------------------------------------------------------------- */

static void expectRead(uint16_t n, rtos_tick_t timeout, uint16_t want, uint8_t first, const char *what)
{
    uint8_t buf[64];
    uint16_t got = rtos_streamRead(&_s, buf, n, timeout), i;
    if(got != want)
    {
        printf("  %s: got %u, want %u\n", what, got, want);
        fail("read length");
    }
    for(i = 0; i < got; ++i)
        if(buf[i] != (uint8_t)(first + i))
            fail("read content");
}

static void task2(void *arg)            /* the reader, higher than T1 */
{
    (void)arg;
    switch(_scenario)
    {
    case 2:
        ++ _t2Wakes;
        expectRead(64, RTOS_FOREVER, 8, 0, "trigger read");   /* wakes at the eighth byte */
        ++ _t2Wakes;
        _t2Got = 8;
        expectRead(64, RTOS_FOREVER, 16, 8, "second read");  /* T1 wrote 16 at once */
        expectRead(64, 5, 0, 0, "timeout, nothing");
        rtos_delay(1);
        expectRead(64, 5, 3, 24, "timeout, below the level");  /* T1 writes 3 then stops */
        rtos_delay(7);                              /* T1 writes 10 meanwhile */
        /* Above the level: no wait at all. */
        {
            const rtos_tick_t before = rtos_getTick();
            expectRead(2, RTOS_FOREVER, 2, 27, "part of a bigger fill");  /* T1 wrote 10 */
            expectRead(64, RTOS_FOREVER, 8, 29, "the rest");
            if(rtos_getTick() != before)
                fail("a read above the level waited");
        }
        pass();
        break;

    case 3:
        rtos_delay(2);                              /* let T1 fill the stream and block */
        expectRead(4, RTOS_NO_WAIT, 4, 0, "make room 1");   /* T1 wakes, writes 4 more */
        rtos_delay(1);
        expectRead(4, RTOS_NO_WAIT, 4, 4, "make room 2");
        rtos_delay(1);
        if(rtos_streamCount(&_s) != 16)
            fail("the writer did not fill the room");
        _phase = 1;
        rtos_delay(20);                             /* T1 waits with a 10 tick timeout, times out */
        if(_phase != 2)
            fail("the writer's timeout did not end the write");
        rtos_streamReset(&_s);                      /* 16 bytes dropped; T1 is blocked again: woken */
        rtos_delay(2);
        if(_phase != 3)
            fail("the reset did not wake the writer");
        /* A waiting reader stays waiting through a reset. */
        rtos_streamReset(&_s);
        _phase = 4;
        expectRead(64, RTOS_FOREVER, 16, 32, "after the reset");   /* T1 resets, then writes */
        pass();
        break;

    case 4:
        _phase = 1;
        {
            uint8_t expect = 0;
            unsigned got = 0, reads = 0;
            while(got < 400)
            {
                uint8_t buf[64];
                uint16_t n = rtos_streamRead(&_s, buf, sizeof buf, RTOS_FOREVER), i;
                ++ reads;
                for(i = 0; i < n; ++i)
                    if(buf[i] != (uint8_t)(expect + i))
                        fail("ISR bytes out of order");
                expect = (uint8_t)(expect + n);
                got += n;
                if(reads == 3)
                    rtos_delay(60);                 /* let the ISR fill the 64 bytes: short writes */
            }
            _phase = 0;
            if(_isrShort == 0)
                fail("the full stream never shortened an ISR write");
            if(_isrBytes < got)
                fail("more read than written");
        }
        /* Drain by a transmit interrupt. */
        rtos_streamReset(&_s);
        {
            uint8_t data[10] = {1,2,3,4,5,6,7,8,9,10};
            (void)rtos_streamWrite(&_s, data, 10, RTOS_NO_WAIT);
        }
        rtos_host_simulateIsr(isrDrain);
        if(_txGot != 10  ||  _txOut[0] != 1  ||  _txOut[9] != 10  ||  rtos_streamCount(&_s) != 0)
            fail("ReadFromIsr");
        rtos_host_simulateIsr(isrDrain);
        if(_txGot != 0)
            fail("ReadFromIsr on an empty stream");
        pass();
        break;

    case 5:
        expectRead(64, RTOS_FOREVER, 1, 0, "first reader");   /* blocks; T3 then blocks too: panic */
        break;

    case 6:
        {
            uint8_t data[64] = {0};
            (void)rtos_streamWrite(&_s, data, 64, RTOS_NO_WAIT);  /* full */
            (void)rtos_streamWrite(&_s, data, 1, RTOS_FOREVER);   /* blocks; T3 then blocks too */
        }
        break;

    default:
        fail("unknown scenario for T2");
    }
}

static void task3(void *arg)            /* the second reader or writer of scenarios 5 and 6 */
{
    uint8_t buf[1] = {0};
    (void)arg;
    rtos_delay(1);
    if(_scenario == 5)
        (void)rtos_streamRead(&_s, buf, 1, RTOS_FOREVER);
    else
        (void)rtos_streamWrite(&_s, buf, 1, RTOS_FOREVER);
    fail("the second waiter was not refused");
}

static void expectWrite(uint8_t first, uint16_t n, rtos_tick_t timeout, uint16_t want, const char *what)
{
    uint8_t buf[64];
    uint16_t i, got;
    for(i = 0; i < n; ++i)
        buf[i] = (uint8_t)(first + i);
    got = rtos_streamWrite(&_s, buf, n, timeout);
    if(got != want)
    {
        printf("  %s: wrote %u, want %u\n", what, got, want);
        fail("write length");
    }
}

static void task1(void *arg)            /* the writer, lower than T2 */
{
    (void)arg;
    switch(_scenario)
    {
    case 2:
        expectWrite(0, 3, RTOS_NO_WAIT, 3, "3");
        if(_t2Wakes != 1) fail("reader woke below the level (3)");
        expectWrite(3, 4, RTOS_NO_WAIT, 4, "4");
        if(_t2Wakes != 1) fail("reader woke below the level (7)");
        expectWrite(7, 1, RTOS_NO_WAIT, 1, "the eighth");
        if(_t2Wakes != 2  ||  _t2Got != 8) fail("reader did not wake at the level");
        expectWrite(8, 16, RTOS_NO_WAIT, 16, "16 at once");
        rtos_delay(8);                              /* T2's empty timeout runs out */
        expectWrite(24, 3, RTOS_NO_WAIT, 3, "3 below the level");
        rtos_delay(8);                              /* T2 times out and takes the 3 */
        expectWrite(27, 10, RTOS_NO_WAIT, 10, "10 above the level");
        rtos_delay(RTOS_FOREVER);
        break;

    case 3:
        expectWrite(0, 16, RTOS_NO_WAIT, 16, "fill");              /* size 16 here */
        expectWrite(16, 8, RTOS_FOREVER, 8, "blocked twice for 4");  /* woken by two reads of 4 */
        while(_phase == 0)
            rtos_delay(1);
        {
            const rtos_tick_t t0 = rtos_getTick();
            expectWrite(24, 8, 10, 0, "timeout on a full stream");
            if(rtos_getTick() - t0 < 10  ||  rtos_getTick() - t0 > 12)
                fail("the writer's timeout is off");
        }
        _phase = 2;
        expectWrite(24, 8, RTOS_FOREVER, 8, "woken by the reset");  /* T2 resets: room for 16 */
        _phase = 3;
        while(_phase != 4)
            rtos_delay(1);
        rtos_delay(2);                                        /* T2 waits now; the reset left it waiting */
        rtos_streamReset(&_s);
        expectWrite(32, 16, RTOS_NO_WAIT, 16, "after the reset");
        rtos_delay(RTOS_FOREVER);
        break;

    default:
        rtos_delay(RTOS_FOREVER);
    }
}

static int runScenario(int n)
{
    _scenario = n;
    rtos_init();
    if(n == 1)
    {
        wrapAround();
        pass();
    }
    rtos_streamInit(&_s, _ring, n == 3 ? 16 : 64, n == 2 ? 8 : 1);
    rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
    rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
    if(n >= 5)
        rtos_createTask(T3, task3, NULL, _stack3, sizeof(_stack3));
    rtos_start();
    return 1;
}

#define NUM_SCENARIOS 6

int main(int argc, char **argv)
{
    int n, failures = 0;

    if(argc > 1)
        return runScenario(atoi(argv[1]));
    for(n=1; n<=NUM_SCENARIOS; ++n)
    {
        char num[8];
        int status = 0;
        pid_t pid;
        snprintf(num, sizeof(num), "%d", n);
        fflush(stdout);
        pid = fork();
        if(pid == 0)
        {
            execl(argv[0], argv[0], num, (char *)NULL);
            _exit(99);
        }
        waitpid(pid, &status, 0);
        if(!WIFEXITED(status)  ||  WEXITSTATUS(status) != 0)
        {
            printf("scenario %d: exit status %d\n", n, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            ++ failures;
        }
    }
    printf("%s: test_stream (%d scenarios, %d failures)\n", failures == 0 ? "PASS" : "FAIL",
           NUM_SCENARIOS, failures);
    return failures == 0 ? 0 : 1;
}
