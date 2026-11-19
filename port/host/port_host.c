/**
 * @file port_host.c
 * Host port implementation on top of ucontext. The "CPU" is the test process, an interrupt
 * is a function the test runs explicitly, and the idle sleep returns immediately so the
 * idle hook can drive simulated time.
 *
 * With RTOS_HOST_CORES above 1 (nornInternal/nornCore/smp_plan.md, step 2) the process has one thread per
 * core. What is per core (the interrupt mask, the ISR depth, the core number) is thread
 * local, and a critical section is the core's interrupt mask plus the one kernel lock, a
 * spinlock taken when the mask goes on and released when it goes off again, so that a
 * kernel built for one core is protected across threads exactly where it masks
 * interrupts. An IPI is a flag per core, delivered as a simulated interrupt at the next
 * point where that core enables interrupts, or at once to a core asleep in
 * rtos_port_idleSleep. With RTOS_HOST_CORES 1 the lock and the thread code are compiled
 * out and the port is the single threaded one.
 */

#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <ucontext.h>

#include "rtos.h"                       /* port.h: RTOS_HOST_CORES */
#include "rtos_kernel.h"

#if RTOS_HOST_CORES > 1
#include <pthread.h>
#include <sched.h>
#endif

#define HOST_STACK_SIZE (256u * 1024u)

#if RTOS_HOST_CORES > 1
# define PORT_TLS   __thread
#else
# define PORT_TLS
#endif

/* ---- per core state --------------------------------------------------------------------- */

static PORT_TLS int _irqDisabled = 0;
static PORT_TLS int _inIsr = 0;

#if RTOS_HOST_CORES > 1

__thread uint8_t rtos_port_hostCore = 0;

#define ISR_QUEUE_LEN   16u

typedef struct hostCore
{
    pthread_t thread;
    pthread_mutex_t mutex;          /* with cond: the core's sleep in rtos_port_idleSleep */
    pthread_cond_t cond;
    volatile int ipiPending;
    void (*isrQueue[ISR_QUEUE_LEN])(void);  /* interrupts raised from the hardware thread */
    volatile unsigned qHead, qTail;
    void (*entry)(void);
    uint8_t id;
    bool started;
} hostCore_t;

static __thread int _isHardware = 0;    /* the test's hardware thread: it raises, never runs */

static hostCore_t _cores[RTOS_HOST_CORES];
static volatile int _kernelLock = 0;                /* 0 free, 1 held */
static volatile uint8_t _lockOwner = RTOS_NO_TASK;  /* diagnostics only */

static void lockAcquire(void)
{
    while(__atomic_exchange_n(&_kernelLock, 1, __ATOMIC_ACQUIRE) != 0)
        sched_yield();
    _lockOwner = rtos_port_hostCore;
}

static void lockRelease(void)
{
    _lockOwner = RTOS_NO_TASK;
    __atomic_store_n(&_kernelLock, 0, __ATOMIC_RELEASE);
}

static void deliverIpi(void);

#else

static inline void lockAcquire(void) { }
static inline void lockRelease(void) { }
static inline void deliverIpi(void) { }

#endif

uint8_t rtos_host_lockOwner(void)
{
#if RTOS_HOST_CORES > 1
    return _lockOwner;
#else
    return RTOS_NO_TASK;
#endif
}


/* ---- critical sections: the core's mask, and the lock while the mask is on ------------- */

rtos_port_cs_t rtos_port_enterCritical(void)
{
    const int s = _irqDisabled;
    if(!s)
        lockAcquire();
    _irqDisabled = 1;
    return s;
}

/** Interrupts back on for this core: release the lock, then take what came in meanwhile. */
static void enable(void)
{
    if(_irqDisabled)
    {
        _irqDisabled = 0;
        lockRelease();
        deliverIpi();
    }
}

void rtos_port_exitCritical(rtos_port_cs_t s)
{
    if(s)
        _irqDisabled = 1;               /* still inside an outer section */
    else
        enable();
}

void rtos_port_enableInterrupts(void)
{
    enable();
}

int rtos_host_interruptsDisabled(void)
{
    return _irqDisabled;
}

void rtos_port_idleSleep(void)
{
#if RTOS_HOST_CORES > 1
    hostCore_t * const c = &_cores[rtos_port_hostCore];
    if(c->id == 0)
    {
        deliverIpi();                   /* core 0 never sleeps: the idle hook drives the test */
        return;
    }
    pthread_mutex_lock(&c->mutex);
    while(!c->ipiPending  &&  c->qHead == c->qTail)
        pthread_cond_wait(&c->cond, &c->mutex);
    pthread_mutex_unlock(&c->mutex);
    deliverIpi();
#endif
}

void rtos_port_idleSleepTicks(rtos_tick_t ticks)
{
    (void)ticks;
}

void rtos_port_halt(void)
{
    fprintf(stderr, "rtos_port_halt: kernel halted\n");
    exit(2);
}

void rtos_port_reset(void)
{
    fprintf(stderr, "rtos_port_reset: kernel reset requested\n");
    exit(3);
}

