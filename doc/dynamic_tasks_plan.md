# Dynamic tasks: plan

nornCore is a static kernel: every task is created in main() before rtos_start() and
lives for ever. The team's code review of 2026-11-08 decided to prepare for dynamic
tasks (creation and deletion at run time) before adding any further scheduler feature,
and to do it by formalising the life cycle first. This document is the plan: what is
done, what remains, in which order, and the decisions still open. The rules the steps
build on are in doc/design.md, section "Task life cycle"; the contract for every step
is tests first, then the change, then a before/after measurement with tools/measure.sh
into doc/measurements/.

Status 2026-11-09: the preparation (steps A to C below) is done and measured. Steps D to
H are not started. Nothing of the public API has changed yet except that a task whose
function returns is now visibly TERMINATED.

## Goals and non-goals

Goals:

1. A task can end, and its slot and stack can be reused, without any new failure mode
   in a system that never uses the feature: the hot path and the static configuration
   stay as they are, to the cycle and to the byte, when the option is off.
2. Every state transition is derived from the TCB and checked by test_lifecycle: no
   state byte, no store on the hot path.
3. Deletion is self-termination plus a reap from another context. There is no kill of
   a READY, BLOCKED or RUNNING task from outside: that is the operation behind most
   use-after-free bugs in kernels (timed list, waiter masks, mutex ownership, the stack
   the victim is executing on) and nothing in the planned applications needs it.
4. Task id stays equal to priority and unique. A slot reused at run time gives the new
   task the priority of the slot. This keeps the ready mask, the boosted mask and the
   ceiling protocol untouched.

Non-goals: kill from outside (above), priority change at run time, more than
RTOS_NUM_TASKS tasks ever alive, a kernel-owned stack allocator on the AVR.

## Done

* **A. Scheduling order with a held mutex** (step 8, doc/measurements/step8-tie-*). The
  order is the key (effective priority, boosted, base priority); a reused slot inherits
  nothing of it because tcbReset clears the boost. test/test_tie.c.
* **B. The running task's own boost** (step 9, step9-curboost-*). A task holding a
  ceiling mutex no longer pays the slow path for its own bit. Not a life-cycle step, but
  it was found during A and a dynamic task will often hold a mutex at creation time.
* **C. The life cycle itself** (step 10, step10-lifecycle-after). RTOS_WAIT_TERMINATED
  and RTOS_TASK_TERMINATED, the check `effPrio == prio` in rtos_kernel_taskExit, and
  rtos_kernel_tcbReset(taskId) as the one producer of the UNUSED state, shared by
  rtos_init and the future reap. test/test_lifecycle.c covers termination, deafness,
  reset and re-creation under a critical section. Text +62 B AVR, +52 B ESP32, timing
  unchanged.

## Remaining steps, in order

Each step: extend test_lifecycle.c (or add a sibling) first, implement, run
`tools/measure.sh stepNN-<label>` before and after, host-cov, analyze and the full
regression including QEMU. Sizes are measured on uno, mega2560 and esp32; the hot-path
rows of tc10 must not move.

* **D. rtos_exit()**. A public call that terminates the running task without returning
  through its whole call chain: it does what the return into rtos_kernel_taskExit does
  (the same RTOS_CHECK, the same mark). One line of kernel code plus the documentation
  of the rule that a task can only ever terminate itself. Test: a task calling
  rtos_exit from a nested function is TERMINATED and deaf; calling it while holding a
  ceiling mutex panics; calling it from an ISR is an argument error (RTOS_CHECK on
  rtos_inIsr if the port provides it, otherwise documented as undefined).
* **E. Held-mutex count in the TCB**. One byte, incremented in lock and the handover,
  decremented in unlock. It closes the only hole in the exit check (a mutex whose
  ceiling equals the owner's priority raises nothing, so `effPrio == prio` cannot see
  it) and it lets the reap assert that a terminated task owns nothing. Cost to measure:
  the tc10 row "mutex lock+unlock" on both boards (expected +2 to +4 cycles on the
  Mega, +1 to +2 on the ESP32); TCB +1 byte per task. If the cost shows on the hot path
  the count goes under a configuration option that the dynamic task option selects.
  Test: lock a mutex with ceiling == prio and return: RTOS_PANIC_ARGUMENT.
* **F. Reap**. `rtos_deleteTask(taskId)` (name to decide, see below), callable from
  task context only, on a TERMINATED task only (RTOS_CHECK, never on self, never on
  READY, BLOCKED or RUNNING), under a critical section: a port hook
  `rtos_port_freeStack(ctx, stackArea, stackSize)` (the host port frees the ucontext
  and the malloc'd stack, AVR and Xtensa do nothing), then rtos_kernel_tcbReset. The
  slot is UNUSED afterwards and the stack belongs to the caller again. The stack
  monitor and the watchdog skip UNUSED slots (they already skip `taskFunc == NULL`;
  verify with a test). Test: the scenario 2 of test_lifecycle rewritten through the
  public call, plus the panics for the forbidden callers.
* **G. Run-time create**. rtos_createTask stays as it is (main() only, documented). A
  new `rtos_createTaskRunning(...)` (name to decide) wraps it: RTOS_CHECK that the
  slot is UNUSED, critical section, create, rtos_kernel_schedule, since the new task
  may be the highest. From an ISR it is not allowed (the stack fill under
  RTOS_CFG_STACK_CHECK is unbounded work for an ISR); a task that must be started from
  an ISR is started by a notification to a task that creates it. Test: create from a
  lower task preempts immediately; create from a higher task does not run the new task
  until the creator blocks; a stale event, a suspended flag or a raised priority in
  the old TCB never reach the new task (already in scenario 2, kept).
* **H. Configuration option and the gate**. D, F and G go under `RTOS_CFG_DYNAMIC_TASKS`
  (default 0) so a static system compiles exactly the step C kernel; the measurement
  of this step is the proof: sizes with the option off identical to step10-lifecycle-after
  on all three targets, hot path unchanged with it on. A board application (tc16 or a
  wider tc08) that creates and deletes a task in a loop on the Mega and the ESP32,
  under the watchdog and the stack monitor, run once as a soak. Then README and
  design.md: "What is deliberately absent" loses dynamic tasks and gains kill.

## Decisions wanted

* **Names.** rtos_exit, rtos_deleteTask (or rtos_reapTask: the call does not delete a
  running task, and the name should not promise that), rtos_createTaskRunning (or a
  flag on rtos_createTask; a separate function keeps the main()-only path free of the
  critical section and the check).
* **Who reaps.** The creator, after it sees RTOS_TASK_TERMINATED in rtos_getTaskInfo
  (polling, simplest, fits a supervisor task), or a notification from rtos_exit to a
  configured task (one more TCB field or a global), or the idle task reaping every
  terminated slot it finds (frees the stack without anyone asking, which is also the
  objection to it). Proposal: the creator, with polling; the idle variant later if an
  application asks.
* **Who owns the stack.** The caller, as today, in every port: no kernel allocator.
  With RTOS_CFG_HEAP an application can take the stack from the nornCore heap and give
  it back after the reap; the kernel does not do that for it.
* **Step E placement.** Always on (+1 byte per TCB, a few cycles on lock/unlock) or
  under the option. Decide from the measurement.
