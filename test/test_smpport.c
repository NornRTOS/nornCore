/**
 * @file test_smpport.c
 * The host port with several cores (RTOS_HOST_CORES 4, nornInternal/nornCore/smp_plan.md step 2): the
 * primitives the SMP kernel will stand on, exercised with 2, 3 and 4 cores in turn, before
 * any kernel code knows about cores.
 *
 *   A  the kernel lock: every core increments one shared counter inside critical sections,
 *      nested ones too, while core 0 does the same from simulated interrupts; a holder
 *      variable catches two cores inside at once; the sum must be exact
 *   B  an IPI to a core inside a critical section is delivered when the section ends, not
 *      before, in interrupt context with interrupts masked
 *   C  an IPI to a core asleep in rtos_port_idleSleep wakes it
 *   D  many IPIs to a busy core coalesce: at least one delivery, never more than sent
 *   E  every thread sees its own core number
 *
 * The kernel is initialized but not started: rtos_isrTail at the end of a simulated
 * interrupt finds nothing to switch to.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rtos.h"

static void usleep(unsigned us)         /* nanosleep: POSIX 2008 has no usleep */
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    (void)nanosleep(&ts, NULL);
}

#define ITER        50000u
#define ISR_ITER    20000u
#define MAX_CORES   RTOS_PORT_CORES

static unsigned _checks, _fails;
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

/* Shared state. Everything the workers write is per core or atomic; the counter and the
   holder are the thing under test and are protected by the kernel lock only. */
static volatile uint32_t _counter;
static volatile uint8_t _holder = RTOS_NO_TASK;
static volatile uint32_t _violations[MAX_CORES];
static volatile uint32_t _ipiCount[MAX_CORES];
static volatile uint32_t _ipiBadContext;          /* handler without ISR context or with interrupts on */
static volatile uint8_t _seenCore[MAX_CORES];
static volatile int _phase;                       /* what the workers do now */
static volatile int _targetCore;                  /* phase B and C: which core acts */
static volatile int _inSection;                   /* phase B: the target is inside */
static volatile int _release;                     /* phase B: let it out */
static volatile int _woke;                        /* phase C: the target returned from sleep */
static volatile uint32_t _busyDelivered;          /* phase D */

