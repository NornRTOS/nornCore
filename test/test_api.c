/**
 * @file test_api.c
 * Contract test of the public API on the host port: every function is called the way the
 * header documents it, from one scripting task, with a second task as the partner that
 * blocks and is woken, and simulated interrupts for the *FromIsr variants. Unlike the
 * trace based tests this one checks return values and object state directly, so that
 * the documented contract of each call (polling with RTOS_NO_WAIT, timeouts, the
 * statistics fields, the run time initializers) is pinned down in one place.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtos.h"
#include "rtos_kernel.h"

#define T1  1u      /* partner, low priority */
#define T2  2u      /* script */

static uint8_t _stack1[1], _stack2[1];
static uint32_t _idleLoops;
static volatile bool _done;
static unsigned _checks, _fails;
static volatile unsigned _t1Woken;
static volatile int _t1Phase;
static volatile unsigned _t1MutexEffPrio;   /* T1's effective priority while it held the mutex */

#define CHECK(cond)  check((cond), #cond, __LINE__)
static void check(bool ok, const char *what, int line)
{
    ++ _checks;
    if(!ok)
    {
        ++ _fails;
        printf("CHECK FAILED line %d: %s\n", line, what);
    }
}

static rtos_sem_t _sem;
static rtos_mutex_t _mutex;
static uint8_t _qStorage[3 * 2];
static rtos_queue_t _q;
static uint8_t _mbStorage[2];
static rtos_mbox_t _mb;
static rtos_evgroup_t _g;
static rtos_timer_t _tmr;
static uint8_t _poolStorage[2 * 8];
static rtos_pool_t _pool;
static uint8_t _heapArea[256];
static volatile unsigned _timerFired, _restarts;
static rtos_timer_t _tmrRestart;

static void timerCb(void *arg) { (void)arg; ++ _timerFired; }

/** A callback that restarts its own timer with a zero delay: must run once per tick,
    never again within the same tick (that would loop inside the tick interrupt). */
static void restartCb(void *arg)
{
    (void)arg;
    ++ _restarts;
    rtos_timerStart(&_tmrRestart, 0, 0);
}

/* Simulated interrupts. */
static void isrSemGive(void)      { rtos_semGiveFromIsr(&_sem); }
static void isrQueueSend(void)    { const uint16_t v = 0x1234; CHECK(rtos_queueSendFromIsr(&_q, &v)); }
static void isrQueueRecv(void)    { uint16_t v = 0; CHECK(rtos_queueReceiveFromIsr(&_q, &v)); CHECK(v == 0x1234); }
static void isrQueueRecvEmpty(void) { uint16_t v; CHECK(!rtos_queueReceiveFromIsr(&_q, &v)); }
static void isrMboxPost(void)     { const uint16_t v = 0x4321; CHECK(!rtos_mboxPostFromIsr(&_mb, &v)); }
static void isrNotify(void)       { rtos_notifyFromIsr(T2, 0x10, RTOS_NOTIFY_OR); }
static void isrEvgroupSet(void)   { rtos_evgroupSetFromIsr(&_g, 0x40); }
static void isrPostEvents(void)   { rtos_postEventsFromIsr(T1, 0x2); }
static void isrPool(void)
{
    void *p = rtos_poolAllocFromIsr(&_pool);
    CHECK(p != NULL);
    rtos_poolFreeFromIsr(&_pool, p);
}
static void isrResume(void)       { rtos_resumeFromIsr(T1); }

static void task1(void *arg)
{
    (void)arg;
    for(;;)
    {
        rtos_evt_t got;
        _t1Phase = 1;
        got = rtos_waitEvents(0x2 | 0x8 | 0x4, RTOS_WAIT_ANY, RTOS_FOREVER);
        CHECK(got == 0x2  ||  got == 0x8  ||  got == 0x4);
        if(got != 0x4)
            ++ _t1Woken;                        /* the mutex round is not a counted wake */
        if(got & 0x4)
        {
            /* The mutex is held by T2, which blocks while holding it: we wait and get it
               handed over, raised to the ceiling from that moment. */
            rtos_taskInfo_t info;
            rtos_mutexLock(&_mutex);
            rtos_getTaskInfo(T1, &info);
            _t1MutexEffPrio = info.effPrio;
            rtos_mutexUnlock(&_mutex);
            rtos_getTaskInfo(T1, &info);
            CHECK(info.effPrio == T1);
        }
    }
}

