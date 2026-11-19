/**
 * @file test_tie.c
 * The scheduling order at equal effective priorities. The contract: tasks are ordered by
 * the key (effPrio, boosted, prio), boosted meaning effPrio != prio, compared
 * lexicographically; the picker takes the greatest key, and a ready task preempts the
 * running one only if its key is strictly greater. The immediate priority ceiling protocol
 * needs this: an owner raised to a ceiling c runs before the task whose base priority is
 * c (which is a user of the mutex and would otherwise block on it), and the order among
 * several boosted tasks at one ceiling is fixed and documented, not an artefact of the
 * scan. Trace based like test_prio; the mutexes M and P both have ceiling 3.
 *
 *   1. Tie in the task-context pick: T1 holds M (eff 3), T4 (above the ceiling) preempts,
 *      readies T3 (base 3) with an event and blocks. The owner at 3 must continue before
 *      T3 although T3's effective priority is also 3.
 *   2. Two boosted tasks at the same ceiling: T1 holds M and T2 holds P, both blocked
 *      while holding (allowed, discouraged) and woken by the same tick. T2, the higher
 *      base priority, runs first.
 *   3. Tie in the interrupt tail: T1 holds M and sleeps, T3 (base 3) computes while ticks
 *      arrive. The tick that wakes T1 must switch to it at once.
 *   4. The owner is suspended while blocked with M held: a contender waits on M without
 *      a deadlock panic (the detector only follows mutex waits), the owner is resumed,
 *      wakes still as the owner, and the unlock hands M over.
 *   5. The running owner against the task at the ceiling level: T1 holds M and computes
 *      while ticks wake T3 (base 3). T3 becomes ready but must not preempt the owner
 *      (equal effective priority, the running task is boosted); it runs at the unlock.
 *      This is the fast path's case: the only boosted ready task is the running one.
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
static rtos_mutex_t _m = RTOS_MUTEX_INIT(3);
static rtos_mutex_t _p = RTOS_MUTEX_INIT(3);
static uint32_t _idleLoops;
static volatile uint8_t _step;      /* moved on by T1, and by T2 in scenario 4 */
static volatile bool _t4Done, _t3Ran, _t1Woke, _t2Woke, _t3Ran5;
static volatile rtos_tick_t _wakeTarget;
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
    /* 1: boosted owner against the task at the ceiling level, pick in task context */
    { T1, "lockM1", 3 },
    { T4, "preempts", 0 },
    { T1, "ranBeforeT3", 0 },           /* T3 has not run when T1 gets back (t3Ran = 0) */
    { T3, "gotEvent", 11 },             /* T3 only ran after the unlock (step 11) */
    { T1, "unlockM1", 1 },
    /* 2: two boosted tasks at the same ceiling, woken by one tick */
    { T1, "lockM2", 3 },
    { T2, "lockP2", 3 },
    { T2, "wokeFirst", 0 },             /* T1 had not woken (t1Woke = 0) */
    { T1, "wokeSecond", 1 },            /* T1 got in when T2 dropped to 2 */
    { T2, "unlockP2", 2 },              /* T1's unlock dropped it to 1: T2 (2) back in */
    { T1, "unlockM2", 1 },
    /* 3: boosted owner against the task at the ceiling level, pick in the interrupt tail */
    { T1, "lockM3", 3 },
    { T3, "computing", 0 },
    { T1, "wokeAtTie", 30 },            /* the tick switched to T1: T3 had not finished */
    { T3, "computeDone", 1 },           /* T3 saw step 31 within its tick budget */
    { T1, "unlockM3", 1 },
    /* 4: owner suspended while blocked with M held */
    { T1, "lockM4", 3 },
    { T2, "lockBlocked", 0 },           /* T2 waits for the suspended owner, no panic */
    { T3, "resume", 0 },
    { T1, "resumed", 3 },               /* still the owner, still at the ceiling */
    { T2, "gotM", 3 },                  /* hand-over raised T2 to 3: runs before T1 (1) */
    { T2, "unlockM4b", 2 },
    { T1, "unlockM4", 1 },
    /* 5: running owner against the task at the ceiling level, woken by the interrupt */
    { T1, "lockM5", 3 },
    { T1, "heldAgainstT3", 0 },         /* three ticks woke T3, it never ran (t3Ran5 = 0) */
    { T3, "ranAfterUnlock", 50 },
    { T1, "unlockM5", 1 },
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
    printf("%s: test_tie (%u entries, %u failures, %lu idle loops)\n", failures == 0 ? "PASS" : "FAIL",
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
   the step on or the tick budget is spent. Returns true if the step moved. */
static bool computeUntilStep(uint8_t step)
{
    int t;
    for(t=0; t<5 && _step != step; ++t)
        rtos_host_tick();
    return _step == step;
}

/* Sleep until the absolute tick _wakeTarget (rtos_delay(n) wakes at now + n + 1). */
static void delayUntilTarget(void)
{
    rtos_delay((rtos_tick_t)(_wakeTarget - rtos_getTick() - 1u));
}

/* ---- tasks --------------------------------------------------------------------------- */

static void task1(void *arg)
{
    (void)arg;
    /* 1 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM1", effPrio(T1));
    _step = 10;
    {
        int t;
        for(t=0; t<5 && !_t4Done; ++t)  /* T4 preempts on a tick, readies T3, blocks */
            rtos_host_tick();
    }
    trace(T1, "ranBeforeT3", _t3Ran);
    _step = 11;
    rtos_mutexUnlock(&_m);              /* drops to 1: T3 preempts here */
    trace(T1, "unlockM1", effPrio(T1));
    rtos_delay(0);                      /* let the others settle before the next scenario */

    /* 2 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM2", effPrio(T1));
    _wakeTarget = rtos_getTick() + 4u;
    _step = 20;
    delayUntilTarget();                 /* blocks while holding; T2 locks P and joins */
    _t1Woke = true;
    trace(T1, "wokeSecond", _t2Woke);
    rtos_mutexUnlock(&_m);
    trace(T1, "unlockM2", effPrio(T1));
    rtos_delay(0);

    /* 3 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM3", effPrio(T1));
    _step = 30;
    rtos_delay(2);                      /* T3 computes meanwhile, feeding ticks */
    trace(T1, "wokeAtTie", _step);
    _step = 31;
    rtos_mutexUnlock(&_m);              /* drops to 1: T3 preempts here */
    trace(T1, "unlockM3", effPrio(T1));
    rtos_delay(0);

    /* 4 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM4", effPrio(T1));
    _step = 40;
    rtos_delay(2);                      /* suspended by T2 in here, resumed by T3 */
    trace(T1, "resumed", effPrio(T1));
    rtos_mutexUnlock(&_m);              /* hand-over to T2, which runs at 3 before us */
    trace(T1, "unlockM4", effPrio(T1));
    rtos_delay(0);

    /* 5 */
    rtos_mutexLock(&_m);
    trace(T1, "lockM5", effPrio(T1));
    {
        int t;
        for(t=0; t<3; ++t)              /* each tick wakes the polling T3: ready, not run */
            rtos_host_tick();
    }
    trace(T1, "heldAgainstT3", _t3Ran5);
    _step = 50;
    rtos_mutexUnlock(&_m);              /* drops to 1: T3 runs now */
    trace(T1, "unlockM5", effPrio(T1));
    trace(T1, "end", 0);
    _done = true;
    for(;;)
        rtos_delay(100);
}