static int load(volatile int *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store(volatile int *p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

/** Wait until *p == v, at most about 5 s; false on timeout. */
static bool waitFor(volatile int *p, int v)
{
    unsigned i;
    for(i = 0; i < 5000u; ++i)
    {
        if(load(p) == v)
            return true;
        usleep(1000);
    }
    return false;
}

static void spinAWhile(void)
{
    volatile unsigned n = 20;
    while(n-- > 0)
        ;
}

/** One protected increment with the exclusivity check; nested every 64th time. */
static void bump(uint8_t me, bool nest)
{
    const rtos_cs_t cs = rtos_enterCritical();
    rtos_cs_t inner = 0;
    if(_holder != RTOS_NO_TASK)
        ++ _violations[me];
    _holder = me;
    if(nest)
        inner = rtos_enterCritical();
    spinAWhile();
    ++ _counter;
    if(_holder != me)
        ++ _violations[me];
    if(nest)
        rtos_exitCritical(inner);
    if(_holder != me  ||  !rtos_host_interruptsDisabled())      /* the inner exit must not release */
        ++ _violations[me];
    _holder = RTOS_NO_TASK;
    rtos_exitCritical(cs);
}

static void isrBody(void)
{
    if(_holder != RTOS_NO_TASK)
        ++ _violations[0];
    _holder = 0;
    spinAWhile();
    ++ _counter;
    if(_holder != 0)
        ++ _violations[0];
    _holder = RTOS_NO_TASK;
}

void rtos_port_ipiHandler(void)
{
    const uint8_t me = rtos_port_coreId();
    if(!rtos_port_inIsr()  ||  !rtos_host_interruptsDisabled())
        __atomic_fetch_add(&_ipiBadContext, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&_ipiCount[me], 1u, __ATOMIC_RELAXED);
}

static void worker(void)
{
    const uint8_t me = rtos_port_coreId();
    uint32_t i;

    _seenCore[me] = 1;

    /* A: the lock. */
    while(load(&_phase) < 1) usleep(100);
    for(i = 0; i < ITER; ++i)
        bump(me, (i & 63u) == 0u);

    /* B: inside a critical section when the IPI comes. */
    while(load(&_phase) < 2) usleep(100);
    while(load(&_targetCore) != (int)me) { if(load(&_phase) > 2) break; usleep(100); }
    if(load(&_phase) == 2)
    {
        const rtos_cs_t cs = rtos_enterCritical();
        store(&_inSection, 1);
        while(load(&_release) == 0)
            ;                           /* holding the lock: core 0 must not need it here */
        rtos_exitCritical(cs);          /* the IPI lands here */
        store(&_inSection, 0);
    }

    /* C: asleep when the IPI comes. */
    while(load(&_phase) < 3) usleep(100);
    while(load(&_targetCore) != (int)me) { if(load(&_phase) > 3) break; usleep(100); }
    if(load(&_phase) == 3)
    {
        rtos_port_idleSleep();          /* returns after the IPI has been delivered */
        store(&_woke, 1);
    }

    /* D: busy in and out of critical sections while IPIs rain. */
    while(load(&_phase) < 4) usleep(100);
    while(load(&_phase) == 4)
    {
        const rtos_cs_t cs = rtos_enterCritical();
        spinAWhile();
        rtos_exitCritical(cs);
    }
}

static void runWith(uint8_t n)
{
    uint8_t c;
    uint32_t i, sum = 0;

    printf("-- %u cores\n", n);
    _counter = 0; _holder = RTOS_NO_TASK; _ipiBadContext = 0;
    memset((void *)_violations, 0, sizeof _violations);
    memset((void *)_ipiCount, 0, sizeof _ipiCount);
    memset((void *)_seenCore, 0, sizeof _seenCore);
    store(&_phase, 0); store(&_targetCore, -1); store(&_inSection, 0); store(&_release, 0); store(&_woke, 0);

    for(c = 1; c < n; ++c)
        rtos_port_startCore(c, worker);

    /* A */
    store(&_phase, 1);
    for(i = 0; i < ISR_ITER; ++i)
    {
        rtos_host_simulateIsr(isrBody);
        if((i & 7u) == 0u)
            bump(0, (i & 63u) == 0u);
    }
    /* Wait for the workers' part by the counter: n-1 workers times ITER plus ours. */
    {
        const uint32_t expected = (uint32_t)(n - 1u) * ITER + ISR_ITER + ISR_ITER / 8u;
        unsigned t;
        for(t = 0; t < 20000u  &&  _counter != expected; ++t)
            usleep(1000);
        CHECK(_counter == expected);
        printf("   A: counter %lu of %lu\n", (unsigned long)_counter, (unsigned long)expected);
    }
    for(c = 0; c < n; ++c)
        CHECK(_violations[c] == 0);
    CHECK(rtos_host_lockOwner() == RTOS_NO_TASK);

    /* B */
    store(&_phase, 2);
    for(c = 1; c < n; ++c)
    {
        store(&_inSection, 0); store(&_release, 0);
        store(&_targetCore, c);
        CHECK(waitFor(&_inSection, 1));
        rtos_port_ipi(c);
        usleep(20000);
        CHECK(_ipiCount[c] == 0);                   /* masked: not yet */
        store(&_release, 1);
        CHECK(waitFor(&_inSection, 0));
        CHECK(_ipiCount[c] == 1);                   /* delivered at the exit */
    }
    store(&_targetCore, -1);

    /* C */
    store(&_phase, 3);
    for(c = 1; c < n; ++c)
    {
        store(&_woke, 0);
        store(&_targetCore, c);
        usleep(20000);                              /* let it reach the sleep */
        CHECK(load(&_woke) == 0);
        rtos_port_ipi(c);
        CHECK(waitFor(&_woke, 1));
        CHECK(_ipiCount[c] == 2);
    }
    store(&_targetCore, -1);

    /* D */
    store(&_phase, 4);
    usleep(5000);
    for(i = 0; i < 1000u; ++i)
        for(c = 1; c < n; ++c)
            rtos_port_ipi(c);
    usleep(50000);
    store(&_phase, 5);
    for(c = 1; c < n; ++c)
    {
        rtos_host_joinCore(c);
        CHECK(_ipiCount[c] >= 3u  &&  _ipiCount[c] <= 1002u);
        sum += _ipiCount[c] - 2u;
    }
    printf("   D: %lu deliveries of %lu IPIs sent\n", (unsigned long)sum, (unsigned long)(1000u * (n - 1u)));

    /* E, and the IPI context. */
    CHECK(_seenCore[0] == 0);
    for(c = 1; c < n; ++c)
        CHECK(_seenCore[c] == 1);
    for(c = n; c < MAX_CORES; ++c)
        CHECK(_seenCore[c] == 0);
    CHECK(_ipiBadContext == 0);
    CHECK(rtos_port_coreId() == 0);
}

void test_idleHook(void)
{
}

int main(void)
{
    uint8_t n;

    rtos_init();                        /* kernel data sane; not started */
    CHECK(RTOS_PORT_CORES == 4);
    CHECK(RTOS_CFG_SMP == 0);
    for(n = 2; n <= MAX_CORES; ++n)
        runWith(n);

    printf("%s: test_smpport (%u checks, %u failures)\n", _fails == 0 ? "PASS" : "FAIL", _checks, _fails);
    return _fails == 0 ? 0 : 1;
}
