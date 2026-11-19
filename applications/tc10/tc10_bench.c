/**
 * @file tc10_bench.c
 * Test case 10: the kernel's cost in CPU cycles on the current board, measured with the
 * cycle counter of hal/<arch>/board.h (a free running timer at F_CPU on the Mega, CCOUNT
 * on the ESP32) and a probe interrupt at PROBE_HZ, which is not a multiple of the tick so
 * that its phase sweeps the tick period.
 *
 *   api        cycles of one call pair without a task switch, e.g. semGive + semTake with
 *              no waiter: the price of the critical section and the bookkeeping. Minimum
 *              over the iterations, so an interrupt in between does not count.
 *   switch     taskLow signals taskHigh, which waits again at once: one round trip is
 *              two switches plus the signalling pair. Reported per switch, min and avg,
 *              for the semaphore, the notification, the queue and the event flags.
 *   isr        entry: hardware event (compare match) to the first line of the RTOS_ISR
 *              body. isr->task: from the give in the ISR body to the first instruction of
 *              the woken task after its take returns, through rtos_isrTail and the
 *              context switch. Both min/avg/max over every probe of the run, with the
 *              other benchmarks, the tick and the UART running at the same time, so the
 *              max shows the longest interrupt-off window of the kernel. hw->task is the
 *              sum of the two minima: the best case a driver can plan with.
 *   ram        sizes of the TCB and the objects, the smallest allowed stack, and how much
 *              stack each task of this program used (fill pattern high water mark).
 *   ceiling held  the api row "sem give+take, ceiling held" and the isr row "isr->task
 *              notify, ceiling held" repeat two measurements while taskLow holds a mutex
 *              whose ceiling is above its priority, i.e. while the running task is
 *              boosted: the scheduler's decision then involves the boosted mask. The
 *              isr row is taken in a 300 ms window in which taskLow spins holding the
 *              mutex, so the probes find it running (cpuLoad rises accordingly).
 *
 *   smp        in an SMP build (RTOS_CFG_SMP with RTOS_CFG_SMP_AFFINITY) every task above is
 *              pinned to core 0 first, so that the rows measure the same thing as on one
 *              core plus the kernel lock; then taskHigh moves to core 1 and the four
 *              primitives are measured across the cores: the time the signalling call
 *              takes on core 0 (it ends in an IPI, not a switch) and the time until
 *              taskHigh runs on core 1, once while core 1 is busy with a spinning task
 *              and once while it is asleep in its idle task. The probe's isr->task is
 *              repeated with taskWake on core 1, busy and asleep. Core 1's cycle counter
 *              is related to core 0's through a handshake calibrated offset (the two run
 *              from one clock, so the offset is constant); its spread is printed.
 *
 * The numbers are the point; the PASS limits only catch gross regressions. The README
 * carries the figures per board.
 */

#include "rtos.h"
#include "rtos_kernel.h"
#include "uart.h"
#include "board.h"
#include "testresult.h"

#if !BOARD_HAS_CYCLE_COUNTER
# error tc10 needs the cycle counter of hal/<arch>/board.h
#endif


/*
 * Defines
 */

#if RTOS_CFG_SMP
# if !RTOS_CFG_SMP_AFFINITY
#  error the SMP variant of tc10 needs RTOS_CFG_SMP_AFFINITY
# endif
# define TASK_WAKE      4       /* woken by the probe ISR */
# define TASK_HIGH      3       /* the switch partner */
# define TASK_LOW       2       /* runs the benchmarks and prints */
# define TASK_BUSY      1       /* core 1: spins, calibrates, or sleeps, as asked */
#else
# define TASK_WAKE      3       /* woken by the probe ISR */
# define TASK_HIGH      2       /* the switch partner */
# define TASK_LOW       1       /* runs the benchmarks and prints */
#endif

/* The probe period is dithered between PROBE_MIN_US and PROBE_MIN_US + 1000 us in 101
   steps, so that over a run the probes land at every phase of the 1 ms tick and of the
   benchmark loops; a fixed rate would repeat the same few phases (1250 Hz against 1 kHz:
   five). PROBE_HZ only sets the first period. */
#define PROBE_HZ        1250ul
#define PROBE_MIN_US    800ul
#define PROBE_STEPS     101ul
#define PROBE_COUNTS(us)  ((uint32_t)((uint64_t)BOARD_HIRES_TIMER_COUNT_HZ * (us) / 1000000ull))
#define PROBE_STEP      (PROBE_COUNTS(1000ul) / PROBE_STEPS)
#define PROBE_AVG_US    (PROBE_MIN_US + 500ul)
#if !BOARD_HIRES_TIMER_HZ_IS_EXACT(PROBE_HZ)
# error PROBE_HZ is not an exact rate of the probe timer
#endif

