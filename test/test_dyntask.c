/**
 * @file test_dyntask.c
 * The dynamic task API (RTOS_CFG_DYNAMIC_TASKS) on the host port: rtos_exit, rtos_reapTask
 * and rtos_startTask, their error codes, and the two checks that panic. One child process
 * per scenario, as test_lifecycle does, because four scenarios end in a panic.
 *
 *   1. T2 locks and unlocks a mutex, then calls rtos_exit from a nested function: it is
 *      TERMINATED and deaf, and never runs again while ticks pass. T1 (lower) reaps it:
 *      RTOS_TASK_NONE. T1 then starts a new task in the slot: being higher, it runs
 *      before rtos_startTask returns, sees neither the stale event posted at the dead
 *      task nor anything else of it, returns, and is reaped again.
 *   2. T2 (higher) starts a task in slot 1: it does not run until T2 blocks. The idle
 *      task starts a task in slot 3 from the idle hook: it runs at once, exits, and the
 *      idle hook reaps it.
 *   3. Every error code: busy, argument, state and context (from a simulated interrupt),
 *      the names, and a reap of a task that terminated by returning.
 *   4. rtos_exit while holding a mutex whose ceiling equals the task's priority, which
 *      the effective priority cannot show: RTOS_PANIC_ARGUMENT with the held count.
 *   5. rtos_exit from the idle task: RTOS_PANIC_ARGUMENT.
 *   6. rtos_exit from a simulated interrupt: RTOS_PANIC_ARGUMENT.
 *   7. A monitored task calls rtos_exit: the monitor is dropped, ticks pass, no panic;
 *      the reap succeeds.
 *   8. A monitored task returns instead: the monitor stays and fires, as design.md says.
 *   9. The held count across a hand-over: T1 blocks while holding, T2 waits for the
 *      mutex and is handed it, unlocks and exits without a panic.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "rtos.h"
#include "rtos_kernel.h"

#define T1 1u
#define T2 2u
#define T3 3u

static uint8_t _stack1[1], _stack2[1], _stackDyn[1], _stackDyn3[1];
static int _scenario;
static volatile unsigned _t2Runs, _dynRuns, _dyn3Runs;
static volatile rtos_evt_t _dynGot;
static volatile uint8_t _dynEff;
static volatile int _idlePhase, _idleResult;
static rtos_mutex_t _mHigh = RTOS_MUTEX_INIT(T3);     /* ceiling above T2 */
static rtos_mutex_t _mEq = RTOS_MUTEX_INIT(T2);       /* ceiling equal to T2 */
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

static void expectErr(const char *what, rtos_err_t got, rtos_err_t want)
{
    if(got != want)
    {
        printf("  %s: got %s, want %s\n", what, rtos_errName(got), rtos_errName(want));
        fail("wrong error code");
    }
}

static void expectState(uint8_t task, uint8_t state, const char *when)
{
    rtos_taskInfo_t info;
    if(!rtos_getTaskInfo(task, &info))
        fail("getTaskInfo returned false");
    if(info.state != state)
    {
        printf("  task %u state %u, want %u, after %s\n", task, info.state, state, when);
        fail("wrong state");
    }
}

void rtos_panicHook(const rtos_panicInfo_t *pInfo)
{
    bool ok = false;
    switch(_scenario)
    {
    case 4: ok = pInfo->reason == RTOS_PANIC_ARGUMENT  &&  pInfo->arg1 == T2  &&  pInfo->arg2 == 1; break;
    case 5: ok = pInfo->reason == RTOS_PANIC_ARGUMENT  &&  pInfo->arg1 == RTOS_IDLE_TASK; break;
    case 6: ok = pInfo->reason == RTOS_PANIC_ARGUMENT  &&  pInfo->arg2 == RTOS_ERR_CONTEXT; break;
    case 8: ok = pInfo->reason == RTOS_PANIC_WATCHDOG  &&  pInfo->arg1 == T2; break;
    default: break;
    }
    printf("scenario %d %s: panic reason %u (%lu, %lu)\n", _scenario, ok ? "PASS" : "FAIL",
           pInfo->reason, (unsigned long)pInfo->arg1, (unsigned long)pInfo->arg2);
    exit(ok ? 0 : 1);
}

/* ---- dynamic tasks ------------------------------------------------------------------- */

static void dynTask(void *arg)
{
    (void)arg;
    ++ _dynRuns;
    _dynGot = rtos_waitEvents(0x1, RTOS_WAIT_ANY, RTOS_NO_WAIT);
    {
        rtos_taskInfo_t info;
        rtos_getTaskInfo(rtos_getCurrentTask(), &info);
        _dynEff = info.effPrio;
        if(info.overruns != 0)
            fail("restarted task inherited overruns");
    }
    /* returns: terminated */
}

