/**
 * @file test_timeout.c
 * Timeouts at the limit of the tick type (audit finding F6), with 16 bit ticks so that the
 * limit is reachable on the host. The timed list, the software timers and the periodic
 * wait compare tick values as signed differences, which works up to half the counter
 * range: RTOS_MAX_TIMEOUT. Checked here:
 *
 *   - rtos_delay accepts every value but RTOS_FOREVER and splits a long delay into waits
 *     of at most RTOS_MAX_TIMEOUT, so RTOS_MAX_TIMEOUT + 1000 and RTOS_FOREVER - 1 (which
 *     the "+1" used to turn into FOREVER) last as long as asked;
 *   - a semaphore wait of exactly RTOS_MAX_TIMEOUT times out after that many ticks, while
 *     a short delay of another task and a short timer, sorted in front of it, keep their
 *     own times;
 *   - a one shot and a periodic timer of RTOS_MAX_TIMEOUT, a periodic wait of it;
 *   - rtos_getTimeUs over the whole run, across eight wraps of the 16 bit tick (F9).
 *
 * Ticks are counted in 32 bits by the idle hook, which delivers them one at a time. The
 * long waits are alone in the timed list, as in the audit's reproduction: a shorter
 * entry in front of a long one would hide the defect until the long one becomes the head.
 */

#include <stdio.h>
#include <stdlib.h>
#include "rtos.h"

#ifndef RTOS_MAX_TIMEOUT                        /* to run this against an older kernel */
# define RTOS_MAX_TIMEOUT   ((rtos_tick_t)(RTOS_FOREVER >> 1))
#endif

#define T1 1
#define T2 2
#define T3 3

static uint8_t _stack1[1], _stack2[1], _stack3[1];
static rtos_sem_t _never = RTOS_SEM_INIT_BINARY(0);
static rtos_sem_t _fired = RTOS_SEM_INIT_BINARY(0);
static rtos_timer_t _short, _long;
static volatile uint32_t _ticks;                /* 32 bit tick count of the idle hook */
static volatile uint32_t _shortFiredAt, _t2Elapsed;
static rtos_sem_t _t2Go = RTOS_SEM_INIT_BINARY(0);
static int _failures, _checks;

void test_idleHook(void)
{
    ++ _ticks;
    if(_ticks > 2000000ul)
    {
        printf("FAIL: runaway (%d checks, %d failures)\n", _checks, _failures);
        exit(1);
    }
    rtos_host_tick();
}

static void check(const char *what, uint32_t got, uint32_t lo, uint32_t hi)
{
    ++ _checks;
    if(got < lo  ||  got > hi)
    {
        ++ _failures;
        printf("CHECK FAILED %s: %lu not in %lu..%lu\n", what, (unsigned long)got,
               (unsigned long)lo, (unsigned long)hi);
    }
    else
        printf("ok %s: %lu\n", what, (unsigned long)got);
}

static void shortCb(void *arg) { (void)arg; _shortFiredAt = _ticks; }
static void longCb(void *arg)  { (void)arg; rtos_semGiveFromIsr(&_fired); }

static void task3(void *arg)
{
    uint32_t t0;
    const uint32_t us0 = rtos_getTimeUs(), ticks0 = _ticks;
    (void)arg;

    t0 = _ticks;
    rtos_delay((rtos_tick_t)(RTOS_MAX_TIMEOUT + 1000u));
    check("delay MAX+1000", _ticks - t0, RTOS_MAX_TIMEOUT + 1000ul, RTOS_MAX_TIMEOUT + 1003ul);

    t0 = _ticks;
    rtos_delay((rtos_tick_t)(RTOS_FOREVER - 1u));
    check("delay FOREVER-1", _ticks - t0, (uint32_t)RTOS_FOREVER - 1ul, (uint32_t)RTOS_FOREVER + 2ul);

    /* A wait of exactly the limit, with a short delay and a short timer in front of it. */
    rtos_timerInit(&_short, shortCb, NULL);
    t0 = _ticks;
    rtos_timerStart(&_short, 50, 0);
    rtos_semGive(&_t2Go);                       /* T2 delays 100 while we wait */
    check("semTake MAX timed out", rtos_semTake(&_never, RTOS_MAX_TIMEOUT) ? 0u : 1u, 1, 1);
    check("semTake MAX elapsed", _ticks - t0, RTOS_MAX_TIMEOUT - 1ul, RTOS_MAX_TIMEOUT);
    check("short timer beside it", _shortFiredAt - t0, 49, 51);
    check("short delay beside it", _t2Elapsed, 100, 101);

    /* Timers of the limit. */
    rtos_timerInit(&_long, longCb, NULL);
    t0 = _ticks;
    rtos_timerStart(&_long, RTOS_MAX_TIMEOUT, 0);
    (void)rtos_semTake(&_fired, RTOS_FOREVER);
    check("one shot timer MAX", _ticks - t0, RTOS_MAX_TIMEOUT - 1ul, RTOS_MAX_TIMEOUT + 1ul);
    rtos_timerStart(&_long, RTOS_MAX_TIMEOUT, RTOS_MAX_TIMEOUT);
    (void)rtos_semTake(&_fired, RTOS_FOREVER);
    t0 = _ticks;
    (void)rtos_semTake(&_fired, RTOS_FOREVER);
    check("periodic timer MAX", _ticks - t0, RTOS_MAX_TIMEOUT, RTOS_MAX_TIMEOUT);
    rtos_timerStop(&_long);

    /* A periodic wait of the limit: the first call re-bases (the base is old), the
       second waits a full period. */
    (void)rtos_waitPeriodic(RTOS_MAX_TIMEOUT);
    t0 = _ticks;
    check("waitPeriodic MAX on time", rtos_waitPeriodic(RTOS_MAX_TIMEOUT) ? 0u : 1u, 1, 1);
    check("waitPeriodic MAX elapsed", _ticks - t0, RTOS_MAX_TIMEOUT - 1ul, RTOS_MAX_TIMEOUT);

    /* The microsecond clock across eight wraps of the 16 bit tick (audit F9). */
    check("getTimeUs across tick wraps", rtos_getTimeUs() - us0, (_ticks - ticks0) * 1000ul, (_ticks - ticks0) * 1000ul);

    printf("%s: test_timeout (%d checks, %d failures, %lu ticks)\n", _failures == 0 ? "PASS" : "FAIL",
           _checks, _failures, (unsigned long)_ticks);
    exit(_failures == 0 ? 0 : 1);
}

static void task2(void *arg)
{
    (void)arg;
    (void)rtos_semTake(&_t2Go, RTOS_FOREVER);   /* nothing of ours in the timed list before */
    {
        const uint32_t t0 = _ticks;
        rtos_delay(100);
        _t2Elapsed = _ticks - t0;
    }
    for(;;)
        rtos_delay(RTOS_FOREVER);
}

static void task1(void *arg)
{
    (void)arg;
    for(;;)
        rtos_delay(RTOS_FOREVER);
}

int main(void)
{
    rtos_init();
    rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
    rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
    rtos_createTask(T3, task3, NULL, _stack3, sizeof(_stack3));
    rtos_start();
    return 0;
}