#define SWITCH_ROUNDS   500u
#define API_ITERS       64u
#define WAKE_ROUNDS     1000u   /* probes measured with the semaphore before the notification */

#ifndef TC10_STOP_AFTER_MS
# if RTOS_CFG_SMP
#  define TC10_STOP_AFTER_MS 6000ul     /* the cross core rows take two more seconds */
# else
#  define TC10_STOP_AFTER_MS 3000ul
# endif
#endif
#define CROSS_ROUNDS    300u    /* per primitive and per state of core 1 */
#define CROSS_ISR_ROUNDS 300u   /* probes per isr->task row across the cores */

#if RTOS_PORT_MIN_STACK > 192u
# define STACK_SIZE     (2u * RTOS_PORT_MIN_STACK)
#else
# define STACK_SIZE     192u
#endif

#define CYC(n)          ((uint32_t)(n) & BOARD_CYCLES_MASK)
#define NS_OF_CYCLES(c) ((uint32_t)(((uint64_t)(c) * 1000000000ull) / BOARD_CYCLES_HZ))
#define NS_OF_HIRES(c)  ((uint32_t)(((uint64_t)(c) * 1000000000ull) / BOARD_HIRES_TIMER_COUNT_HZ))

/* Limits for PASS, in ns: far above the expected values. An emulator (TC_TOLERANCE_PERMILLE
   raised for it, as tc04 does) stalls for milliseconds when its host is busy, so its
   latency limits are wider still; the numbers printed are what matters there, not the
   limits. */
#define LIMIT_SWITCH_NS     100000ul
#if TC_TOLERANCE_PERMILLE > 10u
# define LIMIT_ISR_ENTRY_NS 3000000ul
# define LIMIT_ISR_TASK_NS  3000000ul
#else
# define LIMIT_ISR_ENTRY_NS 300000ul
# define LIMIT_ISR_TASK_NS  300000ul
#endif

enum { MODE_SEM, MODE_NOTIFY, MODE_QUEUE, MODE_EVENTS };


/*
 * Local type definitions
 */

typedef struct stat
{
    uint32_t min, max, sum, n;
} stat_t;


/*
 * Data definitions
 */

static uint8_t _stackWake[STACK_SIZE];
static uint8_t _stackHigh[STACK_SIZE];
static uint8_t _stackLow[STACK_SIZE];

static rtos_sem_t _sem = RTOS_SEM_INIT(0);
static rtos_sem_t _wakeSem = RTOS_SEM_INIT(0);
static rtos_mutex_t _mutex = RTOS_MUTEX_INIT(TASK_LOW);
static rtos_mutex_t _ceil = RTOS_MUTEX_INIT(TASK_HIGH);    /* boosts taskLow while held */
static volatile uint8_t _ceilHeld = 0;      /* taskLow spins holding _ceil: isr row */
static uint8_t _qStorage[4];
static rtos_queue_t _queue = RTOS_QUEUE_INIT(_qStorage, 1, 4);
static uint8_t _mbStorage[2];
static rtos_mbox_t _mbox = RTOS_MBOX_INIT(_mbStorage, 2);
static uint8_t _poolStorage[4 * 8];
static rtos_pool_t _pool = RTOS_POOL_INIT(_poolStorage, 8, 4);
#if RTOS_CFG_SW_TIMERS
static rtos_timer_t _timer;
#endif
#if RTOS_CFG_EVGROUPS
static rtos_evgroup_t _evg = RTOS_EVGROUP_INIT(0);
#endif

static volatile uint8_t _highMode = MODE_SEM;
static volatile uint8_t _wakeMode = MODE_SEM;
static volatile uint32_t _wakeStamp;        /* cycle count taken in the probe ISR */
static volatile uint8_t _probeArmed = 0;

static stat_t _entry;                       /* probe counts */
static stat_t _wakeSemStat, _wakeNotifyStat;    /* cycles */
static stat_t _wakeCeilStat;                /* isr->task while the ceiling is held */
static stat_t _switchStat[4];               /* cycles per switch */
static uint32_t _api[16];                   /* cycles per call pair */
#if RTOS_CFG_STREAMS
static rtos_stream_t _stream;
static uint8_t _streamBuf[32];
#endif
static uint32_t _readCost;                  /* cycles of reading the counter itself */