static void dyn3Task(void *arg)
{
    (void)arg;
    ++ _dyn3Runs;
    rtos_exit();
}

void test_idleHook(void)
{
    if(++_idleLoops > 100000ul)
        fail("runaway");
    switch(_scenario)
    {
    case 2:
        if(_idlePhase == 1)
        {
            _idlePhase = 2;
            expectErr("startTask from idle", rtos_startTask(T3, dyn3Task, NULL, _stackDyn3, sizeof(_stackDyn3)), RTOS_OK);
            /* The new task is the highest: it ran and exited before we got back here. */
            if(_dyn3Runs != 1)
                fail("the task started from idle did not run at once");
            expectState(T3, RTOS_TASK_TERMINATED, "the exit");
            expectErr("reap from idle", rtos_reapTask(T3), RTOS_OK);
            expectState(T3, RTOS_TASK_NONE, "the reap");
            _idleResult = 1;
        }
        break;
    case 5:
        rtos_exit();                    /* checked: the idle task may not exit */
    default:
        break;
    }
    rtos_host_tick();
}

/* ---- the scripted tasks ---------------------------------------------------------------- */

static void nested(void)
{
    rtos_exit();
}

static void task2(void *arg)
{
    (void)arg;
    ++ _t2Runs;
    switch(_scenario)
    {
    case 1:
        rtos_mutexLock(&_mHigh);
        rtos_mutexUnlock(&_mHigh);      /* the count is back at 0 */
        nested();                       /* never returns */
        fail("rtos_exit returned");
        break;

    case 4:
        rtos_mutexLock(&_mEq);          /* ceiling == prio: effPrio cannot show it */
        rtos_exit();
        break;

    case 7:
        rtos_wdtMonitor(T2, 2);
        rtos_exit();
        break;

    case 8:
        rtos_wdtMonitor(T2, 2);
        return;                         /* the monitor stays: watchdog panic */

    case 9:
        rtos_mutexLock(&_mHigh);        /* T1 holds it and is blocked: we wait, then get the hand-over */
        rtos_mutexUnlock(&_mHigh);
        rtos_exit();
        break;

    default:
        break;
    }
}

