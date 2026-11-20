/**
 * @file rtos_dyntask.c
 * The dynamic task API (RTOS_CFG_DYNAMIC_TASKS): a task ends itself with rtos_exit, another
 * context reaps the terminated slot with rtos_reapTask, and rtos_startTask fills an unused
 * slot while the scheduler runs. The life cycle itself (the TERMINATED state, the deafness
 * of a terminated task, rtos_kernel_tcbReset as the one producer of UNUSED) is in
 * rtos_kernel.c and is there in every configuration; this file only adds the calls. The
 * rules and the reasons are in nornDocs design.md, "Task life cycle".
 *
 * These calls report errors as rtos_err_t instead of panicking, because their arguments
 * come from a running system (a shell, a supervisor) rather than from main(). What cannot
 * be reported back, a context that must not make the call at all, is still RTOS_CHECK.
 */

#include "rtos.h"
#include "rtos_assert.h"
#include "rtos_kernel.h"

#if RTOS_CFG_DYNAMIC_TASKS

/* A port that knows whether it is inside an interrupt defines RTOS_PORT_IN_ISR(); the
   others cannot tell and the rule is documented. */
#ifdef RTOS_PORT_IN_ISR
# define inIsr()    RTOS_PORT_IN_ISR()
#else
# define inIsr()    false
#endif


void rtos_exit(void)
{
    rtos_tcb_t * const pCur = rtos_kernel_current();

    RTOS_CHECK(!inIsr(), pCur->prio, RTOS_ERR_CONTEXT);
    RTOS_CHECK(pCur->prio != RTOS_IDLE_TASK, RTOS_IDLE_TASK, RTOS_ERR_ARG);

    /* Interrupts stay off from here: rtos_kernel_taskExit takes the critical section
       itself (nestable) and never returns. */
    (void)rtos_port_enterCritical();
#if RTOS_CFG_WATCHDOG
    rtos_kernel_wdtForget(pCur->prio);  /* an exit on purpose is not a missed check-in */
#endif
    rtos_kernel_taskExit();
}


rtos_err_t rtos_reapTask(uint8_t taskId)
{
    rtos_tcb_t *pT;
    rtos_port_ctx_t ctx;
    rtos_port_cs_t cs;

    if(inIsr())
        return RTOS_ERR_CONTEXT;
    if(taskId == RTOS_IDLE_TASK  ||  taskId >= RTOS_NUM_TASKS  ||  &rtos_tcbAry[taskId] == rtos_kernel_current())
        return RTOS_ERR_ARG;
    pT = &rtos_tcbAry[taskId];

    cs = rtos_port_enterCritical();
    if(pT->taskFunc == NULL  ||  pT->waitKind != RTOS_WAIT_TERMINATED)
    {
        rtos_port_exitCritical(cs);
        return RTOS_ERR_STATE;
    }
    /* What rtos_kernel_tcbReset relies on: a terminated task is in no list and owns
       nothing (the exit checked the mutexes). */
    RTOS_ASSERT((rtos_kernel_readyMask & pT->bit) == 0);
    RTOS_ASSERT((pT->flags & RTOS_TF_IN_TIMED_LIST) == 0  &&  pT->pWaiters == NULL);
    RTOS_ASSERT(pT->mutexHeld == 0);
    ctx = pT->ctx;
#if RTOS_CFG_WATCHDOG
    rtos_kernel_wdtForget(taskId);
#endif
    rtos_kernel_tcbReset(taskId);
    rtos_port_exitCritical(cs);

    /* Outside the critical section: the host port calls free() here. */
    rtos_port_freeStack(ctx);
    return RTOS_OK;
}


rtos_err_t rtos_startTask( uint8_t prio
                         , rtos_taskFunc_t taskFunc
                         , void *arg
                         , uint8_t *stackArea
                         , size_t stackSize
                         )
{
    rtos_port_cs_t cs;

    if(inIsr())
        return RTOS_ERR_CONTEXT;
    if(prio < 1  ||  prio >= RTOS_NUM_TASKS  ||  taskFunc == NULL  ||  stackArea == NULL)
        return RTOS_ERR_ARG;

    /* The same trimming rtos_createTask does, so that its checks are known to pass. */
    {
        const size_t misalign = (size_t)((uintptr_t)stackArea % RTOS_PORT_STACK_ALIGN);
        size_t usable = stackSize;
        if(misalign != 0)
        {
            const size_t skip = RTOS_PORT_STACK_ALIGN - misalign;
            if(usable <= skip)
                return RTOS_ERR_ARG;
            usable -= skip;
        }
        usable -= usable % RTOS_PORT_STACK_ALIGN;
        if(usable < RTOS_PORT_MIN_STACK)
            return RTOS_ERR_ARG;
    }

    cs = rtos_port_enterCritical();
    if(rtos_tcbAry[prio].taskFunc != NULL)
    {
        rtos_port_exitCritical(cs);
        return RTOS_ERR_BUSY;
    }
    rtos_createTask(prio, taskFunc, arg, stackArea, stackSize);
    rtos_kernel_schedule();             /* the new task may be the highest ready one */
    rtos_port_exitCritical(cs);
    return RTOS_OK;
}


const char *rtos_errName(rtos_err_t e)
{
    switch(e)
    {
    case RTOS_OK:           return "ok";
    case RTOS_ERR_BUSY:     return "busy";
    case RTOS_ERR_STATE:    return "state";
    case RTOS_ERR_ARG:      return "argument";
    case RTOS_ERR_CONTEXT:  return "context";
    default:                return "?";
    }
}

#else
typedef int rtos_dyntask_unused_t;
#endif  /* RTOS_CFG_DYNAMIC_TASKS */
