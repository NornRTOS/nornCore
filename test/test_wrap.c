/**
 * @file test_wrap.c
 * Tick counter wrap-around on the host port. With RTOS_CFG_TICK_BITS 16 the tick wraps
 * every 65.5 s of simulated time; the test starts the counter 50 ticks before the first
 * wrap and runs through four of them while every time based mechanism is in use. The
 * same source built as test_wrap32 (test/test_wrap32.c, 32 bit ticks) crosses the 2^32
 * wrap once.
 *
 *   T3  rtos_waitPeriodic(7): every wake-up must be exactly 7 ticks after the previous
 *       one (differences taken modulo 2^16) and never an overrun
 *   T2  rtos_delay(1..9) in a cycle, each measured; a semaphore take with timeout 5 that
 *       must time out after 5..6 ticks and never hang; a periodic software timer (13)
 *       counted; a one shot timer armed 25..40 ticks before each wrap and due 5 ticks after
 *       it; the task
 *       watchdog with a 50 tick deadline that it meets every loop
 *   T1  rtos_notifyWait with timeout 3, must return false after 3..4 ticks each time
 *
 * Two tasks always have wake-up times on both sides of a wrap, so the sorted timed list
 * and the tick comparison are exercised across it, not just the counter itself. The
 * expectations are the documented ones (rtos_delay: ticks..ticks+1, timeouts likewise,
 * periodic exact), and the counts at the end must match the elapsed simulated time.
 */

#include <stdio.h>
#include <stdlib.h>
#include "rtos.h"
#include "rtos_kernel.h"

#define T1 1u
#define T2 2u
#define T3 3u

#define TICKS           240000ul                /* simulated ticks to run */
#define START_TICK      ((rtos_tick_t)((rtos_tick_t)0 - (rtos_tick_t)50))   /* 50 ticks before the first wrap */
#if RTOS_CFG_TICK_BITS == 16
# define WRAPS_EXPECTED 3u                      /* 65536 per wrap */
# define TEST_NAME      "test_wrap"
#else
# define WRAPS_EXPECTED 1u                      /* only the one the start is placed before */
# define TEST_NAME      "test_wrap32"
#endif

static uint8_t _stack1[1], _stack2[1], _stack3[1];
static rtos_sem_t _sem = RTOS_SEM_INIT(0);
static rtos_timer_t _periodic, _oneShot;
static volatile unsigned _checks, _fails;
static uint32_t _elapsed;                       /* host ticks delivered */
static rtos_tick_t _lastTick;
static unsigned _wraps;
static volatile bool _done;

static volatile uint32_t _periodicWakes, _delays, _takes, _notifyWaits, _timerFires, _oneShotFires;
static volatile uint32_t _oneShotAcrossWrap;
static volatile rtos_tick_t _oneShotArmedAt, _oneShotFiredAt;

#define CHECK(cond)  check((cond), #cond, __LINE__)
static void check(bool ok, const char *what, int line)
{
    ++ _checks;
    if(!ok)
    {
        ++ _fails;
        if(_fails <= 20)
            printf("CHECK FAILED line %d at tick %u: %s\n", line, (unsigned)rtos_getTick(), what);
    }
}

/** Difference of two ticks, wrap safe by construction of the type. */
static rtos_tick_t since(rtos_tick_t then) { return (rtos_tick_t)(rtos_getTick() - then); }

static void periodicCb(void *arg) { (void)arg; ++ _timerFires; }
static void oneShotCb(void *arg)  { (void)arg; ++ _oneShotFires; _oneShotFiredAt = rtos_getTick(); }

static void task3(void *arg)
{
    rtos_tick_t last;
    (void)arg;
    (void)rtos_waitPeriodic(7);
    last = rtos_getTick();
    for(;;)
    {
        const bool overrun = rtos_waitPeriodic(7);
        CHECK(!overrun);
        CHECK(since(last) == 7);
        last = rtos_getTick();
        ++ _periodicWakes;
    }
}

