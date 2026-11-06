/**
 * @file test_prio.c
 * The scheduler against mutex ceilings (audit finding F2): the ready mask is indexed by
 * base priority, so a task that is boosted by a ceiling must still be chosen by its
 * effective priority wherever the next task is picked: in the scheduler called from
 * task context, in the interrupt tail, and at a mutex hand-over. Three scenarios, trace
 * based like test_kernel.
 *
 *   1. The owner blocks while holding (allowed, discouraged): T1 locks M (ceiling 4),
 *      delays one tick; T2 computes without blocking while ticks arrive from a simulated
 *      interrupt. The tick that wakes T1 must switch to it (eff 4 > 2) at once.
 *   2. Hand-over: T2 blocks on M while T1 holds it; T3 (base 3, below the ceiling)
 *      computes when T1 unlocks. The new owner T2 runs at 4 before T3 gets on.
 *   3. Nested ceilings: T1 holds M (4) and N (ceiling 3) in LIFO order; unlocking N
 *      must leave T1 at 4 (still boosted), unlocking M drops it to 1 and T3 preempts.
 *   4. No blocking while holding at all: T1 holds N (3) and computes; T4 (base 4, above
 *      the ceiling) preempts it, readies T2 (base 2) with an event and blocks. The owner
 *      at 3 must get back before T2, although T2's ready bit is the highest base one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtos.h"
#include "rtos_kernel.h"

#define T1 1
#define T2 2
#define T3 3
#define T4 4

static uint8_t _stack1[1], _stack2[1], _stack3[1], _stack4[1];
static rtos_mutex_t _m = RTOS_MUTEX_INIT(4);
static rtos_mutex_t _n = RTOS_MUTEX_INIT(3);
static uint32_t _idleLoops;
static volatile uint8_t _step;      /* moved on by T1 (and T2 in scenario 2) */
static volatile bool _t4Done;
static bool _done;

typedef struct entry { uint8_t task; const char *tag; long value; } entry_t;
static entry_t _trace[64];
static uint8_t _n_;

static void trace(uint8_t task, const char *tag, long value)
{
    if(_n_ < 64)
    {
        _trace[_n_].task = task; _trace[_n_].tag = tag; _trace[_n_].value = value;
        ++ _n_;
    }
}

static const entry_t _expected[] =
{
    /* 1: the owner blocked at the ceiling is scheduled by its effective priority */
    { T1, "lockM", 4 },                 /* effPrio after the lock */
    { T2, "computing", 0 },             /* T2 computes while T1 sleeps one tick */
    { T1, "wokeAtCeiling", 1 },         /* the tick switched to T1 in the middle of T2's
                                           loop: T2 had not finished (step still 1) */
    { T2, "computeDone", 2 },           /* T2 got back after T1 dropped the ceiling */
    { T1, "unlockM", 1 },
    /* 2: hand-over to a waiter raised to the ceiling beats a task in between */
    { T1, "lockM2", 4 },
    { T2, "lockM2blocked", 0 },         /* T2 waits: the owner holds it */
    { T3, "computing", 0 },
    { T2, "gotM", 4 },                  /* T1 woke (at 4), handed over: T2 at 4 runs
                                           before T3 (3) continues computing */
    { T3, "computeDone", 1 },           /* T3 got back once T2 dropped to 2 */
    { T2, "unlockM2", 2 },
    { T1, "unlockM2", 1 },
    /* 3: nested ceilings in LIFO order */
    { T1, "lockM3", 4 },
    { T1, "lockN3", 4 },                /* N's ceiling 3 is below: unchanged */
    { T3, "spinning", 0 },
    { T1, "unlockN3", 4 },              /* still boosted by M, T3 does not get in */
    { T3, "ranAfterDrop", 1 },          /* T3 (base 3) preempted T1 as soon as M dropped */
    { T1, "unlockM3", 1 },
    /* 4: owner preempted from above, a task between base and ceiling readied meanwhile */
    { T1, "lockN4", 3 },
    { T4, "preempts", 0 },              /* T4 (4) above the ceiling may preempt */
    { T1, "ranBeforeT2", 1 },           /* T4 blocked: the owner at 3 beats T2's bit 2 */
    { T2, "gotEvent", 41 },             /* T2 only ran after the unlock (step 41) */
    { T1, "unlockN4", 1 },
    { T1, "end", 0 },
};

static void verify(void)
{
    uint8_t i, failures = 0;
    const uint8_t expected = (uint8_t)(sizeof(_expected) / sizeof(_expected[0]));
    for(i=0; i<_n_ || i<expected; ++i)
    {
        const bool ok = i < _n_  &&  i < expected
                        &&  _trace[i].task == _expected[i].task
                        &&  strcmp(_trace[i].tag, _expected[i].tag) == 0
                        &&  _trace[i].value == _expected[i].value;
        if(!ok)
        {
            ++ failures;
            printf("  %2u: got %s, expected %s\n", i,
                   i < _n_ ? _trace[i].tag : "(nothing)", i < expected ? _expected[i].tag : "(nothing)");
            if(i < _n_) printf("      got task %u value %ld\n", _trace[i].task, _trace[i].value);
        }
    }
    printf("%s: test_prio (%u entries, %u failures, %lu idle loops)\n", failures == 0 ? "PASS" : "FAIL",
           _n_, failures, (unsigned long)_idleLoops);
    exit(failures == 0 ? 0 : 1);
}