#if RTOS_CFG_SMP
static uint8_t _stackBusy[STACK_SIZE];
enum { BUSY_SLEEP, BUSY_SPIN, BUSY_CAL };
static rtos_sem_t _busySem = RTOS_SEM_INIT(0);
static volatile uint8_t _busyMode = BUSY_SLEEP;
static volatile uint32_t _calReq, _calAck, _calCc1;     /* the handshake */
static int32_t _ccOffset;                   /* core 1's counter minus core 0's */
static uint32_t _ccSpread;                  /* max minus min of the 16 offsets taken */
static volatile uint32_t _highStamp;        /* core 1's counter when taskHigh runs */
static volatile uint8_t _highDone;          /* taskHigh is about to block again */
static stat_t _crossGive[2][4];             /* cycles of the signalling call, [busy][mode] */
static stat_t _crossWake[2][4];             /* core 0 cycles from the call to taskHigh running */
static stat_t _crossIsr[2][2];              /* isr->task on core 1: [busy][sem/notify] */
static volatile uint8_t _crossIsrRow = 0xFF;    /* which row taskWake fills, 0xFF: phase 1 rows, 0xFE: discard */
static volatile uint8_t _crossIsrBusy;
#endif


/*
 * Function implementation
 */

static void statInit(stat_t *pS)
{
    pS->min = 0xFFFFFFFFul;
    pS->max = 0;
    pS->sum = 0;
    pS->n = 0;
}

static void statAdd(stat_t *pS, uint32_t v)
{
    if(v < pS->min) pS->min = v;
    if(v > pS->max) pS->max = v;
    pS->sum += v;
    ++ pS->n;
}

static uint32_t statAvg(const stat_t *pS)
{
    return pS->n ? pS->sum / pS->n : 0;
}


/** This core's cycle counter expressed in core 0's: identity on one core, the
    calibrated offset taken off on core 1 (zero until the calibration ran). */
static inline uint32_t coreCycles(void)
{
#if RTOS_CFG_SMP
    if(rtos_port_coreId() != 0)
        return (uint32_t)((int32_t)board_cycles() - _ccOffset);
#endif
    return board_cycles();
}


/** The probe: entry latency, then wake TASK_WAKE and stamp the moment. */
RTOS_ISR(BOARD_HIRES_TIMER_VECT)
{
    const uint32_t e = board_hiresTimerElapsed();
    static uint16_t dither = 0;
    statAdd(&_entry, e);
    dither = (uint16_t)((dither + 37u) % PROBE_STEPS);        /* 37 and 101 are coprime */
    board_hiresTimerSetPeriod(PROBE_COUNTS(PROBE_MIN_US) + (uint32_t)dither * PROBE_STEP);
    if(_probeArmed)
    {
        const uint8_t mode = _wakeMode;
        _wakeStamp = board_cycles();
        if(mode == MODE_SEM)
            rtos_semGiveFromIsr(&_wakeSem);
        else
            rtos_notifyFromIsr(TASK_WAKE, 1, RTOS_NOTIFY_INC);
    }
}


static void taskWake(void *arg)
{
    uint32_t n = 0;
    (void)arg;
    statInit(&_wakeSemStat);
    statInit(&_wakeNotifyStat);
    _probeArmed = 1;
    for(;;)
    {
        uint32_t dt;
        if(_wakeMode == MODE_SEM)
        {
            (void)rtos_semTake(&_wakeSem, RTOS_FOREVER);
            dt = CYC(coreCycles() - _wakeStamp);
#if RTOS_CFG_SMP
            if(_crossIsrRow != 0xFF) { if(_crossIsrRow != 0xFE) statAdd(&_crossIsr[_crossIsrBusy][0], dt); continue; }
#endif
            statAdd(_ceilHeld ? &_wakeCeilStat : &_wakeSemStat, dt);
        }
        else
        {
            (void)rtos_notifyTake(RTOS_FOREVER);
            dt = CYC(coreCycles() - _wakeStamp);
#if RTOS_CFG_SMP
            if(_crossIsrRow != 0xFF) { if(_crossIsrRow != 0xFE) statAdd(&_crossIsr[_crossIsrBusy][1], dt); continue; }
#endif
            statAdd(_ceilHeld ? &_wakeCeilStat : &_wakeNotifyStat, dt);
        }
        if(++n == WAKE_ROUNDS)
            _wakeMode = MODE_NOTIFY;    /* a give that lands in between is harmless */
    }
}


static void taskHigh(void *arg)
{
    uint8_t item;
    (void)arg;
    for(;;)
    {
#if RTOS_CFG_SMP
        _highDone = 1;                      /* about to block: the next signal may come */
#endif
        switch(_highMode)
        {
        case MODE_SEM:      (void)rtos_semTake(&_sem, RTOS_FOREVER); break;
        case MODE_NOTIFY:   (void)rtos_notifyTake(RTOS_FOREVER); break;
        case MODE_QUEUE:    (void)rtos_queueReceive(&_queue, &item, RTOS_FOREVER); break;
        default:            (void)rtos_waitEvents(1, RTOS_WAIT_ANY, RTOS_FOREVER); break;
        }
#if RTOS_CFG_SMP
        _highStamp = coreCycles();          /* running: on whichever core */
#endif
    }
}


