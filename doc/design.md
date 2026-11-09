# nornOS design

A small real time kernel for microcontrollers: ATmega328P/2560 and ESP32 today. This
document records how it works and why, so that the choices can be revisited knowingly.

## Scheduling

Fixed priority, preemptive, one task per priority. The task id is its priority, 1 to
`RTOS_CFG_NUM_TASKS`; 0 is the idle task, which is the context that called `rtos_start`.
The scheduler is a bitmask of ready tasks and a highest-bit lookup: O(1), four to twenty
cycles depending on the CPU. There is no time slicing, deliberately: two tasks at the same
priority would need a per priority list and a quantum, and make the scheduling order
depend on history. A task runs until it blocks or a higher priority task becomes ready.

A task becomes ready in three ways: a timeout (the tick interrupt walks the head of a
list sorted by wake-up tick, so a tick costs the same whether 2 or 30 tasks sleep), an
object hand-over (semaphore token, queue item, mailbox item, pool block, event group bits,
written straight into the waiting task's variables by the giver, so the woken task finds
its result without touching the object again), or a notification or event posted to it.

Context switches happen in two places only: in task context when the running task blocks
or makes a higher task ready, and at the end of an interrupt handler (`rtos_isrTail`) when
the handler did. An interrupt handler therefore never switches in the middle; the port
guarantees the interrupt frame survives other tasks running in between.

## Mutual exclusion

The mutex follows the immediate priority ceiling protocol: locking raises the owner to
the mutex's ceiling at once, so no task that could ever want the mutex runs while it is
held, and the owner can never be preempted by a contender. With correct ceilings nobody
blocks on a mutex, a chain of mutexes cannot deadlock, and the worst case blocking time of
a task is one critical section of a lower task. If a ceiling is too low, or the owner
blocks while holding the mutex (allowed but discouraged), contenders fall back to waiting
and get the mutex handed over in priority order; the hand-over raises the new owner
immediately. A lock that would wait follows the chain of owners and panics on a cycle.

The ready mask is indexed by base priority, so a boosted task that is ready but not
running (an owner that blocked while holding, or a waiter that was just handed the
mutex) is not found by the highest ready bit. The kernel keeps a second mask of the
tasks whose effective priority differs from their base priority (rtos_kernel_boostedMask,
maintained by rtos_kernel_setEffPrio); the task picker (pickBoosted in rtos_kernel.c,
shared by the scheduler and the interrupt tail) scans those few tasks only while one of
them other than the running task is ready, and otherwise costs one mask test on top of
the highest bit lookup. The running task's own boost is covered by the fast path's
comparison, so a task that merely holds a mutex schedules at full speed (before this
exclusion the Mega paid about 190 cycles on every semaphore give made under a mutex).
The scan is one iteration per ready boosted task; nested mutexes on one task are one
bit.

The scheduling order is a contract, not an artefact of that scan: tasks are ordered by
the key (effective priority, boosted, base priority), compared lexicographically, where
boosted means the effective priority differs from the base priority. The picker takes
the greatest key, and a ready task preempts the running one only if its key is strictly
greater. So the highest effective priority wins; at an equal effective priority c the
owner raised to the ceiling c runs before the task whose base priority is c (a user of
the mutex, which would block on it, and the protocol promises that never happens with a
correct ceiling), and that owner's wake-up preempts such a task; and two owners boosted
to the same ceiling, which only happens when one of them blocked while holding, run in
base priority order. The base priority is unique, so the order is total. On the fast
path, where no ready task but possibly the current one is boosted, the highest ready
bit above the current task's effective priority is the same order. test/test_tie.c
checks every case against the trace.

Critical sections are short interrupt masks with the previous state restored, nestable.
There is no kernel lock besides them; every kernel operation runs with interrupts masked
for a bounded number of instructions, which is what gives the interrupt latency figures
in the README.

## Time

A fixed 1 kHz tick (configurable) drives delays, timeouts, periodic tasks and software
timers. `rtos_waitPeriodic` keeps a period base per task so periods do not drift; a
missed period is counted as an overrun and re-based rather than replayed. Software timers
are a sorted list served by the tick; their callbacks run in the tick interrupt unless the
timer task is configured, in which case they run in a kernel task of chosen priority.

Tickless idle (`RTOS_CFG_TICKLESS`) lets the idle task move the tick timer out to the next
deadline and sleep; the missed ticks are counted in one go on wake-up. Only the ESP32 port
implements it; the AVR ports sleep one tick at a time.

Run time statistics are taken at every context switch from a free running counter, so the
per task run times always add up to the elapsed time and `rtos_getCpuLoad` is exact.

## The hot path

Every blocking call and every interrupt that wakes a task goes through the same few
functions: make a task ready, pick the highest ready task, account the run time, switch.
Interrupts are masked from the call that decides to switch until the task switched to
leaves its own kernel call, so this path is both the context switch cost and the worst
interrupt latency; tc10 measures it in cycles. Rules that came out of measuring it on the
Mega (496 to 423 cycles per switch, 2026-11-04): the port's run time counter must be an
addition, not a multiplication (the AVR keeps a per tick base); the highest waiter comes
from the bit mask lookup, not a scan; `rtos_assertFailed` is `noreturn` so that the
compiler does not save registers for the failure branch; the run time accounting is
inlined into the two switch sites; the timed list removal is kept out of line because only
timed out or cancelled waits need it. Inlining the scheduler into the blocking path was
tried and rejected on the AVR: the register pressure cost more than the call. Each of
these is a few cycles; what remains is the register save itself (about 105 cycles on the
AVR, more on the ESP32 where the windows are spilled) and the configurable bookkeeping
(run time statistics about 45, assertions about 20).