uint8_t rtos_port_getResetCause(void)
{
    return RTOS_RESET_POWER_ON;
}


/* ---- contexts --------------------------------------------------------------------------- */

static ucontext_t *getCtx(rtos_tcb_t *pT)
{
    if(pT->ctx == NULL)
    {
        /* Idle task: adopt the current (main) context. */
        pT->ctx = calloc(1, sizeof(ucontext_t));
    }
    return (ucontext_t *)pT->ctx;
}

void rtos_port_switchContext(void)
{
    rtos_tcb_t * const pFrom = rtos_pCurrent;
    rtos_tcb_t * const pTo = rtos_pNext;

    if(!_irqDisabled)
    {
        fprintf(stderr, "rtos_port_switchContext called with interrupts enabled\n");
        exit(2);
    }
    rtos_pCurrent = pTo;
    swapcontext(getCtx(pFrom), getCtx(pTo));
}

rtos_port_ctx_t rtos_port_initStack(uint8_t *stackArea, size_t stackSize, void (*entry)(void))
{
    ucontext_t * const pCtx = calloc(1, sizeof(ucontext_t));

    (void)stackArea;
    (void)stackSize;
    getcontext(pCtx);
    pCtx->uc_stack.ss_sp = malloc(HOST_STACK_SIZE);
    pCtx->uc_stack.ss_size = HOST_STACK_SIZE;
    pCtx->uc_link = NULL;
    makecontext(pCtx, entry, 0);
    return pCtx;
}

void rtos_port_freeStack(rtos_port_ctx_t ctx)
{
    ucontext_t * const pCtx = (ucontext_t *)ctx;

    if(pCtx != NULL)
    {
        free(pCtx->uc_stack.ss_sp);
        free(pCtx);
    }
}

void rtos_port_startTimer(void)
{
}


/* ---- simulated interrupts ------------------------------------------------------------- */

bool rtos_port_inIsr(void)
{
    return _inIsr != 0;
}

void rtos_host_simulateIsr(void (*isrBody)(void))
{
    const int s = _irqDisabled;

#if RTOS_HOST_CORES > 1
    if(_isHardware)
    {
        rtos_host_raiseIsr(0, isrBody); /* the hardware thread: an interrupt for core 0 */
        return;
    }
#endif
    if(!s)
        lockAcquire();                  /* an interrupt masks and, across cores, locks */
    _irqDisabled = 1;
    ++ _inIsr;
    isrBody();
    -- _inIsr;              /* before the tail: it may switch to a task, which is not in the ISR */
    rtos_isrTail();
    if(s)
        _irqDisabled = 1;
    else
        enable();
}


/* ---- cores and IPIs (RTOS_HOST_CORES > 1) --------------------------------------------- */

#if RTOS_HOST_CORES > 1

static void *coreThread(void *arg)
{
    hostCore_t * const c = arg;
    rtos_port_hostCore = c->id;
    c->entry();
    return NULL;
}

void rtos_port_startCore(uint8_t core, void (*entry)(void))
{
    hostCore_t * const c = &_cores[core];

    if(core == 0  ||  core >= RTOS_HOST_CORES  ||  c->started)
    {
        fprintf(stderr, "rtos_port_startCore: core %u cannot be started\n", core);
        exit(2);
    }
    c->id = core;
    c->entry = entry;
    c->ipiPending = 0;
    pthread_mutex_init(&c->mutex, NULL);
    pthread_cond_init(&c->cond, NULL);
    c->started = true;
    if(pthread_create(&c->thread, NULL, coreThread, c) != 0)
    {
        fprintf(stderr, "rtos_port_startCore: pthread_create failed\n");
        exit(2);
    }
}

void rtos_host_joinCore(uint8_t core)
{
    hostCore_t * const c = &_cores[core];
    if(core == 0  ||  core >= RTOS_HOST_CORES  ||  !c->started)
        return;
    pthread_join(c->thread, NULL);
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->mutex);
    c->started = false;
}

void rtos_port_ipi(uint8_t core)
{
    hostCore_t * const c = &_cores[core];

    if(core >= RTOS_HOST_CORES)
        return;
    __atomic_store_n(&c->ipiPending, 1, __ATOMIC_RELEASE);
    if(core == 0  ||  !c->started)
        return;                         /* core 0 takes it at its next enable */
    pthread_mutex_lock(&c->mutex);      /* wake it if it sleeps */
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->mutex);
}

/** Take what is pending for this core, as simulated interrupts: the IPI, then the
    interrupts the hardware thread raised. Interrupts are enabled here; one arriving during
    a handler is taken by the handler's own enable, and the loops see nothing left. */
/** The IPI as an interrupt body: the flag is cleared inside, with the lock held, so that
    the IPI counts as pending (rtos_host_ipiPending) until its handler has run. */
static void ipiBody(void)
{
    __atomic_store_n(&_cores[rtos_port_hostCore].ipiPending, 0, __ATOMIC_RELEASE);
    rtos_port_ipiHandler();
}