/** Signal taskHigh with the primitive of \a mode once (it runs, waits again, we are back). */
static void signalHigh(uint8_t mode)
{
    const uint8_t one = 1;
    switch(mode)
    {
    case MODE_SEM:      rtos_semGive(&_sem); break;
    case MODE_NOTIFY:   rtos_notify(TASK_HIGH, 1, RTOS_NOTIFY_INC); break;
    case MODE_QUEUE:    (void)rtos_queueSend(&_queue, &one, RTOS_NO_WAIT); break;
    default:            rtos_postEvents(TASK_HIGH, 1); break;
    }
}


static void benchSwitch(void)
{
    uint8_t mode, prev = MODE_SEM;
    for(mode=MODE_SEM; mode<=MODE_EVENTS; ++mode)
    {
        unsigned i;
        /* Move taskHigh to the new primitive: it is blocked in the previous one, so one
           signal with that one makes it loop round and block in the new one before we
           continue (it is the higher priority). */
        _highMode = mode;
        if(mode != MODE_SEM)
            signalHigh(prev);
        statInit(&_switchStat[mode]);
        for(i=0; i<SWITCH_ROUNDS; ++i)
        {
            const uint32_t t0 = board_cycles();
            signalHigh(mode);
            statAdd(&_switchStat[mode], CYC(board_cycles() - t0) / 2u);
        }
        prev = mode;
    }
}


#define MEASURE(slot, stmt)                                                     \
    do {                                                                        \
        unsigned i_;                                                            \
        uint32_t best_ = 0xFFFFFFFFul;                                          \
        for(i_=0; i_<API_ITERS; ++i_)                                           \
        {                                                                       \
            const uint32_t t0_ = board_cycles();                                \
            stmt;                                                               \
            {                                                                   \
                const uint32_t dt_ = CYC(board_cycles() - t0_);                 \
                if(dt_ < best_) best_ = dt_;                                    \
            }                                                                   \
        }                                                                       \
        _api[slot] = best_ > _readCost ? best_ - _readCost : 0;                 \
    } while(0)

static void benchApi(void)
{
    uint8_t item = 0, item2[2] = {0, 0};
    uint16_t v;
    void *pBlock;
    unsigned i;

    /* The cost of the measurement itself: two counter reads back to back. */
    _readCost = 0xFFFFFFFFul;
    for(i=0; i<API_ITERS; ++i)
    {
        const uint32_t t0 = board_cycles();
        const uint32_t dt = CYC(board_cycles() - t0);
        if(dt < _readCost) _readCost = dt;
    }

    MEASURE(0, { const rtos_port_cs_t cs = rtos_port_enterCritical(); rtos_port_exitCritical(cs); });
    MEASURE(1, (void)rtos_getTick());
    MEASURE(2, { rtos_semGive(&_sem); (void)rtos_semTake(&_sem, RTOS_NO_WAIT); });
    MEASURE(3, { rtos_notify(TASK_LOW, 1, RTOS_NOTIFY_SET); (void)rtos_notifyWait(&v, RTOS_NO_WAIT); });
    MEASURE(4, { rtos_mutexLock(&_mutex); rtos_mutexUnlock(&_mutex); });
    MEASURE(5, { (void)rtos_queueSend(&_queue, &item, RTOS_NO_WAIT); (void)rtos_queueReceive(&_queue, &item, RTOS_NO_WAIT); });
    MEASURE(6, { (void)rtos_mboxPost(&_mbox, item2); (void)rtos_mboxFetch(&_mbox, item2, RTOS_NO_WAIT); });
#if RTOS_CFG_STREAMS
    {
        uint8_t block[16] = {0};
        rtos_streamInit(&_stream, _streamBuf, sizeof _streamBuf, 1);
        MEASURE(15, { (void)rtos_streamWrite(&_stream, block, 16, RTOS_NO_WAIT); (void)rtos_streamRead(&_stream, block, 16, RTOS_NO_WAIT); });
    }
#endif
    MEASURE(7, { pBlock = rtos_poolAlloc(&_pool, RTOS_NO_WAIT); rtos_poolFree(&_pool, pBlock); });
#if RTOS_CFG_EVGROUPS
    MEASURE(8, { rtos_evgroupSet(&_evg, 1); (void)rtos_evgroupWait(&_evg, 1, RTOS_WAIT_ANY | RTOS_WAIT_CLEAR, RTOS_NO_WAIT); });
#endif
#if RTOS_CFG_SW_TIMERS
    MEASURE(9, { rtos_timerStart(&_timer, 100, 0); rtos_timerStop(&_timer); });
#endif
    MEASURE(10, { rtos_postEvents(TASK_LOW, 1); (void)rtos_waitEvents(1, RTOS_WAIT_ANY, RTOS_NO_WAIT); });
    MEASURE(11, (void)rtos_getStackFree(TASK_HIGH));
    MEASURE(12, (void)rtos_getTimeUs());
#if RTOS_CFG_RUNTIME_STATS
    MEASURE(13, (void)rtos_getCpuLoad());
#endif
    /* The same give+take as row 2 while we are boosted by a ceiling: the give's
       scheduling decision sees the running task in the boosted mask. */
    rtos_mutexLock(&_ceil);
    MEASURE(14, { rtos_semGive(&_sem); (void)rtos_semTake(&_sem, RTOS_NO_WAIT); });
    rtos_mutexUnlock(&_ceil);
}