static void task2(void *arg)
{
    unsigned d = 1;
    bool oneShotArmed = false;
    rtos_tick_t oneShotDelay = 0;
    (void)arg;

    rtos_timerInit(&_periodic, periodicCb, NULL);
    rtos_timerInit(&_oneShot, oneShotCb, NULL);
    rtos_timerStart(&_periodic, 13, 13);
    rtos_wdtInit(1000);
    rtos_wdtMonitor(T2, 50);
    for(;;)
    {
        rtos_tick_t t0 = rtos_getTick();
        rtos_tick_t dt;

        rtos_delay((rtos_tick_t)d);
        dt = since(t0);
        CHECK(dt == d  ||  dt == d + 1);
        ++ _delays;

        t0 = rtos_getTick();
        CHECK(!rtos_semTake(&_sem, 5));
        dt = since(t0);
        CHECK(dt == 5  ||  dt == 6);
        ++ _takes;

        CHECK(rtos_timerRemaining(&_periodic) <= 13);

        /* Arm the one shot 30 ticks before a wrap, once per wrap. */
        if(!oneShotArmed  &&  (rtos_tick_t)(0u - rtos_getTick()) <= 40u  &&  (rtos_tick_t)(0u - rtos_getTick()) > 25u)
        {
            /* Due 5 ticks after the wrap, whatever the distance to it is now. */
            _oneShotArmedAt = rtos_getTick();
            oneShotDelay = (rtos_tick_t)((rtos_tick_t)(0u - _oneShotArmedAt) + 5u);
            rtos_timerStart(&_oneShot, oneShotDelay, 0);
            oneShotArmed = true;
        }
        if(oneShotArmed  &&  !rtos_timerIsActive(&_oneShot))
        {
            const rtos_tick_t took = (rtos_tick_t)(_oneShotFiredAt - _oneShotArmedAt);
            CHECK(took == oneShotDelay  ||  took == oneShotDelay + 1u);
            CHECK(_oneShotFiredAt < _oneShotArmedAt);       /* armed before, fired after the wrap */
            if(_oneShotFiredAt < _oneShotArmedAt)
                ++ _oneShotAcrossWrap;
            oneShotArmed = false;
        }

        rtos_wdtCheckIn();
        d = d % 9u + 1u;
    }
}

static void task1(void *arg)
{
    (void)arg;
    for(;;)
    {
        const rtos_tick_t t0 = rtos_getTick();
        uint16_t v;
        rtos_tick_t dt;
        CHECK(!rtos_notifyWait(&v, 3));
        dt = since(t0);
        CHECK(dt == 3  ||  dt == 4);
        ++ _notifyWaits;
    }
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    printf("PANIC reason %u task %u at tick %u\n", pInfo->reason, pInfo->task, (unsigned)pInfo->tick);
    printf("FAIL: " TEST_NAME " (panic)\n");
    exit(3);
}

void test_idleHook(void)
{
    const rtos_tick_t now = rtos_getTick();
    if(now < _lastTick)
        ++ _wraps;
    _lastTick = now;
    if(_elapsed >= TICKS)
    {
        /* The periodic task wakes every 7 ticks, the timer every 13, T1 every 3..4,
           T2's loop takes 1..9 + 5..6 ticks; all over the whole run, within a percent. */
        CHECK(_wraps >= WRAPS_EXPECTED);
        CHECK(_periodicWakes >= TICKS / 7 - 3  &&  _periodicWakes <= TICKS / 7 + 1);
        CHECK(_timerFires >= TICKS / 13 - 3  &&  _timerFires <= TICKS / 13 + 1);
        CHECK(_notifyWaits >= TICKS / 4 - 3  &&  _notifyWaits <= TICKS / 3 + 1);
        CHECK(_delays >= TICKS / 15 - 3  &&  _delays <= TICKS / 6 + 1);
        CHECK(_oneShotFires >= WRAPS_EXPECTED);
        CHECK(_oneShotAcrossWrap == _oneShotFires);  /* every expiry crossed a wrap */
        CHECK(rtos_getOverrunCount(T3, false) == 0);
        printf("ticks %lu wraps %u periodic %lu timer %lu oneShot %lu (%lu across a wrap) delays %lu takes %lu notify %lu\n",
               (unsigned long)_elapsed, _wraps, (unsigned long)_periodicWakes, (unsigned long)_timerFires,
               (unsigned long)_oneShotFires, (unsigned long)_oneShotAcrossWrap, (unsigned long)_delays,
               (unsigned long)_takes, (unsigned long)_notifyWaits);
        printf("%s: " TEST_NAME " (%u checks, %u failures)\n", _fails == 0 ? "PASS" : "FAIL", _checks, _fails);
        exit(_fails == 0 ? 0 : 1);
    }
    ++ _elapsed;
    rtos_host_tick();
}

void rtos_assertFailed(const char *file, uint16_t line)
{
    printf("ASSERTION FAILED: %s line %u (tick %u)\n", file, line, (unsigned)rtos_getTick());
    exit(2);
}

int main(void)
{
    rtos_init();
    rtos_tickCount = START_TICK;
    _lastTick = START_TICK;
    rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
    rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
    rtos_createTask(T3, task3, NULL, _stack3, sizeof(_stack3));
    rtos_start();
    return 0;
}