static void task2(void *arg)
{
    rtos_taskInfo_t info;
    uint16_t v, v2;
    (void)arg;

    /* --- semaphore: run time init, polling, the limit and the lost count --- */
    rtos_semInit(&_sem, 2);
    CHECK(rtos_semTake(&_sem, RTOS_NO_WAIT));
    CHECK(rtos_semTake(&_sem, RTOS_NO_WAIT));
    CHECK(!rtos_semTake(&_sem, RTOS_NO_WAIT));
    CHECK(!rtos_semTake(&_sem, 2));             /* times out after 2 ticks */
    rtos_semInitMax(&_sem, 0, 1);
    rtos_semGive(&_sem);
    rtos_semGive(&_sem);                        /* binary: this one is lost */
    rtos_host_simulateIsr(isrSemGive);          /* and this one */
    CHECK(_sem.count == 1  &&  _sem.lost == 2);
    CHECK(rtos_semTake(&_sem, RTOS_NO_WAIT));
    CHECK(!rtos_semTake(&_sem, RTOS_NO_WAIT));

    /* --- mutex: run time init, lock and unlock restore the state --- */
    rtos_mutexInit(&_mutex, T2);
    CHECK(_mutex.owner == RTOS_NO_TASK);
    rtos_mutexLock(&_mutex);
    CHECK(_mutex.owner == T2);
    rtos_getTaskInfo(T2, &info);
    CHECK(info.effPrio == T2);                  /* ceiling == own priority: no raise */
    rtos_mutexUnlock(&_mutex);
    CHECK(_mutex.owner == RTOS_NO_TASK);
    /* Fallback hand-over to a waiter below the ceiling: lock, make T1 want it, block
       while holding it (discouraged but allowed), unlock hands it to T1 raised to T2. */
    rtos_mutexLock(&_mutex);
    rtos_postEvents(T1, 0x4);
    rtos_delay(2);                              /* T1 runs, blocks on the mutex */
    CHECK(_mutex.waiters != 0);
    rtos_mutexUnlock(&_mutex);                  /* T1 is the owner now, at prio T2 */
    rtos_delay(1);
    CHECK(_t1MutexEffPrio == T2);
    CHECK(_mutex.owner == RTOS_NO_TASK);

    /* --- queue: run time init, FIFO, full, drops and the high water mark, ISR variants --- */
    rtos_queueInit(&_q, _qStorage, 2, 3);
    CHECK(rtos_queueCount(&_q) == 0);
    for(v=1; v<=3; ++v)
        CHECK(rtos_queueSend(&_q, &v, RTOS_NO_WAIT));
    CHECK(!rtos_queueSend(&_q, &v, RTOS_NO_WAIT));
    CHECK(!rtos_queueSend(&_q, &v, 2));         /* times out: nobody reads */
    CHECK(rtos_queueCount(&_q) == 3  &&  _q.dropped == 2  &&  _q.maxCount == 3);
    for(v=1; v<=3; ++v)
    {
        CHECK(rtos_queueReceive(&_q, &v2, RTOS_NO_WAIT));
        CHECK(v2 == v);
    }
    CHECK(!rtos_queueReceive(&_q, &v2, RTOS_NO_WAIT));
    CHECK(!rtos_queueReceive(&_q, &v2, 2));
    rtos_host_simulateIsr(isrQueueSend);
    CHECK(rtos_queueCount(&_q) == 1);
    rtos_host_simulateIsr(isrQueueRecv);
    rtos_host_simulateIsr(isrQueueRecvEmpty);
    CHECK(rtos_queueCount(&_q) == 0);

    /* --- mailbox: run time init, full flag, overwrite count, ISR post --- */
    rtos_mboxInit(&_mb, _mbStorage, 2);
    CHECK(!rtos_mboxIsFull(&_mb));
    v = 7;
    CHECK(!rtos_mboxPost(&_mb, &v));            /* false: nothing was overwritten */
    CHECK(rtos_mboxIsFull(&_mb));
    v = 8;
    CHECK(rtos_mboxPost(&_mb, &v));             /* true: 7 was overwritten */
    CHECK(_mb.overwrites == 1);
    CHECK(rtos_mboxFetch(&_mb, &v2, RTOS_NO_WAIT)  &&  v2 == 8);
    CHECK(!rtos_mboxIsFull(&_mb));
    CHECK(!rtos_mboxFetch(&_mb, &v2, RTOS_NO_WAIT));
    CHECK(!rtos_mboxFetch(&_mb, &v2, 2));
    rtos_host_simulateIsr(isrMboxPost);
    CHECK(rtos_mboxFetch(&_mb, &v2, RTOS_NO_WAIT)  &&  v2 == 0x4321);

    /* --- event group: run time init, clear, satisfied wait with and without clear --- */
    rtos_evgroupInit(&_g, 0x5);
    CHECK(rtos_evgroupGet(&_g) == 0x5);
    rtos_evgroupClear(&_g, 0x1);
    CHECK(rtos_evgroupGet(&_g) == 0x4);
    CHECK(rtos_evgroupWait(&_g, 0x4, RTOS_WAIT_ANY, RTOS_NO_WAIT) == 0x4);
    CHECK(rtos_evgroupGet(&_g) == 0x4);         /* no clear option: bits stay */
    CHECK(rtos_evgroupWait(&_g, 0x4, RTOS_WAIT_ANY | RTOS_WAIT_CLEAR, RTOS_NO_WAIT) == 0x4);
    CHECK(rtos_evgroupGet(&_g) == 0);
    CHECK(rtos_evgroupWait(&_g, 0x4, RTOS_WAIT_ANY, RTOS_NO_WAIT) == 0);
    CHECK(rtos_evgroupWait(&_g, 0x4, RTOS_WAIT_ALL, 2) == 0);     /* timeout */
    rtos_host_simulateIsr(isrEvgroupSet);
    CHECK(rtos_evgroupGet(&_g) == 0x40);

    /* --- software timer: restart while active, remaining, stop --- */
    rtos_timerInit(&_tmr, timerCb, NULL);
    CHECK(!rtos_timerIsActive(&_tmr));
    rtos_timerStart(&_tmr, 5, 0);
    CHECK(rtos_timerIsActive(&_tmr));
    rtos_timerStart(&_tmr, 10, 0);              /* restart: the old expiry is dropped */
    CHECK(rtos_timerRemaining(&_tmr) >= 9  &&  rtos_timerRemaining(&_tmr) <= 10);
    rtos_delay(7);
    CHECK(_timerFired == 0  &&  rtos_timerIsActive(&_tmr));
    rtos_delay(5);
    CHECK(_timerFired == 1  &&  !rtos_timerIsActive(&_tmr));
    rtos_timerStart(&_tmr, 3, 3);               /* periodic: fires 3..4 ticks out, then every 3 */
    rtos_delay(8);                              /* 8..9 ticks: two or three expiries */
    CHECK(_timerFired == 3  ||  _timerFired == 4);
    rtos_timerStop(&_tmr);
    CHECK(!rtos_timerIsActive(&_tmr)  &&  rtos_timerRemaining(&_tmr) == 0);
    {
        const unsigned fired = _timerFired;
        rtos_port_cs_t cs;
        rtos_delay(4);
        CHECK(_timerFired == fired);            /* stopped: no more expiries */
        cs = rtos_port_enterCritical();
        CHECK(rtos_kernel_timerTicksToNext() == RTOS_FOREVER);    /* no timer active */
        rtos_port_exitCritical(cs);
    }

    /* --- events: broadcast reaches the partner and ourselves, ISR post --- */
    rtos_delay(1);                              /* T1 is blocked in waitEvents now */
    CHECK(_t1Phase == 1  &&  _t1Woken == 0);
    rtos_broadcastEvents(0x8);
    CHECK(rtos_waitEvents(0x8, RTOS_WAIT_ANY, RTOS_NO_WAIT) == 0x8);
    CHECK(rtos_waitEvents(0x8, RTOS_WAIT_ANY, RTOS_NO_WAIT) == 0);
    rtos_delay(1);                              /* T1 runs */
    CHECK(_t1Woken == 1);
    rtos_host_simulateIsr(isrPostEvents);
    rtos_delay(1);
    CHECK(_t1Woken == 2);
    CHECK(rtos_waitEvents(0x1, RTOS_WAIT_ALL, 2) == 0);           /* timeout */

    /* --- notifications: set, or, inc; wait takes all, take decrements --- */
    rtos_notify(T2, 3, RTOS_NOTIFY_SET);
    CHECK(rtos_notifyWait(&v, RTOS_NO_WAIT)  &&  v == 3);
    CHECK(!rtos_notifyWait(&v, RTOS_NO_WAIT));
    CHECK(!rtos_notifyWait(&v, 2));
    rtos_host_simulateIsr(isrNotify);
    rtos_notify(T2, 0x1, RTOS_NOTIFY_OR);
    CHECK(rtos_notifyWait(NULL, RTOS_NO_WAIT));                   /* NULL: value discarded */
    rtos_notify(T2, 0, RTOS_NOTIFY_INC);
    rtos_notify(T2, 0, RTOS_NOTIFY_INC);
    CHECK(rtos_notifyTake(RTOS_NO_WAIT));
    CHECK(rtos_notifyTake(RTOS_NO_WAIT));
    CHECK(!rtos_notifyTake(RTOS_NO_WAIT));
    CHECK(!rtos_notifyTake(2));

    /* --- pool: run time init, exhaustion, failed count, ISR variants --- */
    rtos_poolInit(&_pool, _poolStorage, 8, 2);
    CHECK(rtos_poolFreeCount(&_pool) == 2);
    {
        void *a = rtos_poolAlloc(&_pool, RTOS_NO_WAIT);
        void *b = rtos_poolAlloc(&_pool, RTOS_NO_WAIT);
        CHECK(a != NULL  &&  b != NULL  &&  a != b);
        CHECK(rtos_poolAlloc(&_pool, RTOS_NO_WAIT) == NULL);
        CHECK(rtos_poolAlloc(&_pool, 2) == NULL);                  /* timeout */
        CHECK(_pool.failed == 2  &&  _pool.minFree == 0);
        rtos_poolFree(&_pool, a);
        rtos_poolFree(&_pool, b);
        CHECK(rtos_poolFreeCount(&_pool) == 2);
    }
    rtos_host_simulateIsr(isrPool);
    CHECK(rtos_poolFreeCount(&_pool) == 2);

    /* --- heap: free(NULL) is a no-op, stats --- */
    rtos_heapInit(_heapArea, sizeof(_heapArea));
    rtos_free(NULL);
    {
        rtos_heapStats_t hs;
        void *p = rtos_malloc(16);
        CHECK(p != NULL);
        rtos_heapStats(&hs);
        CHECK(hs.allocs == 1  &&  hs.frees == 0  &&  hs.free < hs.total);
        rtos_free(p);
        rtos_heapStats(&hs);
        CHECK(hs.frees == 1  &&  hs.free == hs.total);
        /* Sizes near SIZE_MAX must be refused, not wrapped into a small block. */
        CHECK(rtos_malloc(SIZE_MAX) == NULL);
        CHECK(rtos_malloc(SIZE_MAX - 8u) == NULL);
        CHECK(rtos_malloc(SIZE_MAX / 2u) == NULL);      /* larger than the heap, no wrap */
        rtos_heapStats(&hs);
        CHECK(hs.failed == 3  &&  hs.free == hs.total  &&  hs.allocs == 1);
    }
    /* A timer whose callback restarts it with delay 0 fires once per tick and the
       tick interrupt always terminates. */
    rtos_timerInit(&_tmrRestart, restartCb, NULL);
    rtos_timerStart(&_tmrRestart, 1, 0);
    rtos_delay(5);
    rtos_timerStop(&_tmrRestart);
    CHECK(_restarts >= 4  &&  _restarts <= 6);
    {
        const unsigned n = _restarts;
        rtos_delay(3);
        CHECK(_restarts == n);
    }

    /* --- periodic wait: the base is the task start, so a first call after a long start
       up is itself an overrun (documented); then overrun detection and the counter reset --- */
    CHECK(rtos_getOverrunCount(T2, false) == 0);
    CHECK(rtos_waitPeriodic(2));                /* far past the task start: overrun, re-based */
    CHECK(!rtos_waitPeriodic(2));               /* on the grid now */
    rtos_delay(5);
    CHECK(rtos_waitPeriodic(2));                /* the boundary passed: overrun */
    CHECK(rtos_getOverrunCount(T2, true) == 2);
    CHECK(rtos_getOverrunCount(T2, false) == 0);
    CHECK(!rtos_waitPeriodic(2));

    /* --- inspection: task info, stack free without stack check, run time, load --- */
    CHECK(rtos_getTaskInfo(T1, &info));
    CHECK(info.state == RTOS_TASK_BLOCKED  &&  info.waitKind == RTOS_WAIT_EVENTS  &&  !info.hasTimeout);
    CHECK(rtos_getTaskInfo(T2, &info)  &&  info.state == RTOS_TASK_RUNNING);
    CHECK(rtos_getTaskInfo(RTOS_IDLE_TASK, &info)  &&  info.state == RTOS_TASK_READY);
    CHECK(!rtos_getTaskInfo(RTOS_NUM_TASKS, &info));
    CHECK(!rtos_getTaskInfo(T1, NULL));
    CHECK(rtos_getStackFree(T1) == 0);          /* RTOS_CFG_STACK_CHECK 0 */
    CHECK(rtos_getTaskRunTime(RTOS_IDLE_TASK) > 0);
    (void)rtos_getCpuLoad();
    CHECK(rtos_getCpuLoad() == 0);              /* nothing elapsed since the last call */
    CHECK(rtos_getCurrentTask() == T2);

    /* --- suspend/resume from an interrupt --- */
    rtos_suspend(T1);
    rtos_postEvents(T1, 0x2);                   /* pending until resume */
    rtos_delay(1);
    CHECK(_t1Woken == 2);
    rtos_host_simulateIsr(isrResume);
    rtos_delay(1);
    CHECK(_t1Woken == 3);

    /* --- watchdog: monitor on and off --- */
    rtos_wdtInit(1000);
    rtos_wdtMonitor(T2, 50);
    rtos_wdtCheckIn();
    rtos_wdtMonitor(T2, 0);
    rtos_delay(60);                             /* unmonitored: no panic */

    /* --- panic record: none after a clean start --- */
    {
        rtos_panicInfo_t pi;
        CHECK(!rtos_getLastPanic(&pi));
        CHECK(rtos_getResetCause() != RTOS_RESET_WATCHDOG);
    }

    _done = true;
    for(;;)
        rtos_delay(100);
}

void test_idleHook(void)
{
    if(_done)
    {
        printf("%s: test_api (%u checks, %u failures, %lu idle loops)\n"
               , _fails == 0 ? "PASS" : "FAIL", _checks, _fails, (unsigned long)_idleLoops);
        exit(_fails == 0 ? 0 : 1);
    }
    if(++_idleLoops > 100000ul)
    {
        printf("FAIL: runaway, test did not finish (%u checks, %u failures)\n", _checks, _fails);
        exit(1);
    }
    rtos_host_tick();
}

void rtos_assertFailed(const char *file, uint16_t line)
{
    printf("ASSERTION FAILED: %s line %u (current task %u)\n", file, line, rtos_getCurrentTask());
    exit(2);
}

int main(void)
{
    rtos_init();
    rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
    rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
    rtos_start();
    return 0;
}