/** isr->task while the running task is boosted: once the probes have moved to the
    notification and a few rounds are in, hold _ceil and spin for 300 ms so that every
    probe interrupts a boosted running task and the interrupt tail switches away from
    it to taskWake. */
static void benchIsrCeil(void)
{
    rtos_tick_t t0;

    while(_wakeMode != MODE_NOTIFY  ||  _wakeNotifyStat.n < 100u)
        rtos_delay(RTOS_MS(10));
    statInit(&_wakeCeilStat);
    rtos_mutexLock(&_ceil);
    _ceilHeld = 1;
    t0 = rtos_getTick();
    while((rtos_tick_t)(rtos_getTick() - t0) < RTOS_MS(300))
        ;
    _ceilHeld = 0;
    rtos_mutexUnlock(&_ceil);
}


#if RTOS_CFG_SMP
/** Core 1's helper: spins to keep the core awake, answers the calibration handshake,
    or sleeps on a semaphore so that core 1 falls into its idle task. */
static void taskBusy(void *arg)
{
    (void)arg;
    for(;;)
    {
        switch(_busyMode)
        {
        case BUSY_SPIN:
            break;
        case BUSY_CAL:
            if(_calReq != 0)
            {
                _calCc1 = board_cycles();
                __asm__ __volatile__ ("memw" : : : "memory");
                _calAck = 1;
                while(_calReq != 0)
                    ;
                _calAck = 0;
            }
            break;
        default:
            (void)rtos_semTake(&_busySem, RTOS_FOREVER);
            break;
        }
    }
}


/** Puts taskBusy into a mode; a give wakes it when it sleeps (a surplus give costs it one
    empty loop). */
static void setBusy(uint8_t mode)
{
    _busyMode = mode;
    rtos_semGive(&_busySem);
}


/** The offset of core 1's cycle counter against core 0's: 16 handshakes through memory,
    the round trip halved, the median taken. The two counters run from one clock. */
static void calibrateCounters(void)
{
    int32_t offs[16];
    uint32_t i, j;
    setBusy(BUSY_CAL);
    rtos_delay(RTOS_MS(2));                 /* taskBusy is in its loop on core 1 */
    for(i=0; i<16; ++i)
    {
        uint32_t t0, t1;
        __asm__ __volatile__ ("memw" : : : "memory");
        t0 = board_cycles();
        _calReq = 1;
        __asm__ __volatile__ ("memw" : : : "memory");
        while(_calAck == 0)
            ;
        t1 = board_cycles();
        offs[i] = (int32_t)_calCc1 - (int32_t)(t0 + (t1 - t0) / 2u);
        _calReq = 0;
        __asm__ __volatile__ ("memw" : : : "memory");
        while(_calAck != 0)
            ;
    }
    for(i=1; i<16; ++i)                     /* insertion sort */
        for(j=i; j>0 && offs[j-1] > offs[j]; --j)
        { const int32_t t = offs[j]; offs[j] = offs[j-1]; offs[j-1] = t; }
    _ccOffset = offs[8];
    _ccSpread = (uint32_t)(offs[15] - offs[0]);
    setBusy(BUSY_SLEEP);
}


/** taskHigh on core 1: the signalling call's own time on core 0 and the time until
    taskHigh runs over there, for every primitive, with core 1 in the given state. */
static void benchCross(uint8_t busy)
{
    uint8_t mode, prev = _highMode;         /* what taskHigh waits in right now */
    setBusy(busy ? BUSY_SPIN : BUSY_SLEEP);
    rtos_delay(RTOS_MS(2));
    for(mode=MODE_SEM; mode<=MODE_EVENTS; ++mode)
    {
        unsigned i;
        _highMode = mode;
        if(mode != prev)
        {
            _highDone = 0;
            signalHigh(prev);               /* taskHigh loops round into the new primitive */
            while(_highDone == 0)
                ;
            rtos_delay(RTOS_MS(1));
        }
        statInit(&_crossGive[busy][mode]);
        statInit(&_crossWake[busy][mode]);
        for(i=0; i<CROSS_ROUNDS; ++i)
        {
            uint32_t t0, t1, stamp;
            _highStamp = 0;
            _highDone = 0;
            __asm__ __volatile__ ("memw" : : : "memory");
            t0 = board_cycles();
            signalHigh(mode);
            t1 = board_cycles();
            while((stamp = _highStamp) == 0)
                ;
            statAdd(&_crossGive[busy][mode], CYC(t1 - t0));
            statAdd(&_crossWake[busy][mode], CYC(stamp - t0));
            while(_highDone == 0)           /* it is back at its wait */
                ;
            { volatile uint32_t spin = 400; while(spin-- > 0) ; }  /* and inside it */
        }
        prev = mode;
    }
    setBusy(BUSY_SLEEP);
}


