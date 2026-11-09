/**
 * @file test_lifecycle.c
 * The end of a task's life, as a preparation for dynamic tasks: a task whose function
 * returns is TERMINATED, visibly and for good; it must not take a mutex with it; and a
 * terminated slot can be reset to UNUSED and created again without inheriting anything.
 * One child process per scenario, as test_checks does, because scenario 3 ends in a
 * panic.
 *
 *   1. T2 returns. rtos_getTaskInfo reports RTOS_TASK_TERMINATED with waitKind
 *      RTOS_WAIT_TERMINATED, no wait object, no timeout, base priority. Events, a
 *      notification, suspend, resume and a broadcast aimed at it change nothing, and it
 *      never runs again while ticks pass.
 *   2. T2 returns with a stale event posted at it afterwards. The slot is reset with
 *      rtos_kernel_tcbReset (what a future reap does once the dying task has been
 *      switched away from): the state is RTOS_TASK_NONE, and a new task created in the
 *      slot (in a critical section, followed by a schedule: what a run-time
 *      rtos_createTask will do) starts clean: no pending event, no overrun, base
 *      priority, and it terminates again when it returns.
 *   3. T1 returns while holding a mutex whose ceiling is above its priority. The owner
 *      would stay recorded for ever and every later lock would wait with no deadlock
 *      panic (the detector only follows mutex waits), so the exit is checked:
 *      RTOS_PANIC_ARGUMENT, nothing else.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "rtos.h"
#include "rtos_kernel.h"

#define T1 1u
#define T2 2u

static uint8_t _stack1[1], _stack2[1], _stack2b[1];
static int _scenario;
static volatile unsigned _t2Runs, _t2bRuns;
static volatile rtos_evt_t _t2bGot;
static volatile uint8_t _t2bEff;
static rtos_mutex_t _m = RTOS_MUTEX_INIT(T2);      /* ceiling above T1 */
static uint32_t _idleLoops;

static void fail(const char *what)
{
    printf("scenario %d FAIL: %s\n", _scenario, what);
    exit(1);
}

static void pass(void)
{
    printf("scenario %d PASS\n", _scenario);
    exit(0);
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    if(_scenario == 3  &&  pInfo->reason == RTOS_PANIC_ARGUMENT)
    {
        printf("scenario %d PASS: RTOS_PANIC_ARGUMENT (%lu, %lu)\n", _scenario,
               (unsigned long)pInfo->arg1, (unsigned long)pInfo->arg2);
        exit(0);
    }
    printf("scenario %d FAIL: panic reason %u (%lu, %lu)\n", _scenario, pInfo->reason,
           (unsigned long)pInfo->arg1, (unsigned long)pInfo->arg2);
    exit(1);
}

void test_idleHook(void)
{
    if(++_idleLoops > 100000ul)
        fail("runaway");
    rtos_host_tick();
}

/* ---- tasks --------------------------------------------------------------------------- */

static void task2(void *arg)
{
    (void)arg;
    ++ _t2Runs;
    /* returns: terminated */
}

static void task2b(void *arg)
{
    (void)arg;
    ++ _t2bRuns;
    _t2bGot = rtos_waitEvents(0x1, RTOS_WAIT_ANY, RTOS_NO_WAIT);
    {
        rtos_taskInfo_t info;
        rtos_getTaskInfo(T2, &info);
        _t2bEff = info.effPrio;
        if(info.overruns != 0)
            fail("recreated task inherited overruns");
    }
}

static void expectTerminated(const char *when)
{
    rtos_taskInfo_t info;
    if(!rtos_getTaskInfo(T2, &info))
        fail("getTaskInfo returned false");
    if(info.state != RTOS_TASK_TERMINATED)
    {
        printf("  state %u after %s\n", info.state, when);
        fail("state is not RTOS_TASK_TERMINATED");
    }
    if(info.waitKind != RTOS_WAIT_TERMINATED)
        fail("waitKind is not RTOS_WAIT_TERMINATED");
    if(info.pWaitObj != NULL  ||  info.hasTimeout)
        fail("terminated task has a wait object or a timeout");
    if(info.effPrio != info.prio  ||  info.prio != T2)
        fail("terminated task is not at its base priority");
}

static void task1(void *arg)
{
    (void)arg;
    switch(_scenario)
    {
    case 1:
        if(_t2Runs != 1)
            fail("T2 did not run first");
        expectTerminated("the return");

        /* Nothing wakes the dead. */
        rtos_postEvents(T2, 0x1);
        rtos_notify(T2, 1, RTOS_NOTIFY_SET);
        rtos_resume(T2);
        rtos_suspend(T2);
        expectTerminated("suspend");
        rtos_resume(T2);
        rtos_broadcastEvents(0x2);
        expectTerminated("the wake-up attempts");
        rtos_delay(3);                  /* ticks pass: still exactly one run */
        if(_t2Runs != 1)
            fail("T2 ran again");
        expectTerminated("three ticks");
        pass();
        break;

    case 2:
        if(_t2Runs != 1)
            fail("T2 did not run first");
        rtos_postEvents(T2, 0x1);       /* stale event in the dead task's TCB */
        expectTerminated("the return");
        {
            rtos_taskInfo_t info;
            const rtos_port_cs_t cs = rtos_port_enterCritical();
            rtos_kernel_tcbReset(T2);
            rtos_port_exitCritical(cs);
            rtos_getTaskInfo(T2, &info);
            if(info.state != RTOS_TASK_NONE)
                fail("reset slot is not RTOS_TASK_NONE");
        }
        {
            /* What a run-time create will do: create under the critical section, then
               let the scheduler run the new, higher task. */
            const rtos_port_cs_t cs = rtos_port_enterCritical();
            rtos_createTask(T2, task2b, NULL, _stack2b, sizeof(_stack2b));
            rtos_kernel_schedule();
            rtos_port_exitCritical(cs);
        }
        if(_t2bRuns != 1)
            fail("the recreated task did not run");
        if(_t2bGot != 0)
            fail("the recreated task saw the stale event");
        if(_t2bEff != T2)
            fail("the recreated task is not at its base priority");
        expectTerminated("the second return");
        pass();
        break;

    case 3:
        rtos_mutexLock(&_m);            /* raised to 2 */
        return;                         /* exit while holding: checked */

    default:
        fail("unknown scenario");
    }
}

static int runScenario(int n)
{
    _scenario = n;
    rtos_init();
    rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
    if(n != 3)
        rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
    rtos_start();
    return 1;
}

#define NUM_SCENARIOS 3

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
    printf("%s: test_lifecycle (%d scenarios, %d failures)\n", failures == 0 ? "PASS" : "FAIL",
           NUM_SCENARIOS, failures);
    return failures == 0 ? 0 : 1;
}
