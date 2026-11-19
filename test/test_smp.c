/**
 * @file test_smp.c
 * The SMP kernel on the host port (nornInternal/nornCore/smp_plan.md, steps 3 and 4), built with 4, 3 and 2
 * cores (test_smp, test_smp3, test_smp2). Three tasks of different priority run in
 * parallel on the cores, woken by a simulated interrupt on core 0, by the tick and by a
 * mutex handed over between cores, while a hardware thread raises the tick and the
 * interrupt for core 0 (as a timer would, whatever core 0 runs) and the idle hook on
 * core 0 checks the scheduling invariant:
 *
 *   - rtos_start brings every other core to its idle loop (rtos_kernel_coresUp)
 *   - the running tasks are the highest ranked ready ones whenever no IPI is pending:
 *     no ready task that runs nowhere ranks above a task that runs (checked under the
 *     kernel lock from the idle hook, thousands of times)
 *   - a core never runs two tasks, a task never runs on two cores, idle bits never in
 *     the running mask
 *   - tasks really run at the same time (a counter of tasks inside their work loop
 *     reaches 2 or more) and migrate between cores (a task sees several core numbers)
 *   - the task woken from the interrupt on core 0 runs on another core when one is idle
 *   - a ceiling mutex locked from different cores never has two owners and its counter
 *     adds up
 *   - inside a critical section the lock is held by the core that runs the task
 *   - rtos_getTaskInfo names the core a running task is on; rtos_getCpuLoad (all cores)
 *     and rtos_getCoreLoad stay within 0..100, see the work, and read near 0 once the
 *     tasks have parked
 *   - with RTOS_CFG_SMP_AFFINITY (test_smpaff, test_smpaff2): T3 pinned to core 0 never runs
 *     elsewhere although other cores are idle, T2 pinned to core 1 stays there until its
 *     mask is widened half way, after which it runs on other cores too; the invariant
 *     counts only the cores a waiting task may run on
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "rtos.h"
#include "rtos_kernel.h"

#define T1 1u                           /* low: mostly running, the one that gets preempted */
#define T2 2u                           /* mid: wakes from the tick */
#define T3 3u                           /* high: woken from the interrupt on core 0 */
#define ROUNDS 300u

static uint8_t _stack1[16], _stack2[16], _stack3[16];
static rtos_sem_t _toT3 = RTOS_SEM_INIT_BINARY(0);
static rtos_mutex_t _mtx;
static unsigned _checks, _fails;
static unsigned long _idleLoops, _invariantChecks;
static volatile bool _done;             /* T3 has had its rounds */
static volatile bool _stop;             /* the tasks park themselves in a delay for ever */
static volatile bool _finished;         /* the hardware thread may leave */

/* Written by tasks on several cores at once: atomics. */
static volatile uint32_t _coreMask[RTOS_NUM_TASKS];     /* cores each task has run on */
static volatile uint32_t _inside, _maxInside;           /* tasks inside their work at once */
static volatile uint32_t _lockErrors, _mtxViolations, _mtxLocks[RTOS_NUM_TASKS];
static volatile uint32_t _violations, _rounds;
static volatile uint32_t _violationKind[8];
static char _firstViolation[200];
static uint32_t _loadMax, _loadMin = 100, _coreLoadMax, _loadReads;
#if RTOS_CFG_SMP_AFFINITY
static uint32_t _t2MaskWhenPinned;      /* the cores T2 had run on when its mask was widened */
static bool _widened;
#endif
static volatile uint8_t _mtxHolder = RTOS_NO_TASK;
static uint32_t _mtxCounter;                            /* protected by the mutex */

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