/** The probe's isr->task with taskWake on core 1, busy and asleep, semaphore and
    notification: CROSS_ISR_ROUNDS probes each. */
static void benchCrossIsr(void)
{
    uint8_t busy, m;
    _crossIsrRow = 0xFE;                    /* from here on: nothing more into the phase 1 rows */
    rtos_setAffinity(TASK_WAKE, 1u << 1);
    for(busy=0; busy<2; ++busy)
    {
        setBusy(busy ? BUSY_SPIN : BUSY_SLEEP);
        for(m=0; m<2; ++m)
        {
            const uint8_t prev = _wakeMode;
            statInit(&_crossIsr[busy][m]);
            _wakeMode = m == 0 ? MODE_SEM : MODE_NOTIFY;
            if(prev == MODE_SEM) rtos_semGive(&_wakeSem);   /* out of the old wait, into the new */
            else rtos_notify(TASK_WAKE, 1, RTOS_NOTIFY_INC);
            rtos_delay(RTOS_MS(3));         /* the transition's samples are discarded */
            _crossIsrBusy = busy;
            _crossIsrRow = (uint8_t)(busy * 2u + m);
            while(_crossIsr[busy][m].n < CROSS_ISR_ROUNDS)
                rtos_delay(RTOS_MS(5));
            _crossIsrRow = 0xFE;
        }
    }
    setBusy(BUSY_SLEEP);
    rtos_setAffinity(TASK_WAKE, 1u);
}


static void benchSmp(void)
{
    calibrateCounters();
    rtos_setAffinity(TASK_HIGH, 1u << 1);   /* from here on taskHigh runs on core 1 */
    benchCross(1);
    benchCross(0);
    rtos_setAffinity(TASK_HIGH, 1u);
    benchCrossIsr();
}
#endif  /* RTOS_CFG_SMP */


static void putStat(const char *label, const stat_t *pS, bool hires)
{
    const uint32_t mn = hires ? NS_OF_HIRES(pS->min) : NS_OF_CYCLES(pS->min);
    const uint32_t av = hires ? NS_OF_HIRES(statAvg(pS)) : NS_OF_CYCLES(statAvg(pS));
    const uint32_t mx = hires ? NS_OF_HIRES(pS->max) : NS_OF_CYCLES(pS->max);
    uart_puts(label);
    uart_putU32(pS->min); uart_putc('/'); uart_putU32(statAvg(pS)); uart_putc('/'); uart_putU32(pS->max);
    uart_puts(hires ? " counts = " : " cycles = ");
    uart_putU32(mn); uart_putc('/'); uart_putU32(av); uart_putc('/'); uart_putU32(mx);
    uart_puts(" ns (n=");
    uart_putU32(pS->n);
    uart_puts(")\r\n");
}

#if RTOS_CFG_STACK_CHECK
static void putStackUsed(const char *name, uint8_t task)
{
    uart_puts(name);
    uart_putU32((uint32_t)(STACK_SIZE - rtos_getStackFree(task)));
}
#endif