## Memory

Everything is static: tasks, stacks and objects are declared by the application and
registered with the kernel; nothing is allocated behind the scenes. The memory pool gives
fixed blocks with O(1) alloc and free; the heap (`RTOS_CFG_HEAP`, off by default) is a
first fit allocator with coalescing for the cases that need one, deterministic in that it
does only what the caller asked and reports its statistics.

## Failure

One path: `rtos_panic`. Assertions, stack overflow (canary at the bottom of every stack,
checked at every switch and tick), the task watchdog, deadlock detection and CPU
exceptions all end there. The record survives a reset, the application's hook can print
or log before the halt or reset, and the next boot reads the record and the hardware's
reset cause. Queues, mailboxes, pools and semaphores count what they drop or overwrite.

## Ports

The kernel talks to hardware through `port.h` only (doc/porting.md). The AVR port is the
simplest: the ISR wrapper and the switch save what the ABI requires and nothing else. The
Xtensa port adds the register window spill, a lazy FPU context, the ROM loader image
format, the PLL and a second kernel instance on the other core (AMP) with a shared
memory link. The host port runs the kernel on Linux ucontext for deterministic unit tests
with simulated interrupts, which is where every primitive is specified by its trace.

## Task life cycle

The states are derived from the TCB, not stored: no byte and no store on the hot path.
This is the table the kernel implements, and the contract a future dynamic task API
builds on.

| State | Predicate | Entered by | Left by |
|---|---|---|---|
| UNUSED | `taskFunc == NULL`; the TCB is in its initial state (rtos_kernel_tcbReset) | rtos_init, a reap | rtos_createTask |
| READY | bit in the ready mask, not the running task | create, wake-up, preemption | pick, block, suspend |
| RUNNING | `rtos_pCurrent` (its bit stays in the ready mask) | pick | block, preemption, return |
| BLOCKED | bit not in the ready mask, `waitKind` says on what, RTOS_TF_IN_TIMED_LIST says if a timeout runs | a blocking call | wake-up, timeout |
| TERMINATED | `waitKind == RTOS_WAIT_TERMINATED`, bit not in the ready mask | the task function returning (rtos_kernel_taskExit) | a reap, from another context |

Suspended is a modifier on READY and BLOCKED, not a state: RTOS_TF_SUSPENDED keeps the
task off the ready mask and RTOS_TF_READY_PENDING remembers that a wake-up arrived
meanwhile. rtos_getTaskInfo reports it as RTOS_TASK_SUSPENDED, except for a terminated
task, which is final either way.

Rules, each checked by test/test_lifecycle.c:

* A terminated task is deaf: nothing makes it ready again. Every wake-up path tests the
  wait kind (events, event groups), a flag the task no longer has (notifications), a list
  it is no longer in (timeouts, waiter masks) or RTOS_TF_READY_PENDING (resume), and
  rtos_kernel_taskExit never calls rtos_kernel_blockCurrent, so the wait kind is never
  reset. Events and notifications posted at it accumulate harmlessly;
  rtos_broadcastEvents and the stack monitor still visit it; a watchdog monitor on it
  fires, as it should.
* A task must not return while holding a mutex: the owner would stay recorded for ever
  and every later lock would wait with no deadlock panic, since the detector only
  follows tasks blocked on mutexes. rtos_kernel_taskExit checks `effPrio == prio`
  (RTOS_PANIC_ARGUMENT), which catches every mutex with a ceiling above the task's
  priority; a ceiling equal to it is invisible without a per task count of held mutexes,
  which is the first thing to add when deletion arrives.
* UNUSED means reset, not just `taskFunc == NULL`. rtos_createTask fills only what it
  owns (function, argument, stack, context) and relies on the rest being in the initial
  state, so a reused slot that kept pending events, a suspended flag or a raised
  effective priority would hand them to the new task. rtos_kernel_tcbReset is the one
  place that produces the initial state; rtos_init runs it over every slot and a reap
  runs it over a terminated task.
* The stack of a terminated task is in use until another context runs. The task
  returns into rtos_kernel_taskExit on its own stack, and the switch away from it saves
  the context there (pushes on the AVR, the window spill on the Xtensa). So a task only
  ever terminates itself, and the reap, the reset and the freeing of the stack happen in
  a different context, after the switch: the idle task or the creator, never the dying
  task, and never a task that is READY, BLOCKED or RUNNING. The host port also has to
  release the ucontext and the stack it allocated in rtos_port_initStack.
* Creating a task while the scheduler runs needs a critical section around
  rtos_createTask, a schedule after it from task context (the new task may be the
  highest) and the switch request from an ISR; the stack fill loop under
  RTOS_CFG_STACK_CHECK makes creation from an ISR a bad idea. rtos_createTask itself
  stays as it is; the run-time variant wraps it.

## What is deliberately absent

Time slicing (see above), dynamic task creation and deletion at run time (static systems
do not need them, and deletion is the source of most use-after-free bugs in kernels; the
life cycle above is the preparation and doc/dynamic_tasks_plan.md the remaining steps), priority
inheritance (the ceiling protocol is simpler and has better bounds), SMP (the two ESP32
cores run two independent kernels instead: no locks in the scheduler, no cache
coherence questions, and a message link that is as fast as a shared memory copy).