void test_idleHook(void)
{
    if(_done) verify();
    if(++_idleLoops > 100000ul) { printf("FAIL: runaway\n"); verify(); }
    rtos_host_tick();
}

static uint8_t effPrio(uint8_t task)
{
    rtos_taskInfo_t info;
    rtos_getTaskInfo(task, &info);
    return info.effPrio;
}

/* Compute without blocking while ticks arrive from a simulated interrupt, until T1 moves
   the step on or the tick budget is spent: the interrupt tail is what must switch away
   to a task that is boosted above the computing one. Returns true if the step moved. */
static bool computeUntilStep(uint8_t step)
{
    int t;
    for(t=0; t<5 && _step != step; ++t)
        rtos_host_tick();
    return _step == step;
}

/* ---- tasks --------------------------------------------------------------------------- */

static void task1(void *arg)
{
    (void)arg;
    /* 1 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM", effPrio(T1));
    _step = 1;
    rtos_delay(3);                      /* blocks while holding: T2 computes meanwhile */
    trace(T1, "wokeAtCeiling", _step);
    _step = 2;
    rtos_mutexUnlock(&_m);              /* drops to 1: T2 preempts here */
    trace(T1, "unlockM", effPrio(T1));

    /* 2 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM2", effPrio(T1));
    _step = 20;
    rtos_delay(3);                      /* T2 blocks on M, T3 starts computing */
    rtos_mutexUnlock(&_m);              /* hand-over to T2 at the ceiling */
    trace(T1, "unlockM2", effPrio(T1));

    /* 3 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM3", effPrio(T1));
    rtos_mutexLock(&_n);
    trace(T1, "lockN3", effPrio(T1));
    _step = 30;
    rtos_delay(3);                      /* T3 sees step 30 and computes */
    rtos_mutexUnlock(&_n);
    trace(T1, "unlockN3", effPrio(T1));
    _step = 31;
    rtos_mutexUnlock(&_m);              /* drops to 1: T3 preempts here */
    trace(T1, "unlockM3", effPrio(T1));

    /* 4 */
    rtos_mutexLock(&_n);
    trace(T1, "lockN4", effPrio(T1));
    _step = 40;
    {
        int t;
        for(t=0; t<5 && !_t4Done; ++t)  /* T4 preempts on a tick and readies T2 */
            rtos_host_tick();
    }
    trace(T1, "ranBeforeT2", _t4Done);
    _step = 41;
    rtos_mutexUnlock(&_n);              /* drops to 1: T2 gets its event now */
    trace(T1, "unlockN4", effPrio(T1));
    trace(T1, "end", 0);
    _done = true;
    for(;;)
        rtos_delay(100);
}

static void task2(void *arg)
{
    (void)arg;
    /* 1 */
    while(_step != 1)
        rtos_delay(0);
    trace(T2, "computing", 0);
    (void)computeUntilStep(2);
    trace(T2, "computeDone", _step);
    /* 2 */
    while(_step != 20)
        rtos_delay(0);
    trace(T2, "lockM2blocked", 0);
    rtos_mutexLock(&_m);
    trace(T2, "gotM", effPrio(T2));
    _step = 21;
    rtos_mutexUnlock(&_m);              /* drops to 2: T3 gets back in here */
    trace(T2, "unlockM2", effPrio(T2));
    /* 4 */
    (void)rtos_waitEvents(0x1, RTOS_WAIT_ANY, RTOS_FOREVER);
    trace(T2, "gotEvent", _step);
    for(;;)
        rtos_delay(100);
}

static void task4(void *arg)
{
    (void)arg;
    while(_step != 40)
        rtos_delay(0);
    trace(T4, "preempts", 0);
    rtos_postEvents(T2, 0x1);
    _t4Done = true;
    for(;;)
        rtos_delay(100);
}

static void task3(void *arg)
{
    (void)arg;
    /* 2 */
    while(_step != 20)
        rtos_delay(0);
    rtos_delay(0);                      /* T2 is blocked on M now */
    trace(T3, "computing", 0);
    trace(T3, "computeDone", computeUntilStep(21));
    /* 3 */
    while(_step != 30)
        rtos_delay(0);
    trace(T3, "spinning", 0);
    trace(T3, "ranAfterDrop", computeUntilStep(31));
    for(;;)
        rtos_delay(100);
}

int main(void)
{
    rtos_init();
    rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
    rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
    rtos_createTask(T3, task3, NULL, _stack3, sizeof(_stack3));
    rtos_createTask(T4, task4, NULL, _stack4, sizeof(_stack4));
    rtos_start();
    return 0;
}