static void report(void)
{
    static const char * const apiName[16] =
    {
        "critical section", "getTick", "sem give+take", "notify set+wait",
        "mutex lock+unlock", "queue send+recv", "mbox post+fetch", "pool alloc+free",
        "evgroup set+wait", "timer start+stop", "events post+wait", "getStackFree",
        "getTimeUs", "getCpuLoad", "sem give+take, ceiling held", "stream write+read 16 B",
    };
    static const char * const switchName[4] = { "sem", "notify", "queue", "events" };
    unsigned i;
    uint32_t hwToTask, ms;
    stat_t entry;

    /* Snapshot first: printing takes tens of milliseconds and the probe keeps counting. */
    _probeArmed = 0;
    {
        const rtos_port_cs_t cs = rtos_port_enterCritical();
        entry = _entry;
        ms = rtos_tickCount * (1000ul / RTOS_CFG_TICK_HZ);
        rtos_port_exitCritical(cs);
    }
    uart_puts("tc10 " BOARD_NAME " F_CPU=");  uart_putU32((uint32_t)F_CPU);
    uart_puts(" (cycles; 1 cycle = ");        uart_putU32(NS_OF_CYCLES(1000) );
    uart_puts(" ps)\r\n  api (one call pair, no switch, min of "); uart_putU32(API_ITERS);
    uart_puts("):\r\n");
    for(i=0; i<16; ++i)
    {
        if(_api[i] == 0  &&  i != 0)
            continue;
        uart_puts("    ");
        uart_puts(apiName[i]);
        uart_puts(": ");
        uart_putU32(_api[i]);
        uart_puts("\r\n");
    }
    uart_puts("  switch (per switch incl. signalling, min/avg over "); uart_putU32(SWITCH_ROUNDS);
    uart_puts(" round trips):\r\n");
    for(i=0; i<4; ++i)
    {
        uart_puts("    ");
        uart_puts(switchName[i]);
        uart_puts(": ");
        uart_putU32(_switchStat[i].min); uart_putc('/'); uart_putU32(statAvg(&_switchStat[i]));
        uart_puts(" cycles = "); uart_putU32(NS_OF_CYCLES(_switchStat[i].min));
        uart_putc('/'); uart_putU32(NS_OF_CYCLES(statAvg(&_switchStat[i])));
        uart_puts(" ns\r\n");
    }
    uart_puts("  isr (min/avg/max):\r\n");
    putStat("    entry:            ", &entry, true);
    putStat("    isr->task sem:    ", &_wakeSemStat, false);
    putStat("    isr->task notify: ", &_wakeNotifyStat, false);
    putStat("    isr->task notify, ceiling held: ", &_wakeCeilStat, false);
    hwToTask = NS_OF_HIRES(entry.min) + NS_OF_CYCLES(_wakeNotifyStat.min);
    uart_puts("    hw->task best:    "); uart_putU32(hwToTask); uart_puts(" ns\r\n");
    uart_puts("  ram (bytes):\r\n    tcb ");   uart_putU32((uint32_t)sizeof(rtos_tcb_t));
    uart_puts(" x ");                           uart_putU32(RTOS_NUM_TASKS);
    uart_puts(", sem ");                        uart_putU32((uint32_t)sizeof(rtos_sem_t));
    uart_puts(", mutex ");                      uart_putU32((uint32_t)sizeof(rtos_mutex_t));
    uart_puts(", queue ");                      uart_putU32((uint32_t)sizeof(rtos_queue_t));
    uart_puts(", mbox ");                       uart_putU32((uint32_t)sizeof(rtos_mbox_t));
    uart_puts(", pool ");                       uart_putU32((uint32_t)sizeof(rtos_pool_t));
#if RTOS_CFG_SW_TIMERS
    uart_puts(", timer ");                      uart_putU32((uint32_t)sizeof(rtos_timer_t));
#endif
#if RTOS_CFG_EVGROUPS
    uart_puts(", evgroup ");                    uart_putU32((uint32_t)sizeof(rtos_evgroup_t));
#endif
    uart_puts("\r\n    min stack ");            uart_putU32(RTOS_PORT_MIN_STACK);
#if RTOS_CFG_STACK_CHECK
    uart_puts(", stack used of ");              uart_putU32(STACK_SIZE);
    putStackUsed(": wake ", TASK_WAKE);
    putStackUsed(", high ", TASK_HIGH);
    putStackUsed(", low ", TASK_LOW);
#endif
    uart_puts("\r\n  cpuLoad ");                uart_putU32(rtos_getCpuLoad());
    uart_puts(" %\r\n");
#if RTOS_CFG_SMP
    uart_puts("  smp (rows above: every task on core 0; below: taskHigh / taskWake on core 1, core 0 cycles via an offset of ");
    uart_putU32((uint32_t)_ccOffset); uart_puts(" cycles, spread "); uart_putU32(_ccSpread); uart_puts("):\r\n");
    for(i=0; i<2; ++i)
    {
        unsigned m;
        uart_puts(i ? "    core 1 busy:   " : "    core 1 asleep: ");
        for(m=0; m<4; ++m)
        {
            uart_puts(switchName[m]); uart_puts(" give "); uart_putU32(_crossGive[i][m].min);
            uart_puts(" cyc, wake "); uart_putU32(NS_OF_CYCLES(_crossWake[i][m].min) / 1000u); uart_putc('/');
            uart_putU32(NS_OF_CYCLES(statAvg(&_crossWake[i][m])) / 1000u); uart_putc('/');
            uart_putU32(NS_OF_CYCLES(_crossWake[i][m].max) / 1000u); uart_puts(" us");
            uart_puts(m < 3 ? "; " : "\r\n");
        }
    }
    putStat("    isr->task sem, core 1 asleep:    ", &_crossIsr[0][0], false);
    putStat("    isr->task notify, core 1 asleep: ", &_crossIsr[0][1], false);
    putStat("    isr->task sem, core 1 busy:      ", &_crossIsr[1][0], false);
    putStat("    isr->task notify, core 1 busy:   ", &_crossIsr[1][1], false);
    uart_puts("    ipi sent "); uart_putU32(rtos_port_ipiSent[0]); uart_putc('/'); uart_putU32(rtos_port_ipiSent[1]);
    uart_puts(" taken "); uart_putU32(rtos_port_ipiTaken[0]); uart_putc('/'); uart_putU32(rtos_port_ipiTaken[1]);
    uart_puts("\r\n");
#endif

    tc_expectRange("switchSemNs", NS_OF_CYCLES(_switchStat[MODE_SEM].min), 1, LIMIT_SWITCH_NS);
    tc_expectRange("isrEntryMaxNs", NS_OF_HIRES(entry.max), 1, LIMIT_ISR_ENTRY_NS);
    tc_expectRange("isrTaskSemN", _wakeSemStat.n, WAKE_ROUNDS, WAKE_ROUNDS);
    tc_expectRange("isrTaskNotifyN", _wakeNotifyStat.n, 100, 0xFFFFFFFFul);
    tc_expectRange("isrTaskMaxNs", NS_OF_CYCLES(_wakeNotifyStat.max), 1, LIMIT_ISR_TASK_NS);
    tc_expectRange("isrTaskCeilN", _wakeCeilStat.n, 50, 0xFFFFFFFFul);
    tc_expectRange("probes", entry.n, (uint32_t)(ms * 1000ul / PROBE_AVG_US) * 9ul / 10ul,
                   (uint32_t)(ms * 1000ul / PROBE_AVG_US) * 11ul / 10ul);
#if RTOS_CFG_STACK_CHECK
    tc_expectRange("stackFreeLow", rtos_getStackFree(TASK_LOW), 16, STACK_SIZE);
#endif
#if RTOS_CFG_SMP
    tc_expectRange("ccSpread", _ccSpread, 0, 2000);
    tc_expectRange("crossWakeN", _crossWake[0][MODE_SEM].n, CROSS_ROUNDS, CROSS_ROUNDS);
    tc_expectRange("crossWakeSemMaxNs", NS_OF_CYCLES(_crossWake[1][MODE_SEM].max), 1, LIMIT_ISR_TASK_NS * 10ul);
    tc_expectRange("crossIsrNotifyN", _crossIsr[0][1].n, CROSS_ISR_ROUNDS, 0xFFFFFFFFul);
    tc_expectRange("crossIsrNotifyMaxNs", NS_OF_CYCLES(_crossIsr[1][1].max), 1, LIMIT_ISR_TASK_NS * 10ul);
#endif
    tc_finish("tc10");
}