static void task2(void *arg)
{
    (void)arg;
    /* 2 */
    while(_step != 20)
        rtos_delay(0);
    rtos_mutexLock(&_p);
    trace(T2, "lockP2", effPrio(T2));
    delayUntilTarget();                 /* same wake-up tick as T1 */
    _t2Woke = true;
    trace(T2, "wokeFirst", _t1Woke);
    rtos_mutexUnlock(&_p);              /* drops to 2: T1 (3) gets in here */
    trace(T2, "unlockP2", effPrio(T2));
    /* 4 */
    while(_step != 40)
        rtos_delay(0);
    rtos_suspend(T1);
    _step = 41;
    trace(T2, "lockBlocked", 0);
    rtos_mutexLock(&_m);                /* owner suspended and blocked: we wait */
    trace(T2, "gotM", effPrio(T2));
    rtos_mutexUnlock(&_m);
    trace(T2, "unlockM4b", effPrio(T2));
    for(;;)
        rtos_delay(100);
}

static void task3(void *arg)
{
    (void)arg;
    /* 1 */
    (void)rtos_waitEvents(0x1, RTOS_WAIT_ANY, RTOS_FOREVER);
    _t3Ran = true;
    trace(T3, "gotEvent", _step);
    /* 3 */
    while(_step != 30)
        rtos_delay(0);
    trace(T3, "computing", 0);
    trace(T3, "computeDone", computeUntilStep(31));
    /* 4 */
    while(_step != 41)
        rtos_delay(0);
    rtos_delay(1);                      /* T1's wake-up passes while it is suspended */
    trace(T3, "resume", 0);
    rtos_resume(T1);
    rtos_delay(0);                      /* give way regardless of the tie rule */
    /* 5 */
    while(_step != 50)
        rtos_delay(0);                  /* woken by T1's ticks while T1 holds M */
    _t3Ran5 = true;
    trace(T3, "ranAfterUnlock", _step);
    for(;;)
        rtos_delay(100);
}

static void task4(void *arg)
{
    (void)arg;
    while(_step != 10)
        rtos_delay(0);
    trace(T4, "preempts", 0);
    rtos_postEvents(T3, 0x1);           /* T3 (base 3) is ready now, like the owner at 3 */
    _t4Done = true;
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