static void isrStart(void)
{
    expectErr("startTask from ISR", rtos_startTask(T3, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_ERR_CONTEXT);
    expectErr("reapTask from ISR", rtos_reapTask(T1), RTOS_ERR_CONTEXT);
}

static void isrExit(void)
{
    rtos_exit();
}

static void task1(void *arg)
{
    (void)arg;
    switch(_scenario)
    {
    case 1:
        if(_t2Runs != 1)
            fail("T2 did not run first");
        expectState(T2, RTOS_TASK_TERMINATED, "rtos_exit");
        rtos_postEvents(T2, 0x1);       /* stale event in the dead task's TCB */
        rtos_notify(T2, 1, RTOS_NOTIFY_SET);
        rtos_resume(T2);
        expectState(T2, RTOS_TASK_TERMINATED, "the wake-up attempts");
        rtos_delay(3);
        if(_t2Runs != 1)
            fail("T2 ran again");
        expectErr("reap", rtos_reapTask(T2), RTOS_OK);
        expectState(T2, RTOS_TASK_NONE, "the reap");

        expectErr("start", rtos_startTask(T2, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_OK);
        /* Higher than us: it ran to its end before the call returned. */
        if(_dynRuns != 1)
            fail("the started task did not preempt");
        if(_dynGot != 0)
            fail("the started task saw the stale event");
        if(_dynEff != T2)
            fail("the started task is not at its base priority");
        expectState(T2, RTOS_TASK_TERMINATED, "the return");
        expectErr("second reap", rtos_reapTask(T2), RTOS_OK);
        pass();
        break;

    case 3:
        rtos_waitEvents(0x1, RTOS_WAIT_ANY, RTOS_FOREVER);
        return;                         /* terminated by returning */

    case 5:
        rtos_delay(RTOS_FOREVER);       /* let the idle task run */
        break;

    case 7:
        rtos_delay(10);                 /* ticks pass; the monitor was dropped at the exit */
        expectState(T2, RTOS_TASK_TERMINATED, "the exit");
        expectErr("reap", rtos_reapTask(T2), RTOS_OK);
        pass();
        break;

    case 8:
        rtos_delay(10);                 /* the watchdog fires meanwhile */
        fail("no watchdog panic for a monitored task that returned");
        break;

    case 9:
        rtos_mutexLock(&_mHigh);
        rtos_delay(2);                  /* blocked while holding: T2 waits */
        rtos_mutexUnlock(&_mHigh);      /* hand-over to T2, which exits */
        rtos_delay(2);
        expectState(T2, RTOS_TASK_TERMINATED, "the hand-over and the exit");
        expectErr("reap", rtos_reapTask(T2), RTOS_OK);
        pass();
        break;

    default:
        fail("unknown scenario for T1");
    }
}

/* Scenario 2 and 3 are scripted from T2, the higher task. */
static void task2script(void *arg)
{
    (void)arg;
    switch(_scenario)
    {
    case 2:
        expectErr("start lower", rtos_startTask(T1, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_OK);
        if(_dynRuns != 0)
            fail("a lower task ran before its creator blocked");
        rtos_delay(1);
        if(_dynRuns != 1)
            fail("the lower task did not run when its creator blocked");
        expectState(T1, RTOS_TASK_TERMINATED, "its return");
        _idlePhase = 1;
        rtos_delay(3);                  /* the idle hook starts and reaps a task meanwhile */
        if(_idleResult != 1)
            fail("the idle task did not start and reap");
        expectErr("reap lower", rtos_reapTask(T1), RTOS_OK);
        pass();
        break;

    case 3:
        expectErr("own slot", rtos_startTask(T2, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_ERR_BUSY);
        expectErr("live slot", rtos_startTask(T1, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_ERR_BUSY);
        expectErr("idle slot", rtos_startTask(0, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_ERR_ARG);
        expectErr("slot out of range", rtos_startTask(RTOS_NUM_TASKS, dynTask, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_ERR_ARG);
        expectErr("NULL function", rtos_startTask(T3, NULL, NULL, _stackDyn, sizeof(_stackDyn)), RTOS_ERR_ARG);
        expectErr("NULL stack", rtos_startTask(T3, dynTask, NULL, NULL, sizeof(_stackDyn)), RTOS_ERR_ARG);
        expectErr("tiny stack", rtos_startTask(T3, dynTask, NULL, _stackDyn, 0), RTOS_ERR_ARG);
        expectState(T3, RTOS_TASK_NONE, "the refused starts");

        expectErr("reap self", rtos_reapTask(T2), RTOS_ERR_ARG);
        expectErr("reap idle", rtos_reapTask(0), RTOS_ERR_ARG);
        expectErr("reap out of range", rtos_reapTask(RTOS_NUM_TASKS), RTOS_ERR_ARG);
        expectErr("reap unused", rtos_reapTask(T3), RTOS_ERR_STATE);
        expectErr("reap blocked", rtos_reapTask(T1), RTOS_ERR_STATE);
        rtos_host_simulateIsr(isrStart);

        if(strcmp(rtos_errName(RTOS_OK), "ok") != 0  ||  strcmp(rtos_errName(RTOS_ERR_BUSY), "busy") != 0
           ||  strcmp(rtos_errName(RTOS_ERR_STATE), "state") != 0  ||  strcmp(rtos_errName(RTOS_ERR_ARG), "argument") != 0
           ||  strcmp(rtos_errName(RTOS_ERR_CONTEXT), "context") != 0  ||  strcmp(rtos_errName((rtos_err_t)99), "?") != 0)
            fail("rtos_errName");

        rtos_postEvents(T1, 0x1);       /* T1 wakes, returns, terminates */
        rtos_delay(1);
        expectState(T1, RTOS_TASK_TERMINATED, "the return");
        expectErr("reap returned", rtos_reapTask(T1), RTOS_OK);
        expectErr("reap twice", rtos_reapTask(T1), RTOS_ERR_STATE);
        pass();
        break;

    case 6:
        rtos_host_simulateIsr(isrExit);
        fail("rtos_exit from an ISR returned");
        break;

    default:
        fail("unknown scenario for T2");
    }
}

static int runScenario(int n)
{
    _scenario = n;
    rtos_init();
    switch(n)
    {
    case 2:
    case 6:
        rtos_createTask(T2, task2script, NULL, _stack2, sizeof(_stack2));
        break;
    case 3:
        rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
        rtos_createTask(T2, task2script, NULL, _stack2, sizeof(_stack2));
        break;
    case 5:
        rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
        break;
    default:
        rtos_createTask(T1, task1, NULL, _stack1, sizeof(_stack1));
        rtos_createTask(T2, task2, NULL, _stack2, sizeof(_stack2));
        break;
    }
    rtos_start();
    return 1;
}

#define NUM_SCENARIOS 9

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
    printf("%s: test_dyntask (%d scenarios, %d failures)\n", failures == 0 ? "PASS" : "FAIL",
           NUM_SCENARIOS, failures);
    return failures == 0 ? 0 : 1;
}