#if RTOS_CFG_SW_TIMERS
static void timerNop(void *arg) { (void)arg; }
#endif

static void taskLow(void *arg)
{
    (void)arg;
#if RTOS_CFG_SW_TIMERS
    rtos_timerInit(&_timer, timerNop, NULL);
#endif
    rtos_poolInit(&_pool, _poolStorage, 8, 4);
    statInit(&_entry);
    rtos_delay(RTOS_MS(50));            /* let the others settle */
    benchSwitch();
    benchApi();
    benchIsrCeil();
#if RTOS_CFG_SMP
    benchSmp();
#endif
    for(;;)
    {
        rtos_delay(RTOS_MS(100));
#if TC10_STOP_AFTER_MS > 0
        if(rtos_getTick() >= RTOS_MS(TC10_STOP_AFTER_MS))
            report();
#endif
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
    uart_puts("\r\nnornOS " RTOS_VERSION " tc10_bench\r\n");

    rtos_init();
    rtos_createTask(TASK_WAKE, taskWake, NULL, _stackWake, sizeof(_stackWake));
    rtos_createTask(TASK_HIGH, taskHigh, NULL, _stackHigh, sizeof(_stackHigh));
    rtos_createTask(TASK_LOW,  taskLow,  NULL, _stackLow,  sizeof(_stackLow));
#if RTOS_CFG_SMP
    rtos_createTask(TASK_BUSY, taskBusy, NULL, _stackBusy, sizeof(_stackBusy));
    rtos_setAffinity(TASK_WAKE, 1u);    /* the rows of the one core kernel first: all on core 0 */
    rtos_setAffinity(TASK_HIGH, 1u);
    rtos_setAffinity(TASK_LOW,  1u);
    rtos_setAffinity(TASK_BUSY, 1u << 1);
#endif

    board_cycleCounterStart();
    board_hiresTimerStart(PROBE_HZ);
    rtos_start();
    return 0;
}
