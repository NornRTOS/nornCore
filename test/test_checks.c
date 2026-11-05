/**
 * @file test_checks.c
 * The mandatory checks (RTOS_CHECK) with assertions compiled out: every scenario hands
 * the kernel an argument that would corrupt memory and expects RTOS_PANIC_ARGUMENT,
 * nothing else. One child process per scenario, as test_robust does; a scenario that
 * returns, or panics for another reason, fails.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "rtos.h"

#define T1 1u
#define T2 2u

static uint8_t _stack1[16], _stack2[16];
static int _scenario;
static uint8_t _tiny[4];
static uint8_t _storage[32];
static uint8_t _heapArea[256];
static rtos_queue_t _q;
static rtos_mbox_t _mb;
static rtos_pool_t _pool;
static rtos_timer_t _tmr;
static uint8_t _foreign[64];

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    if(pInfo->reason == RTOS_PANIC_ARGUMENT)
    {
        printf("scenario %d PASS: RTOS_PANIC_ARGUMENT (%lu, %lu)\n", _scenario,
               (unsigned long)pInfo->arg1, (unsigned long)pInfo->arg2);
        exit(0);
    }
    printf("scenario %d FAIL: panic reason %u instead of ARGUMENT\n", _scenario, pInfo->reason);
    exit(1);
}

static void task1(void *arg)
{
    (void)arg;
    switch(_scenario)
    {
    case 3:  rtos_postEvents(RTOS_NUM_TASKS, 1); break;          /* id out of range */
    case 4:  rtos_notify(0, 1, RTOS_NOTIFY_SET); break;           /* the idle task */
    case 5:  (void)rtos_malloc(16); break;                        /* heap not initialized */
    case 6:  rtos_timerInit(&_tmr, NULL, NULL); break;            /* no callback */
    case 7:  rtos_queueInit(&_q, NULL, 2, 2); break;              /* no storage */
    case 8:  rtos_mboxInit(&_mb, _storage, 0); break;             /* no item size */
    case 9:  rtos_poolInit(&_pool, _storage, 8, 4); rtos_poolFree(&_pool, _foreign); break;
    case 10: rtos_heapInit(_heapArea, sizeof(_heapArea)); rtos_free(_foreign); break;
    case 11: rtos_wdtInit(100); rtos_wdtMonitor(RTOS_NUM_TASKS, 10); break;
    case 12: (void)rtos_getOverrunCount(RTOS_NUM_TASKS, false); break;   /* id used as an index */
    case 13: rtos_timerStart(&_tmr, 1, 0); break;                 /* never initialized */
    case 14: { rtos_poolInit(&_pool, _storage, 8, 4); { void *b = rtos_poolAlloc(&_pool, 0); rtos_poolFree(&_pool, b); rtos_poolFree(&_pool, b); } break; }
    default: break;
    }
    printf("scenario %d FAIL: the call returned\n", _scenario);
    exit(1);
}

static int runScenario(int n)
{
    _scenario = n;
    rtos_init();
    switch(n)
    {
    case 1:  rtos_createTask(RTOS_NUM_TASKS, task1, NULL, _stack1, sizeof(_stack1)); break;   /* prio out of range */
    case 2:  rtos_createTask(T1, task1, NULL, _tiny, 0); break;                                /* stack too small */
    case 15: rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
             rtos_createTask(T1, task1, NULL, _stack2, sizeof(_stack2)); break;                /* slot in use */
    default: rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1)); break;
    }
    rtos_start();
    return 1;
}

#define NUM_SCENARIOS 15

void test_idleHook(void)
{
    rtos_host_tick();
}

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
    printf("%s: test_checks (%d scenarios, %d failures)\n", failures == 0 ? "PASS" : "FAIL",
           NUM_SCENARIOS, failures);
    return failures == 0 ? 0 : 1;
}