static void atomicOr(volatile uint32_t *p, uint32_t v) { (void)__atomic_fetch_or(p, v, __ATOMIC_RELAXED); }
static void atomicInc(volatile uint32_t *p) { (void)__atomic_fetch_add(p, 1u, __ATOMIC_RELAXED); }
static void atomicMax(volatile uint32_t *p, uint32_t v)
{
    uint32_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while(v > cur  &&  !__atomic_compare_exchange_n(p, &cur, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

/** The work every task does between its kernel calls: note the core, overlap with the
    others, hold the mutex for a moment. */
static void work(uint8_t me, unsigned spin)
{
    volatile unsigned n = spin;
    const uint32_t inside = __atomic_add_fetch(&_inside, 1u, __ATOMIC_RELAXED);
    atomicMax(&_maxInside, inside);
    atomicOr(&_coreMask[me], 1ul << rtos_port_coreId());
    while(n-- > 0)
        ;
    {
        const rtos_cs_t cs = rtos_enterCritical();
        if(rtos_host_lockOwner() != rtos_port_coreId())
            atomicInc(&_lockErrors);
        rtos_exitCritical(cs);
    }
    rtos_mutexLock(&_mtx);
    if(_mtxHolder != RTOS_NO_TASK)
        atomicInc(&_mtxViolations);
    _mtxHolder = me;
    ++ _mtxCounter;
    n = spin / 4u;
    while(n-- > 0)
        ;
    if(_mtxHolder != me)
        atomicInc(&_mtxViolations);
    _mtxHolder = RTOS_NO_TASK;
    rtos_mutexUnlock(&_mtx);
    atomicInc(&_mtxLocks[me]);
    (void)__atomic_sub_fetch(&_inside, 1u, __ATOMIC_RELAXED);
}

static void park(void)
{
    if(_stop)
        rtos_delay(RTOS_FOREVER);
}

static void task1(void *arg)
{
    unsigned i = 0;
    (void)arg;
    for(;;)
    {
        park();
        work(T1, 20000);
        if(++i % 40u == 0u)
            rtos_delay(1);
    }
}

static void task2(void *arg)
{
    (void)arg;
    for(;;)
    {
        rtos_delay(1);
        park();
        work(T2, 40000);
    }
}

static void task3(void *arg)
{
    (void)arg;
    for(;;)
    {
        (void)rtos_semTake(&_toT3, RTOS_FOREVER);
        park();
        work(T3, 30000);
        if(__atomic_add_fetch(&_rounds, 1u, __ATOMIC_RELAXED) >= ROUNDS)
            _done = true;
    }
}

static void isrGive(void)
{
    rtos_semGiveFromIsr(&_toT3);
}

/** The hardware: a tick every 200 us and the interrupt that wakes T3 every third tick,
    raised for core 0, until the test is done. */
static void hardware(void)
{
    unsigned n = 0;
    while(!_finished)
    {
        struct timespec ts = { 0, 200000L };
        (void)nanosleep(&ts, NULL);
        rtos_host_tick();
        if(++n % 3u == 0u  &&  !_stop)
            rtos_host_simulateIsr(isrGive);
    }
}

/* ---- the invariant, checked from the idle hook under the kernel lock ------------------ */

static bool ranksAbove(const rtos_tcb_t *pA, const rtos_tcb_t *pB)
{
    if(pA->effPrio != pB->effPrio)
        return pA->effPrio > pB->effPrio;
    if((pA->effPrio != pA->prio) != (pB->effPrio != pB->prio))
        return pA->effPrio != pA->prio;
    return pA->prio > pB->prio;
}

static void checkInvariant(void)
{
    const rtos_cs_t cs = rtos_enterCritical();
    const rtos_taskmask_t ready = rtos_kernel_getReadyMask();
    const rtos_taskmask_t running = rtos_kernel_getRunningMask();
    uint8_t c, t;
    bool pending = false;

    for(c = 0; c < RTOS_PORT_CORES; ++c)
        if(rtos_host_ipiPending(c)  ||  rtos_kernel_needSwitchOf[c])
            pending = true;
    if((running & 1u) != 0)
    {   atomicInc(&_violations); atomicInc(&_violationKind[0]); }   /* an idle bit as running */
    for(c = 0; c < RTOS_PORT_CORES; ++c)
    {
        const rtos_tcb_t * const pCur = rtos_kernel_pCurrentOf[c];
        uint8_t d;
        if(pCur->prio != RTOS_IDLE_TASK  &&  ((running & pCur->bit) == 0  ||  (ready & pCur->bit) == 0))
        {   atomicInc(&_violations); atomicInc(&_violationKind[1]); }   /* running but not in the masks */
        if(pCur->prio != RTOS_IDLE_TASK)
        {
            rtos_taskInfo_t info;                           /* the snapshot names the core */
            if(!rtos_getTaskInfo(pCur->prio, &info)  ||  info.core != c  ||  info.state != RTOS_TASK_RUNNING)
            {   atomicInc(&_violations); atomicInc(&_violationKind[2]); }
        }
        for(d = (uint8_t)(c + 1u); d < RTOS_PORT_CORES; ++d)
            if(rtos_kernel_pCurrentOf[d] == pCur)
            {   atomicInc(&_violations); atomicInc(&_violationKind[3]); }   /* one task on two cores */
    }
    if(!pending)
    {
        for(t = 1; t < RTOS_NUM_TASKS; ++t)
        {
            const rtos_tcb_t * const pT = &rtos_tcbAry[t];
            if((ready & pT->bit) == 0  ||  (running & pT->bit) != 0)
                continue;                                   /* not ready, or running */
            for(c = 0; c < RTOS_PORT_CORES; ++c)
            {
#if RTOS_CFG_SMP_AFFINITY
                if((rtos_getAffinity(t) & (1u << c)) == 0)
                    continue;                               /* it may not run there anyway */
#endif
                if(ranksAbove(pT, rtos_kernel_pCurrentOf[c]))
                {
                    atomicInc(&_violations); atomicInc(&_violationKind[4]);   /* a better task waits while a worse one runs */
                    if(_firstViolation[0] == 0)
                        snprintf(_firstViolation, sizeof _firstViolation, "task %u waits, core %u runs prio %u; ready %x running %x affinity %x widened %d",
                                 t, c, rtos_kernel_pCurrentOf[c]->prio, (unsigned)ready, (unsigned)running,
#if RTOS_CFG_SMP_AFFINITY
                                 rtos_getAffinity(t), (int)_widened
#else
                                 0xFF, 0
#endif
                                 );
                }
            }
        }
        ++ _invariantChecks;
    }
    rtos_exitCritical(cs);
}

/** True when every task is blocked without a timeout (its final delay, or T3 on the
    semaphore nobody gives any more) and no core runs one. */
static bool allParked(void)
{
    const rtos_cs_t cs = rtos_enterCritical();
    bool parked = rtos_kernel_getRunningMask() == 0;
    uint8_t t;
    for(t = 1; t < RTOS_NUM_TASKS; ++t)
    {
        rtos_taskInfo_t info;
        if(!rtos_getTaskInfo(t, &info)  ||  info.state != RTOS_TASK_BLOCKED  ||  info.hasTimeout)
            parked = false;
    }
    rtos_exitCritical(cs);
    return parked;
}

void test_idleHook(void)
{
    if(_done)
    {
        uint8_t t;
        uint32_t all = 0, locks = 0;
        _stop = true;                   /* the tasks park at their next loop; the counters settle */
        if(!allParked())
        {
            if(++_idleLoops > 20000000ul)
            {
                printf("FAIL: the tasks did not park (%lu rounds)\n", (unsigned long)_rounds);
                exit(1);
            }
            return;
        }
        _finished = true;
        CHECK(rtos_kernel_coresUp == RTOS_PORT_CORES);
        CHECK(_violations == 0);
        if(_violations != 0)
            printf("   violations by kind: idle-running %lu masks %lu core %lu two-cores %lu waits %lu; first: %s\n",
                   (unsigned long)_violationKind[0], (unsigned long)_violationKind[1], (unsigned long)_violationKind[2],
                   (unsigned long)_violationKind[3], (unsigned long)_violationKind[4], _firstViolation);
        CHECK(_invariantChecks > 100u);
        CHECK(_lockErrors == 0);
        CHECK(_mtxViolations == 0);
        for(t = 1; t < RTOS_NUM_TASKS; ++t) { all |= _coreMask[t]; locks += _mtxLocks[t]; }
        CHECK(_mtxCounter == locks);
        CHECK(locks >= 3u * ROUNDS / 2u);
        CHECK(_maxInside >= 2u);                            /* really parallel */
#if !RTOS_CFG_SMP_AFFINITY
        CHECK((_coreMask[T3] & ~1ul) != 0);                 /* woken on core 0, ran elsewhere */
#endif
        CHECK((all & ~1ul) != 0  &&  (all & 1ul) != 0);     /* core 0 and others ran tasks */
        CHECK((_coreMask[T1] & (_coreMask[T1] - 1ul)) != 0);    /* the low task migrated */
#if RTOS_CFG_SMP_AFFINITY
        CHECK(_coreMask[T3] == 1ul);                            /* pinned to core 0, never elsewhere */
        CHECK(_widened  &&  _t2MaskWhenPinned == (1ul << 1));   /* pinned to core 1 until widened */
        CHECK((_coreMask[T2] & ~(1ul << 1)) != 0);              /* then on other cores too */
        CHECK(rtos_getAffinity(T2) == (1u << RTOS_PORT_CORES) - 1u  &&  rtos_getAffinity(T3) == 1u
              &&  rtos_getAffinity(T1) == (1u << RTOS_PORT_CORES) - 1u);
#endif
        CHECK(_loadReads >= 10u  &&  _loadMax <= 100u  &&  _coreLoadMax <= 100u);
        CHECK(_loadMax > 0u  &&  _coreLoadMax > 0u);            /* the tasks were seen working */
        {
            volatile unsigned n = 2000000u;                 /* quiet now: a low total load */
            (void)rtos_getCpuLoad();
            while(n-- > 0) ;
            CHECK(rtos_getCpuLoad() <= 100u / RTOS_PORT_CORES + 5u);   /* only this idle loop is "busy": none */
        }
        printf("   cores %u: coreMask T1 %lx T2 %lx T3 %lx, max inside %lu, mutex locks %lu, invariant checks %lu, load %lu..%lu %% (core max %lu)\n",
               RTOS_PORT_CORES, (unsigned long)_coreMask[T1], (unsigned long)_coreMask[T2], (unsigned long)_coreMask[T3],
               (unsigned long)_maxInside, (unsigned long)locks, _invariantChecks,
               (unsigned long)_loadMin, (unsigned long)_loadMax, (unsigned long)_coreLoadMax);
        printf("%s: test_smp (%u checks, %u failures, %lu idle loops, %lu rounds)\n",
               _fails == 0 ? "PASS" : "FAIL", _checks, _fails, _idleLoops, (unsigned long)_rounds);
        rtos_host_joinHardware();
        exit(_fails == 0 ? 0 : 1);
    }
    if(++_idleLoops > 20000000ul)
    {
        printf("FAIL: runaway (%u checks, %u failures, %lu rounds)\n", _checks, _fails, (unsigned long)_rounds);
        exit(1);
    }
    checkInvariant();
#if RTOS_CFG_SMP_AFFINITY
    if(!_widened  &&  _rounds >= ROUNDS / 2u)
    {
        _t2MaskWhenPinned = _coreMask[T2];
        _widened = true;
        rtos_setAffinity(T2, (uint8_t)((1u << RTOS_PORT_CORES) - 1u));  /* from the idle task: allowed */
    }
#endif
    if((_idleLoops % 5000u) == 0u)
    {
        const uint32_t load = rtos_getCpuLoad();        /* an interval of some ms */
        uint8_t c;
        ++ _loadReads;
        if(load > _loadMax) _loadMax = load;
        if(load < _loadMin) _loadMin = load;
        for(c = 0; c < RTOS_PORT_CORES; ++c)
        {
            const uint32_t cl = rtos_getCoreLoad(c);
            if(cl > _coreLoadMax) _coreLoadMax = cl;
        }
    }
}

int main(void)
{
    rtos_init();
    CHECK(RTOS_CFG_SMP == 1  &&  RTOS_PORT_CORES >= 2);
    CHECK(rtos_kernel_pCurrentOf[0] == &rtos_tcbAry[RTOS_IDLE_TASK]);
    rtos_mutexInit(&_mtx, T3);
    rtos_createTask(T1, task1, NULL, _stack1, sizeof _stack1);
    rtos_createTask(T2, task2, NULL, _stack2, sizeof _stack2);
    rtos_createTask(T3, task3, NULL, _stack3, sizeof _stack3);
#if RTOS_CFG_SMP_AFFINITY
    CHECK(rtos_getAffinity(T1) == (1u << RTOS_PORT_CORES) - 1u);
    rtos_setAffinity(T3, 1u);                   /* the "driver": core 0 only */
    rtos_setAffinity(T2, 1u << 1);              /* core 1 only, widened half way */
#endif
    rtos_host_startHardware(hardware);
    rtos_start();
    return 0;
}