static void deliverIpi(void)
{
    hostCore_t * const c = &_cores[rtos_port_hostCore];
    for(;;)
    {
        void (*body)(void) = NULL;
        while(__atomic_load_n(&c->ipiPending, __ATOMIC_ACQUIRE) != 0)
            rtos_host_simulateIsr(ipiBody);
        pthread_mutex_lock(&c->mutex);
        if(c->qHead != c->qTail)
        {
            body = c->isrQueue[c->qHead % ISR_QUEUE_LEN];
            ++ c->qHead;
        }
        pthread_mutex_unlock(&c->mutex);
        if(body == NULL)
            return;
        rtos_host_simulateIsr(body);
    }
}

void rtos_host_raiseIsr(uint8_t core, void (*isrBody)(void))
{
    hostCore_t * const c = &_cores[core];

    if(core >= RTOS_HOST_CORES)
        return;
    pthread_mutex_lock(&c->mutex);
    if(c->qTail - c->qHead >= ISR_QUEUE_LEN)
    {
        fprintf(stderr, "rtos_host_raiseIsr: core %u has %u interrupts waiting\n", core, ISR_QUEUE_LEN);
        exit(2);
    }
    c->isrQueue[c->qTail % ISR_QUEUE_LEN] = isrBody;
    ++ c->qTail;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->mutex);
}

static void *hardwareThread(void *arg)
{
    void (*fn)(void) = (void (*)(void))(uintptr_t)arg;
    _isHardware = 1;
    fn();
    return NULL;
}

static pthread_t _hardware;
static bool _hardwareStarted;

void rtos_host_startHardware(void (*fn)(void))
{
    if(_hardwareStarted  ||  pthread_create(&_hardware, NULL, hardwareThread, (void *)(uintptr_t)fn) != 0)
    {
        fprintf(stderr, "rtos_host_startHardware failed\n");
        exit(2);
    }
    _hardwareStarted = true;
}

void rtos_host_joinHardware(void)
{
    if(_hardwareStarted)
        pthread_join(_hardware, NULL);
    _hardwareStarted = false;
}

RTOS_PORT_WEAK void rtos_port_ipiHandler(void)
{
}

bool rtos_host_ipiPending(uint8_t core)
{
    return core < RTOS_HOST_CORES  &&  __atomic_load_n(&_cores[core].ipiPending, __ATOMIC_ACQUIRE) != 0;
}

#else

void rtos_port_startCore(uint8_t core, void (*entry)(void))
{
    (void)core; (void)entry;
    fprintf(stderr, "rtos_port_startCore: RTOS_HOST_CORES is 1\n");
    exit(2);
}

void rtos_host_joinCore(uint8_t core)
{
    (void)core;
}

void rtos_port_ipi(uint8_t core)
{
    (void)core;
}

RTOS_PORT_WEAK void rtos_port_ipiHandler(void)
{
}

bool rtos_host_ipiPending(uint8_t core)
{
    (void)core;
    return false;
}

void rtos_host_raiseIsr(uint8_t core, void (*isrBody)(void))
{
    (void)core;
    rtos_host_simulateIsr(isrBody);    /* one core, one thread: at once */
}

void rtos_host_startHardware(void (*fn)(void))
{
    (void)fn;
    fprintf(stderr, "rtos_host_startHardware: RTOS_HOST_CORES is 1\n");
    exit(2);
}

void rtos_host_joinHardware(void)
{
}

#endif  /* RTOS_HOST_CORES > 1 */


/* ---- time and the watchdog ------------------------------------------------------------ */

/* Simulated hardware watchdog: counts simulated ticks since the last kick. */
static uint32_t _wdtTimeout = 0;
static uint32_t _wdtCounter = 0;

void rtos_port_wdtStart(uint32_t timeoutMs)
{
    _wdtTimeout = (uint32_t)(timeoutMs * RTOS_CFG_TICK_HZ / 1000ul);
    _wdtCounter = 0;
}

void rtos_port_wdtKick(void)
{
    _wdtCounter = 0;
}

static volatile uint32_t _hostTicks;

static void tickBody(void)
{
    if(_wdtTimeout != 0  &&  ++_wdtCounter > _wdtTimeout)
    {
        fprintf(stderr, "host watchdog expired\n");
        rtos_port_reset();
    }
    ++ _hostTicks;
    rtos_tickFromIsr();
}

void rtos_host_tick(void)
{
#if RTOS_HOST_CORES > 1
    if(_isHardware)
    {
        rtos_host_raiseIsr(0, tickBody);
        return;
    }
#endif
    rtos_host_simulateIsr(tickBody);
}

/* Run time and microseconds come from the port's own 32 bit tick count, as on the AVR from
   its timer count: with 16 bit ticks the kernel's counter wraps every 65536 ticks. */
rtos_port_runtime_t rtos_port_getRunTimeCounter(void)
{
    return _hostTicks;
}

uint32_t rtos_getTimeUs(void)
{
    return _hostTicks * (1000000ul / RTOS_CFG_TICK_HZ);
}
