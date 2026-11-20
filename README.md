# nornOS

A small preemptive real time kernel for the ATmega2560 (Arduino Mega) and the ATmega328P
(Arduino UNO), written in C99 with the context switch in assembler. No dependency on the Arduino core; only avr-libc. Grew out
of the bTool brewing controller, whose application lives in applications/bTool.

## Licence

Business Source License 1.1 with the Additional Use Grant in LICENSE.md (Tenforward AB);
third-party components in THIRD_PARTY_LICENSES.md.

## Layout

    kernel/        portable kernel, C99, no architecture knowledge: scheduler, time, events
                   (rtos_kernel.c), semaphore and mutex (rtos_sync.c), queue and mailbox
                   (rtos_queue.c), task notification (rtos_notify.c), software timers
                   (rtos_timer.c), memory pool (rtos_pool.c), public API (rtos.h)
    port/<arch>/   everything the kernel needs from the CPU: port.h (critical sections,
                   ISR entry, port types and constants), context switch in assembler,
                   tick timer, stack preparation, and arch.mk with the toolchain, the
                   boards of that architecture and the image/sim/upload rules
    port/avr/      ATmega2560 and ATmega328P
    port/xtensa/   ESP32 (Xtensa LX6): port_start.S (entry), port_vectors.S (vector table
                   with the window handlers), port_context.S (context switch, interrupt
                   entry), port_esp32.c (interrupt matrix, CCOMPARE0 tick, time base),
                   esp32.ld (ROM loadable image, no bootloader)
    port/host/     Linux port on ucontext, used by the unit tests only; RTOS_HOST_CORES
                   gives it a thread per core, a kernel spinlock and IPIs (nornInternal/nornCore/smp_plan.md)
    hal/           drivers with one API for all architectures: uart.h (polled transmit,
                   polled or interrupt driven receive into a stream buffer, UART_CFG_RX_STREAM),
                   gpio.h, delay.h,
                   flash.h (two erase blocks of non-volatile memory for the application's
                   data: the top of the SPI flash on the ESP32, the EEPROM on the AVR),
                   log.h (log classes PANIC..DEBUG over the UART with a run time level),
                   hooks.c (panics and stack warnings on the console, RTOS_CFG_HAL_HOOKS),
                   hd44780 (LCD), dht11; hal/<arch>/ holds hal_arch.h (flash strings,
                   delays), gpio_arch.h, uart.c, flash.c and board.h (pin map per board);
                   hal/host/ is the console and hal_arch.h for the host unit tests
    config/        rtos_config.template.h, copy to the application as rtos_config.h
    test/          host unit tests of the kernel, one program per test_*.c
    applications/<app>/  one folder per application: rtos_config.h and app.mk (BOARD)

Adding an architecture means a new port/<arch>/ and hal/<arch>/ plus one line in the
Makefile mapping its boards to the architecture. The kernel and the generic HAL are not
touched; porting.md in the nornDocs repository describes the port interface.

## Build

    make APP=tc01             ELF/HEX in build/tc01-<board>, prints memory use
    make APP=tc01 BOARD=uno   build for another board than app.mk says
    make APP=tc01 sim         run in simavr, UART0 output on the terminal
    make APP=tc01 upload PORT=/dev/ttyACM0
    make host-test            kernel unit tests on the PC, with ASan and UBSan; a test
                              passes only if it exits 0 and ends with a PASS line
    tools/measure.sh LABEL    size and timing snapshot (tc01 size per board, tc10/tc11/
                              tc12 on the Mega, the ESP32 and QEMU) -> nornInternal/nornCore/measurements/
    make test-all             host tests, every tc in simavr for uno and mega2560, and on
                              an ESP32 at /dev/ttyUSB0 if present (tools/test_all.sh);
                              AVR_HW="mega2560:/dev/ttyACM0" adds a real AVR board

## Boards

    mega2560   ATmega2560, 16 MHz, 8 KB RAM. Tick on timer 3 by default (1, 4, 5 possible).
    uno        ATmega328P, 16 MHz, 2 KB RAM. Tick on timer 1, the only 16 bit timer.

The application picks its board in app.mk. Pins are written as BOARD_PIN_Dn from
hal/avr/board.h, which maps the Arduino numbering to the ports of the selected MCU. Timer
0 is never touched; timer 2 is left to the application (board_timer2StartCtc). The port
layer needs nothing per board: RAMPZ and the 3 byte return address are selected by the
compiler's MCU macros.

    esp32      ESP32-D0WDQ6, dual core Xtensa LX6, 520 KB RAM, 4 MB flash. NodeMCU-32S
               style devkit with CP2102 on /dev/ttyUSB0, LED on GPIO2. CPU_MHZ=240 by
               default from the PLL (port_clock.c); 40, 80 and 160 work as well.

The ESP32 image is loaded by the ROM directly into IRAM/DRAM from flash offset 0x1000; no
second stage bootloader, no ESP-IDF, no FreeRTOS. Code that does not fit the 128 KB of
IRAM can run from flash through the cache: the objects and archives an application or a
module names in FLASH_OBJS (and functions marked RTOS_PORT_FLASH_TEXT) go to a second
image at FLASH_BASE (0x80000), which `upload` writes too and `sim` places in the QEMU
flash; rtos_port_flashInit maps it through the flash MMU before main (esp32.ld,
port_esp32.c, tc09). The kernel, the port and interrupt handlers stay in IRAM. The
kernel runs on the PRO CPU; the APP
CPU is held in reset until a second nornOS instance gets a job there. Peripheral
interrupts are routed with rtos_port_attachInterrupt to level 1 CPU interrupts and
dispatched to RTOS_ISR bodies. A critical section masks PS.INTLEVEL 1 to 3
(PORT_INTLEVEL_CRITICAL in port_regs.h), so the latency figures below apply to those;
level 4, 5 and the NMI are never masked by the kernel and are free for an assembler
handler that does not call it (port_regs.h lists the levels of the CPU interrupts). A context switch spills every live register window first
(see port_context.S), so the kernel keeps the same model on both architectures: switch in
task context when blocking, or at the end of an interrupt. tc01, tc02 and tc03 run on the
board with the same counters as on the AVR; stacks need about 400 bytes more than on the
AVR because of the interrupt frame and the window spill areas. Toolchain: Espressif xtensa-esp-elf (crosstool-NG)
under ~/dev/tools, esptool in a venv there; both paths can be overridden with XTENSA_ROOT
and ESP_PYTHON. `make APP=hello upload` flashes, `make APP=hello monitor` resets the board
and shows UART0 (tools/monitor.py, Ctrl-C to leave). `make APP=tc01 BOARD=esp32 sim` runs
the image in Espressif's QEMU (QEMU_ESP, tools/qemu_run.py); its timers drift about a
percent against the CPU clock, so test builds for it use
EXTRA_CFLAGS=-DTC_TOLERANCE_PERMILLE=30. The single precision FPU is switched lazily
between tasks: only tasks that use floating point pay for it, and interrupt handlers must
not use it (see port/xtensa/port.h).

Both cores: an application can run a second nornOS image on the APP CPU (AMP, one kernel
instance per core, nothing shared but an 8 KB window). `AMP_CORE1 = <dir>` in app.mk names
a sub-application built with the core 1 memory layout (port/xtensa/mem_core1.ld) and
embedded in the core 0 image, which copies it into place and starts the APP CPU at boot
(rtos_port_startAppCpu). hal/xtensa/amp.c is the link: one ring buffer per direction in
the shared window, 16 byte messages, the receiver woken by the other core's FROM_CPU
interrupt. Several tasks of one core may receive: signals that arrive together merge
into one interrupt, so a receiver that takes a message and leaves more behind wakes the
next one. tc07 shows it: 200 requests per second answered by core 1, 6 us round trip,
then 20 bursts of two replies to two receivers of which the first stays busy.

The other way to use both cores is one kernel over them, `RTOS_CFG_SMP 1` in rtos_config.h
(off by default on two cores): the two highest ranked ready tasks run on the two cores,
one kernel lock inside every critical section, an IPI to the core that must give way,
and `rtos_setAffinity` (RTOS_CFG_SMP_AFFINITY) to pin a task to a core, for a driver whose
interrupt lives on one core (nornWIFI keeps itself on core 0 and needs the sub option). The one
thing to look at in an application written for one core: a lower priority task may now
run at the same instant, so a variable two tasks shared through priority alone needs a
mutex or a critical section. tc17 is the test, tc10 built with -DRTOS_CFG_SMP=1 the
benchmark (every row on core 0, then the same wake-ups across the cores); the cost is in
nornInternal/nornCore/measurements/step16-lock.md and step18-tc10-smp.md, the design in nornDocs design.md
"Several cores".

## Three configurations to start from

Only RTOS_CFG_NUM_TASKS is required in rtos_config.h; every other option has a default
in kernel/rtos_types.h, with a note on what it costs, and config/rtos_config.template.h
is the same list for copying. A one line rtos_config.h is therefore a complete
configuration (examples/hello has two: the tasks and RTOS_CFG_HAL_HOOKS). These three
sets are starting points measured with tc10 on the boards (cycles per context switch:
ESP32 at 240 MHz / Mega at 16 MHz):

    option                   small              default             large
    NUM_TASKS esp32/mega/uno 7 / 4 / 3          15 / 7 / 4          31 / 7 / -
    TICK_BITS esp32/avr      32 / 16            32 / 32 (uno 16)    32 / 32
    EVT_BITS esp32/avr       16 / 8             32 / 16 (uno 8)     32 / 16
    STACK_CHECK              1                  2 (canary)          2
    STACK_WARN               0                  1                   1
    RUNTIME_STATS            0                  1                   1
    ASSERT                   0                  1                   1
    EVGROUPS, SUSPEND        0                  1 (uno 0)           1
    HEAP, WATCHDOG           0                  esp32 1, avr 0      esp32 1, avr 0
    TRACE                    0                  0                   1
    TICKLESS (esp32)         0                  0                   1
    SW_TIMERS, NOTIFY        1 (uno: timers 0)  1                   1
    switch, esp32 / mega     347 / 315          440 / 472           472 / -
    meant for                the lean kernel    an ordinary app,    development and
                                                nornWIFI needs it   diagnosis; not the UNO

Common to all: TICK_HZ 1000, IDLE_SLEEP 1, PANIC_ACTION 0 (halt). TIMER_TASK_PRIO: the
kernel's default 0 runs timer callbacks in the tick interrupt, where every callback adds
to the interrupt latency of the whole system and may only use FromIsr functions; the
template therefore starts with a timer task (the top slot) on the Mega and the ESP32 and
keeps 0 on the UNO, where a task's stack does not fit. 0 is the optimisation for short
callbacks, not the starting point; nornWIFI's own callbacks are such and its applications
keep 0.
nornWIFI needs the heap, event groups and 32 event bits (default or large on the ESP32);
the original Mega bootloader does not survive a watchdog reset, hence no watchdog on
the AVR. The module tasks place themselves at the top by default: the nornWIFI radio at
RTOS_CFG_NUM_TASKS, its service task below it, the nornNET net task below those (or at
the top without the radio), nosh wherever the application puts it (1, the lowest, in
the examples). Each module says how many tasks it takes (NW_TASKS 2, NN_TASKS 1,
NOSH_TASKS 1), so an application with two tasks of its own, nosh, nornNET and nornWIFI
writes `#define RTOS_CFG_NUM_TASKS (2 + NOSH_TASKS + NN_TASKS + NW_TASKS)` or simply 6,
and numbers its own from 1.

The hooks an application may define are functions with weak defaults: rtos_idleHook
(RTOS_CFG_IDLE_HOOK as a macro still works), rtos_stackWarnHook, rtos_panicHook,
rtos_assertFailed, rtos_traceHook. The weak defaults are silent; RTOS_CFG_HAL_HOOKS 1
links hal/hooks.c instead, which prints a panic ("PANIC assert task 2 arg1 57 arg2 0 at
app.c line 57"), the failed assertion behind it and a stack warning on the console
through hal/log.h, so an application has readable panics from the first minute without
a hook of its own.

rtos_init passes RTOS_CONFIG_SIGNATURE, every layout changing option folded into one
number, to the kernel; an application compiled against another rtos_config.h than the
kernel objects it is linked with (a stale build directory, a library) panics at once
with RTOS_PANIC_CONFIG instead of corrupting memory.

## Model

* One task per priority. Task id = priority, 1..RTOS_CFG_NUM_TASKS, higher runs first.
  Task 0 is the implicit idle task (the context that called rtos_start). There is no
  time slicing and no round robin: a task runs until it blocks or a higher task becomes
  ready. Why, and how a design that would use round robin elsewhere is written here, is
  the section "One task per priority" below.
* 1 kHz tick from a 16 bit timer in CTC mode (RTOS_CFG_TIMER, default timer 3). 32 bit tick
  counter. rtos_waitPeriodic gives drift free periodic tasks with overrun detection.
* Blocking primitives: rtos_delay, rtos_waitPeriodic, rtos_waitEvents (per task event
  flags, any/all, timeout), rtos_semTake (counting semaphore), rtos_mutexLock,
  rtos_queueSend/Receive (FIFO of fixed size items, copied by value) and rtos_mboxFetch
  (one slot holding the latest item, a post overwrites).
* Queue and mailbox hand items directly to a blocked task, so a woken task never has to
  re-check or loop and no third task can take the item in between.
* Task notification: one 16 bit value per task, set/OR/increment from any task or ISR,
  rtos_notifyWait takes the value, rtos_notifyTake decrements it (semaphore without an
  object). Cheapest way to signal a task from an interrupt.
* Memory pool: fixed size blocks with an O(1) free list; a blocking allocation gets a
  released block handed over directly.
* SMP (RTOS_CFG_SMP, nornInternal/nornCore/smp_plan.md): one kernel over all cores as a choice, with the
  default from the port (RTOS_PORT_CORES: off up to two cores, so the ESP32 keeps AMP; on
  above). The N highest ranked ready tasks run on the N cores, one kernel lock, an IPI to
  the core that must give way, affinity as a sub option (RTOS_CFG_SMP_AFFINITY,
  rtos_setAffinity), load per core (rtos_getCoreLoad, rtos_taskInfo_t.core). Ports: the
  host (RTOS_HOST_CORES threads, for the tests) and the ESP32 (tc17; both cores run code
  from flash; the suite runs as SMP builds in Espressif's QEMU too). An SMP build's fatal
  report adds the registers and a backtrace (PORT_FATAL_TRACE_DEPTH). A critical section
  costs about 30 cycles more in an SMP build (a test-and-set lock with the fast path
  inline), a switch or a wake-up on the same core 2.7 times the one core figure (4.6 us
  against 1.7 at 240 MHz), a wake-up of a task on the other core 8 to 10 us from a task
  and 7.7 to 8.7 us from an interrupt; off, nothing changes (nornInternal/nornCore/measurements/step15-smp.md,
  step16-lock.md, step18-tc10-smp.md).
* Stream buffers (RTOS_CFG_STREAMS, off by default): a ring of bytes with writes and reads
  of any length, one writer (a task or an interrupt) and one reader, who is woken when a
  trigger level is in; for a UART driver's receive path or a frame queue. The bytes are
  copied with interrupts enabled (the writer owns the free part of the ring, the reader
  the filled part) and only the counts under the critical section, so a 1500 byte frame
  costs the interrupt latency nothing. Its own file; off, nothing of it is compiled.
* Run time statistics: at every context switch the tick timer is read and the elapsed
  time charged to the task that ran. rtos_getTaskRunTime gives the per task total,
  rtos_getCpuLoad the non-idle share since the previous call. Costs about 60 cycles per
  switch; disable with RTOS_CFG_RUNTIME_STATS 0. RTOS_CFG_RUNTIME_STATS 2 also counts the
  switches into each task (rtos_taskInfo_t.switches, nosh `sched`): one 16 bit increment
  per switch, 5 cycles on the ESP32, 2 bytes per task (absorbed by the TCB's padding there),
  and the locks that found a mutex held (rtos_mutexContended): 2 bytes per mutex, nothing
  on the uncontended path.
* Software timers: static rtos_timer_t objects, one shot or periodic without drift, in a
  list sorted by expiry. The callback runs in the tick interrupt with interrupts disabled,
  so keep it short and use *FromIsr functions or rtos_timerStart/Stop only.
* Timeouts, timer delays and periods, periodic waits and watchdog intervals are at most
  RTOS_MAX_TIMEOUT, half the tick range (32767 ticks with 16 bit ticks, 2^31 - 1 with
  32 bit ticks); the kernel compares ticks as signed differences so the counter may wrap.
  A larger value is RTOS_PANIC_ARGUMENT, except in rtos_delay, which splits it.
* Mutexes use the immediate priority ceiling protocol: the owner runs at the ceiling, so
  with a correct ceiling nobody ever blocks on the mutex and deadlock is impossible. An
  owner that blocks while holding (allowed, discouraged) keeps the ceiling and is
  scheduled by it when it wakes; so is a waiter the mutex is handed over to. At an equal
  effective priority the owner raised to the ceiling runs before the task whose base
  priority is the ceiling (the order is the key effective priority, boosted, base
  priority; design.md in nornDocs). The recipe for the ceiling: start every mutex with
  RTOS_MUTEX_CEILING_ANY (the highest priority; correct for any set of tasks, a library's
  and a dynamic task's included, and free, since the ceiling is a number in the mutex),
  and lower it only when a measurement shows the holder delaying a task with a deadline.
  A library takes its ceiling as a configuration macro with that default
  (STORE_CFG_MUTEX_CEILING). A ceiling that is too low is caught by an assertion at the
  first lock from above it; with assertions off the lock degrades to a plain blocking
  mutex and shows up in rtos_mutexContended / rtos_getMutexContended
  (RTOS_CFG_RUNTIME_STATS 2, the counting level; nosh sched prints the total), which stay
  0 in a correct system.
  Priority inheritance is deliberately absent: see design.md.
* A task whose function returns is terminated: RTOS_TASK_TERMINATED in rtos_getTaskInfo,
  never scheduled again, slot and stack kept. Returning while holding a ceiling mutex is
  a checked error (RTOS_PANIC_ARGUMENT). With RTOS_CFG_DYNAMIC_TASKS a task also ends
  with rtos_exit, the creator reaps the slot with rtos_reapTask once it is terminated,
  and rtos_startTask fills a slot while the system runs; these three return an rtos_err_t
  instead of panicking, since their arguments come from a running system. Nothing is ever
  killed from outside: a task that should stop is told and exits itself. Off (the
  default) the kernel is the static one byte for byte; on, it costs 1 byte per task and
  an increment per mutex lock and unlock (the count that catches an exit with a mutex of
  any ceiling). The life cycle and its rules are in design.md in nornDocs; the steps that
  led here and their measurements in nornInternal/nornCore/dynamic_tasks_plan.md.
* Interrupts that talk to the kernel are written with RTOS_ISR(vector) { ... } and use
  the *FromIsr functions; a task switch, if needed, happens once at the end of the ISR.
* Critical sections are short interrupt masks with the previous state saved (nestable):
  rtos_enterCritical and rtos_exitCritical, 3 cycles for the pair on the Mega, for a
  shared counter or flag that a task and an interrupt both touch.

## One task per priority, and no time slicing

Most kernels let several tasks share a priority and give them the CPU in turns, a
quantum each. nornCore does not, by design, and a reader coming from FreeRTOS or Zephyr
should know that before the first rtos_config.h.

**What it buys.** With one task per priority the set of ready tasks is one bit mask, the
scheduler is a highest-bit lookup (4 to 20 cycles depending on the CPU), the task id is
the priority (no TCB to find), the ceiling protocol is a comparison, and a woken task is
handed its item directly because there is exactly one waiter of that rank. That is the
whole switch of 408 cycles on the ESP32 and 440 on the Mega (tc10) and the kernel of
6912 bytes on the UNO (tc01). Equal priorities need a FIFO per level, a quantum counted
in every tick and a scheduling order that depends on history: a different hot path, not
a feature on top of this one, and one that makes the worst case harder to state.

**What it costs.** At most RTOS_CFG_NUM_TASKS tasks (31 on a 32 bit mask; 7 or 15 on
the AVR configurations), every task on its own slot, and modules that take slots by
convention (NW_TASKS, NN_TASKS, NOSH_TASKS, top down: see "Three configurations"). A task
that never blocks starves everything below it; with round robin it would starve only
half as fast, which is not a design either.

**How a round robin design is written here.** The pattern below is typical of what people
build with equal priorities: a few identical workers that serve requests as they come, a
periodic sampler and a logger, "all at the same priority so that none hogs the CPU".

    typical elsewhere                           here
    ------------------------------------------  ------------------------------------------
    3 worker tasks, same priority, time sliced   1 worker task and a queue of jobs
    sampler, same priority, sleeps 10 ms         sampler one slot above, rtos_waitPeriodic
    logger, same priority, prints when told      logger lowest, blocks on its queue
    the kernel decides who runs next             the design does: by deadline, shortest first

The workers become one task because they are the same code: a request is a job, jobs go
through a queue, the worker takes the next one when the previous is done. The queue is
the fairness (FIFO), no quantum is needed because a job is bounded, and one stack serves
three. Requests that must not wait behind a long job go on a second queue served by a
second worker one slot up, which is the priority the design actually meant.

    typedef struct { uint8_t source; uint16_t length; uint8_t *data; } job_t;
    static rtos_queue_t _jobs;
    static job_t        _jobStorage[8];

    static void worker(void *arg)             /* slot 2: serves every request in order */
    {
        job_t job;
        for(;;)
        {
            rtos_queueReceive(&_jobs, &job, RTOS_FOREVER);
            handle(&job);                     /* bounded work, then back to the queue */
        }
    }

    static void sampler(void *arg)            /* slot 3: 100 Hz, drift free, never late by a job */
    {
        for(;;)
        {
            rtos_waitPeriodic(RTOS_MS(10));
            job_t job = { .source = SENSOR, .length = read(&job.data) };
            rtos_queueSend(&_jobs, &job, 0);  /* or a second queue for the urgent ones */
        }
    }

    static void logger(void *arg)             /* slot 1: runs when nothing else wants to */
    {
        for(;;)
        {
            rtos_queueReceive(&_log, &line, RTOS_FOREVER);
            uart_puts(line);
        }
    }

    int main(void)
    {
        rtos_queueInit(&_jobs, (uint8_t *)_jobStorage, sizeof(job_t), 8);
        RTOS_TASK(1, logger,  NULL, RTOS_STACK_DEFAULT);
        RTOS_TASK(2, worker,  NULL, RTOS_STACK_DEFAULT);
        RTOS_TASK(3, sampler, NULL, RTOS_STACK_DEFAULT);
        rtos_start();
    }

The order of the slots is the design decision round robin would have hidden: the
sampler has the shortest period, so it ranks highest (rate monotonic); the worker's work
is bounded and may wait a sample; the logger has no deadline. Every task blocks, so they
interleave by blocking, which is what equal priorities would have given, with the
difference that the sampler is never 10 ms late because a worker's quantum ran first.

Two more cases. Several long computations that should share the CPU "fairly": put them
in one task and alternate chunks yourself (cooperative, with the switch points where the
data is consistent), or accept that one finishes first; the total time is the same and
the one that finishes first is useful earlier. Programs started on demand (a console
command, a plug-in): dynamic tasks (RTOS_CFG_DYNAMIC_TASKS, rtos_startTask) take and
return slots at run time, which is how nosh runs programs from the shell.

## Primitives

    rtos_delay, rtos_waitPeriodic      time; periodic tasks with overrun detection
    rtos_enterCritical/exitCritical    interrupts masked for a few instructions, nestable
    rtos_streamWrite/Read              bytes of any length, one writer (task or ISR), one
                                       reader woken at a trigger level (RTOS_CFG_STREAMS)
    rtos_exit, rtos_reapTask, rtos_startTask
                                       tasks that end and slots reused at run time
                                       (RTOS_CFG_DYNAMIC_TASKS), error codes, no kill
    events                             per task flags, wait any/all with timeout
    event groups                       flags shared by tasks, wait any/all, clear on exit
    notifications                      one 16 bit value per task: set, or, count
    semaphore                          counting, with an upper limit; binary as a case of it
    mutex                              immediate priority ceiling, deadlock detection;
                                       rtos_mutexTryLock and rtos_mutexLockTimeout for the
                                       exceptions (an owner that blocked while holding)
    queue, mailbox                     copies of fixed size items; the mailbox overwrites
    memory pool                        fixed blocks, O(1), blocking allocation
                                       rtos_queuePeek, rtos_queueSpace and rtos_queueReset,
                                       rtos_semReset: readers and restarts, nothing on the
                                       send and receive paths
    heap                               first fit with coalescing, optional
    software timers                    one shot and periodic, in the tick or in a task
    suspend / resume                   take a task out of scheduling and back

design.md in the nornDocs repository explains the model and what was left out on
purpose; `make api` there generates the API reference from kernel/rtos.h.

## Low power

What each port does when there is nothing to run, stated plainly because "tickless" is
easily read as more than it is here:

    mode                                   AVR (uno, mega2560)        ESP32
    idle between ticks                     SLEEP_MODE_IDLE, wakes     waiti: the CPU waits, clocks run;
                                           at every tick              wakes at every tick
    tickless (RTOS_CFG_TICKLESS)           no: the hook sleeps one    yes, but it skips ticks, not power:
                                           tick                       the CPU waits at full clock, RAM
                                                                      and peripherals stay on (tc16)
    light sleep (CPU and clocks stopped,   no                         no
      RAM kept, execution continues)
    deep sleep with RTC wake-up            no                         yes: hal/power.h, power_deepSleep(ms);
      (everything off but the clock;                                  waking is a reset with
      waking is a reset)                                              RTOS_RESET_DEEP_SLEEP, POWER_RTC_DATA
                                                                      survives in RTC slow memory (tc19)

So no port sleeps deeper than waiting for the next interrupt at full clock, except the
ESP32's deep sleep, which is the shape battery nodes take anyway: wake, measure, send,
sleep. The kernel side of tickless is generic and done (porting.md "Time"); what is
missing is in the ports: on the AVR a tick source that keeps running in power-save (Timer2
on a 32 kHz crystal, or an external RTC; the UNO and Mega boards have neither) and on the
ESP32 light sleep with the RTC timer, which also has to agree with nornWIFI's modem sleep.
Both are planned with their prerequisites in nornInternal (kernel_plan step 9); a Cortex-M
port is to put its tick on a timer that survives its stop mode (LPTIM or the RTC, not
SysTick) from the start.

## Boot and update: the OTA build (ESP32)

Without it the ROM loads the image from flash offset 0x1000 and the port maps the flash
code at 0x80000: one image, updated over the cable. `make OTA=1` (nornInternal/nornCore/
ota_plan.md) puts a selector there instead and the application in one of two slots:

    0x001000  the selector (boot/, 17 KB with the signature check): the image the ROM loads
    0x00E000  slot table, two copies (sequence number, CRC): the active slot, each slot's
              lengths, SHA-256 and state (empty, trial, confirmed, rejected), trials left
    0x010000  slot A: the RAM image, the flash image at +0x80000
    0x190000  slot B
    0x3FE000  nornStore, as before

The selector turns the ROM's boot watchdog off, reads the table, picks the confirmed slot
or a trial slot with boots left (counting the boot; a trial that never confirms is
rejected after OTA_TRIALS boots and the other slot takes over), hashes the slot's two
images with the chip's SHA block through the cache window, checks the ed25519 signature
of the 32 digest bytes against the public key compiled into it (Monocypher, boot/; a
slot whose signature fails is rejected on the spot), copies the RAM image into place as
the ROM would and jumps, leaving the slot's flash base in two RTC store registers for
rtos_port_flashInit. The key: tools/ota_keygen.py makes ota_key.pem (private, ignored by
git, the build runs it when the file is missing) and boot/ota_pubkey.h; tools/ota_table.py
and tools/ota_image.py sign with it. A fresh checkout gets a key of its own, so its
selector takes only its own images, which is the point; a product makes the key once and
keeps it off the build machines. The same binary runs in either slot: the RAM image is
loaded to fixed addresses and the flash code is reached through the MMU window, so only
the mapping moves. `make OTA=1 upload` writes the selector, the table (slot A confirmed,
tools/ota_table.py) and slot A; `make OTA=1 image` makes the pieces in build/<app>-esp32-ota.
An application does not know about any of it: tc01, tc09 (code from flash) and the
wifi example (the radio libraries in flash) run unchanged.

Measured on the board (2026-11-20), the selector's own cost from reset to the
application's first line: tc01 (10 KB) hash 8 ms, load 1 ms; tc09 (72 KB) hash 36 ms,
load 24 ms; wifi (58 KB + 337 KB of flash code) hash 182 ms, signature 120 ms, load
20 ms, the first application line at 370 ms against 58 ms without the selector (the
signature took 3730 ms with TweetNaCl before Monocypher). It runs at the crystal's
40 MHz, as the ROM leaves it (80 MHz after a software reset, where the PLL stays
selected; the selector reads its clock). Default builds are unchanged to the byte.

The application side is hal/ota.h: `ota_begin()` picks the other slot, `ota_write()`
streams the image file (tools/ota_image.py: a 48 byte header, the RAM image, the flash
image, SHA-256 over both), erasing each sector just before its first byte and hashing as
it goes, `ota_end()` checks the hash and marks the slot as the one to try; after the
reset the new image calls `ota_confirm()` once it has proven itself, or the selector
rejects it after three boots. Where the bytes come from is the application's:
examples/ota in nornRTOS does an HTTP GET straight into ota_write and tools/ota_test.sh
there runs the update and the roll back on the board (a 403 KB image in 7 to 11 s over
Wi-Fi, the trial boot's table write 139 ms; a forged image, one signature byte changed,
is written, rejected at its first boot and rolled back). Next in the plan: flash
encryption and secure boot through the ROM's eFuses, which turn "the selector checks the
signature" into "the ROM checks the selector"; not started, since the eFuses are
irreversible and wait for a board set aside for it. The selector already reads flash
through the cache window, as encryption requires.

## Robustness

What the kernel does when things go wrong, all optional per configuration:

    rtos_panic         the one way to stop: records reason, task, arguments and tick in
                       memory that survives a reset, calls rtos_panicHook (print, log),
                       then halts or resets (RTOS_CFG_PANIC_ACTION). After the reset
                       rtos_getLastPanic hands the record back and rtos_getResetCause
                       says whether the hardware saw a software, watchdog, brown-out,
                       external or power-on reset. Assertions go through it.
    stack canary       RTOS_CFG_STACK_CHECK 2: the bottom of every stack is checked at
                       each context switch and tick; overflow = RTOS_PANIC_STACK_OVERFLOW
                       with the task id, long before the corruption reaches a neighbour.
    stack guard        RTOS_CFG_STACK_GUARD (ESP32): a data breakpoint on the lowest 32
                       bytes of the running task's stack, armed at every switch, so the
                       first write there is RTOS_PANIC_STACK_OVERFLOW at that instruction
                       (arg2: its PC), also inside a critical section, before anything
                       below the stack is touched; the canary is the second line for a
                       frame that steps over the window. Two special register writes per
                       switch: tc10 on the ESP32 408 -> 416 cycles per switch, isr->task
                       418 -> 430, tc12 entry unchanged, tc01 +200 bytes; off, nothing.
                       The AVR has no such
                       hardware. tc18 provokes it. No memory protection beyond this:
                       design.md "What is deliberately absent".
    stack monitor      RTOS_CFG_STACK_WARN: the idle task watches the high water marks and
                       calls rtos_stackWarnHook once per task whose untouched margin
                       falls below the port's warning level, before anything overflows.
    watchdog           RTOS_CFG_WATCHDOG: hardware watchdog fed by the idle task, plus
                       rtos_wdtMonitor/rtos_wdtCheckIn deadlines per task. A late task is
                       reported by name (RTOS_PANIC_WATCHDOG) while the system still runs;
                       a starved idle task (runaway task, interrupt storm) ends in a
                       hardware reset that the next boot sees as RTOS_RESET_WATCHDOG.
    deadlock           a mutex lock that would wait follows the chain of owners; if it
                       leads back to the caller: RTOS_PANIC_DEADLOCK with both tasks.
    task inspection    rtos_getTaskInfo: state, what the task waits on and with which
                       timeout, priorities, stack, run time, overruns. The nornSHELL
                       module (nosh) puts these on a serial console as ps, mem, sched
                       and uptime, using nothing but this public API.
    object statistics  queues count drops and their high water mark, mailboxes their
                       overwrites, pools their low water mark and failed allocations,
                       semaphores the gives lost at the limit (RTOS_SEM_INIT_BINARY).
    timer task         RTOS_CFG_TIMER_TASK_PRIO: software timer callbacks in a kernel
                       task instead of the tick interrupt, so they may use the whole API.
    trace              RTOS_CFG_TRACE: rtos_traceHook at switches, blocks, wake-ups, timer
                       callbacks and panics; hal/trace.c keeps a ring buffer with time
                       stamps that rtos_panicHook can dump.
    log classes        hal/log.h: LOG_P/E/W/I/D(statements) and LOGS_x("text") write a
                       line `I (12345) text` on the console UART when the class is within
                       `log_level` (INFO at start; set by the application, by nornSHELL's
                       `log`, or from a stored value as examples/device does with
                       nornStore). A module can have a level of its own (LOG_MODULE,
                       nornWIFI's radio log is `wifi`) that `log wifi warning` sets while
                       log_setLevel() sets them all; LOG_CFG_LOCK 1 serializes whole
                       lines with a binary semaphore, off by default, and LOG_AT_ISR logs
                       from interrupt context without it. A class that is off costs one
                       byte compare, no UART time; LOG_CFG_MAX (default INFO) keeps DEBUG out of the image
                       altogether until an application asks for it. PANIC is always
                       written. Not part of the kernel, which never logs: an application
                       that does not use the macros links none of it, and nornWIFI's
                       radio log follows the same level.
    mandatory checks   RTOS_CHECK (rtos_assert.h) is compiled in every configuration,
                       unlike RTOS_ASSERT: a task id used as an array index, a stack
                       smaller than the port's frames, a NULL storage or callback, a
                       block freed that is not the pool's or the heap's, a heap used
                       before its init, the memory layout the ESP32 port relies on. A
                       failure is RTOS_PANIC_ARGUMENT with the values. Assertions
                       remain for contracts and internal invariants (a mutex unlocked
                       by a non owner, a wait list invariant) and may be switched off.
    input checks       rtos_malloc refuses a size that would wrap (NULL, counted as
                       failed); a software timer started with delay 0 fires at the next
                       tick, never again in the tick it was started from; the ESP32
                       port validates the APP CPU's image (every segment inside the
                       image, word aligned, inside that CPU's memory) before copying.

AVR note: a watchdog reset is only survivable with a bootloader that disables the watchdog
at start. The original Mega 2560 bootloader does not, and the board then resets in a loop
until power is cycled. rtos_port_reset therefore jumps to the reset vector by default
(RTOS_CFG_AVR_RESET_VIA_WDT 0), which does not pass the bootloader, and tc05 skips its
hardware watchdog stage on such a port. Should a board get stuck anyway: unplug it, and
flash within the first second after plugging it in again. The cure is a bootloader that
clears the watchdog at start, Optiboot (or the current Arduino Mega bootloader): burn it
once with an ISP programmer or a second Arduino as ISP (the Arduino IDE's "Burn
Bootloader" with the Mega 2560 board and Optiboot selected does exactly that), then set
RTOS_CFG_AVR_RESET_VIA_WDT 1 and RTOS_CFG_WATCHDOG 1 and the Mega resets through the
watchdog like the ESP32 does. The UNO ships with Optiboot and needs nothing.

## Testing

Three layers, all run by `make test-all` (about a minute with an ESP32 connected):

    test/test_*.c      deterministic unit tests on the host port: every primitive is driven
                       through a scripted sequence of calls, simulated ticks and simulated
                       interrupts, and the trace of (task, event, value) is compared with
                       the expected one. Built with -Wconversion, ASan and UBSan.
    test/test_timeout.c timeouts at the limit with 16 bit ticks: delays of RTOS_MAX_TIMEOUT
                       + 1000 and RTOS_FOREVER - 1, a wait of exactly the limit beside short
                       ones, timers and a periodic wait of the limit
    test/test_prio.c   the scheduler against mutex ceilings: an owner that blocked while
                       holding is switched to by the interrupt tail when it wakes, a
                       waiter handed the mutex runs before a task between its base
                       priority and the ceiling, nested ceilings restore in LIFO order
    test/test_tie.c    the scheduling order at equal effective priorities (the key
                       effective priority, boosted, base priority): the owner at the
                       ceiling beats the task whose base priority is the ceiling in the
                       task-context pick and in the interrupt tail, two owners at one
                       ceiling run in base order, a suspended owner holding a mutex
    test/test_lifecycle.c a task whose function returns is RTOS_TASK_TERMINATED and deaf
                       to every wake-up, a terminated slot reset with
                       rtos_kernel_tcbReset starts a new task clean, and returning
                       while holding a ceiling mutex is RTOS_PANIC_ARGUMENT
    test/test_dyntask.c the dynamic task API: rtos_exit from a nested call, reap and
                       restart from a lower task (preempts), from a higher task (waits)
                       and from the idle hook, every error code including the interrupt
                       context, an exit with a mutex whose ceiling equals the priority,
                       the idle task and an interrupt exiting (all RTOS_PANIC_ARGUMENT),
                       the watchdog monitor dropped by rtos_exit and kept by a return,
                       the held count across a mutex hand-over
    test/test_stream.c stream buffers: wrap-around for every size 1..64 and trigger against
                       a reference sequence, the trigger level (woken at the eighth byte,
                       not before), timeouts on both sides, the writer blocked and woken
                       as room is made, a reset that wakes a writer and leaves a reader
                       waiting, an interrupt writer against a task reader with short
                       writes on the full stream, ReadFromIsr, and a second reader or
                       writer refused (RTOS_PANIC_ARGUMENT)
    test/test_robust.c robustness features on the host port, one child process per
                       scenario: deadlock, stack canary, task watchdog, timer task,
                       statistics and binary semaphore, task info, trace, assertion
    test/test_api.c    contract test: every public function called as the header documents
                       it, return values and object statistics checked directly: polling
                       with RTOS_NO_WAIT, timeouts, the *FromIsr variants, run time
                       initializers, overrun counting, task info
    test/test_wrap.c   tick counter wrap-around: with 16 bit ticks the counter starts 50
                       ticks before a wrap and runs through four of them while periodic
                       waits, delays, timeouts, a periodic and a one shot timer and the
                       task watchdog are in use, every interval checked to the tick;
                       test_wrap32 is the same source across the 2^32 wrap
    test/test_stress.c randomized stress test on the host port: six tasks pick random
                       operations on every primitive (semaphore, mutex, queue, mailbox,
                       pool, event group, heap, events, notifications, periodic waits,
                       suspend/resume, software timers) with random timeouts, the idle
                       hook injects random interrupts, and the kernel invariants are
                       checked after every operation and interrupt (ready mask, timed
                       list, waiter masks, ceiling priorities, suspended tasks, pool,
                       queue and heap accounting, run time sum). Queue items and pool and
                       heap blocks carry stamps that must survive. The sequence is fixed
                       by SEED, so a failure reproduces; 8 seeds x 200000 ticks pass.
    applications/tc*   on the target: each test case checks its own counters at the end
                       and prints RESULT tcNN PASS or FAIL (hal/testresult.h), then halts,
                       which ends a simavr run. tools/test_all.sh collects the lines,
                       after first checking that a failing host test fails `make
                       host-test` (the runner once piped into tail and hid exit codes).

`make host-cov` builds the host tests with gcov and prints the line coverage of every
kernel file and the uncovered line ranges (tools/host_cov.sh); the kernel is at 98.8 %,
the rest being the reset paths of rtos_panic.c, which the host cannot take, and the
stack realignment that only the ESP32 needs. `make analyze` runs gcc's static analyzer
(-fanalyzer) over the kernel, the ports and the HAL with each architecture's compiler
(tools/analyze.sh); it reports nothing on any of them.

Every change that may touch the hot path, the stacks or the code size is measured before
and after with `tools/measure.sh <label>`; the summaries live in nornInternal/nornCore/measurements/
(baseline.md is the state before the 2026-11-05 audit fixes) and the raw logs in
build/measure/<label>/.

tc01..tc06 pass on a real Mega 2560 with the same counters as simavr, on the ESP32 board
and in QEMU. Not covered yet: a real UNO, and static analysis beyond the compiler warnings
(cppcheck is not installed here). .github/workflows-disabled/ci.yml (parked, not run) has the host tests, simavr
and QEMU. porting.md in nornDocs describes the port interface.

## Timing (tc04, tc10)

tc10 measures the kernel in CPU cycles with a cycle counter and a probe interrupt whose
period is dithered so that it lands at every phase of the tick and of the other activity
(hal/<arch>/board.h: timer 1 and timer 4 on the Mega, CCOUNT and the application timer
on the ESP32). All figures are taken with everything in the test running at once: the
switch benchmark, the probe at ~770 Hz, the tick, and the default configuration (stack
fill, run time statistics, assertions, software timers). simavr is cycle accurate and
gives the same numbers as the board (the isr -> task minimum is one lucky probe and may
differ by a few cycles between the two). The figures below are from 2026-11-09
(nornInternal/nornCore/measurements/step9-curboost-after.md), after the audit fixes and the scheduler
contract for mutex ceilings; "ceiling held" repeats a row while the running task holds a
mutex with a ceiling above its priority.

    mega2560, 16 MHz                      cycles        time
    switch (sem give/take, per switch)    439 min       27.4 us   479 avg
    switch (notify)                       432 min       27.0 us
    switch (queue, 1 byte item)           544 min       34.0 us
    isr entry (event to RTOS_ISR body)    50 min        3.1 us    max seen 34 us
    isr -> task (give in ISR to task)     471 min       29.4 us   max seen 52 us
    isr -> task, ceiling held             493 min       30.8 us
    hw event -> task, best case                         33.5 us
    api, no switch: sem give+take 145 (ceiling held 153), notify 220,
                    mutex lock+unlock 218, queue send+recv 453, mbox 282,
                    pool alloc+free 487, evgroup set+wait 272, timer start+stop 241,
                    getTick 21
    ram: tcb 42 B/task; sem 4, mutex 4, queue 12, mbox 7, pool 12, timer 18, evgroup 3;
         min stack 96 B; the benchmark tasks use 39-55 B of stack plus what they print with

Stack per task (tc11, bytes left on the stack by the kernel and the port; the task's own
frames come on top). An interrupt lands on the stack of the running task, so every task
needs the "interrupted" figure plus its own use; the ESP32 figures are from QEMU, which
is exact for stack use:

    board      only blocks   woken from ISR   interrupted while running   RTOS_PORT_MIN_STACK
    uno        30            32               40                          96
    mega2560   34            38               44                          96
    esp32      192           192              336                         512   (board)
    esp32      300           300              336                         512   (QEMU)

The "only blocks" figure is 192 on the board when no interrupt happens to land on the
task during its short runs and 300 when one does (the 112 byte exception frame plus the
handler's save area pointer, below the task's two frames): which of the two tc11 prints
depends on the timing of the 1 kHz test interrupt against the task, so a change of a few
cycles anywhere can flip it, and QEMU always shows it. The figure to size a stack by is
the interrupted one (336 on the ESP32), which does not depend on this.

### Stacks

What a task needs is its own frames plus what the kernel and the port leave under them
when it is interrupted (tc11, tc12). The port gives three sizes, and `rtos.h` names them
RTOS_STACK_MIN, RTOS_STACK_DEFAULT and RTOS_STACK_LARGE:

    level                 esp32     mega2560/uno   meant for
    technical minimum     512 B     96 B           very simple tasks whose stack use has
                                                   been measured and is bounded: on the
                                                   ESP32 it leaves 96 bytes, two call8
                                                   frames, beyond the preemption case
    default               1024 B    192 B          an ordinary task; start here
    complex tasks         2048 B+   384 B+         recommended starting size for tasks
                                                   that print, nest calls, use floating
                                                   point or big locals; no upper bound:
                                                   tc12 shows 32 nested frames alone
                                                   take 1.4 KB on the ESP32, and crypto
                                                   or network tasks need more
    stack monitor         high water mark (rtos_getStackFree, rtos_getTaskInfo) plus a
                          warning: with RTOS_CFG_STACK_WARN the idle task reads one task's
                          mark per loop and calls rtos_stackWarnHook once for a task whose
                          untouched margin drops below RTOS_PORT_STACK_WARN (96 B on the
                          ESP32, 24 B on the AVR); RTOS_CFG_STACK_CHECK 2 adds the canary
                          that turns an actual overflow into a panic

The minimum is what the kernel accepts, not a promise: nornOS does not guarantee that
every application works with it, and none of the levels is a limit in the other
direction. Size a task from the default, run it, read the high water mark, and keep a
margin of at least the warning level.

tc12 repeats this at the bottom of a call chain 0, 4, 8, 16 and 32 frames deep: the
kernel and port share stays the same at every depth (ESP32: 416 bytes preempted with a
switch out of the interrupt, 240 blocking, 416 with floating point, each including the
two bottom frames of the task itself; Mega: 56, 46, 65), so the rule for a stack is the
task's own frames (48 bytes per call8 level with a few locals; the register file absorbs
the first four) plus the "interrupted while running" figure. Interrupts do not nest on
either port, a second source waits and runs on the same spot.

On the ESP32 a call8 frame is 32 bytes and the interrupt frame 176, and the switch spills
every live window, so the blocking path (task, semTake, blockCurrent, switchContext) costs
what a whole AVR stack does. The port inlines the scheduler into the blocking path, the
interrupt dispatcher and the ISR tail into the exception handler (one frame each); the
trampoline frame under every task cannot go without breaking the window spill chain.

    esp32, 240 MHz                        cycles        time
    switch (sem give/take, per switch)    408 min       1.70 us   409 avg
    switch (notify)                       418 min       1.74 us
    switch (queue, 1 byte item)           461 min       1.92 us
    isr entry (event to RTOS_ISR body)    35 avg        0.9 us    max seen 2.7 us
    isr -> task (give in ISR to task)     418 min       1.74 us   max seen 3.3 us
    isr -> task, ceiling held             449 min       1.87 us
    hw event -> task, best case                         1.85 us
    api, no switch: sem give+take 112 (ceiling held 115), notify 145,
                    mutex lock+unlock 156, queue send+recv 285, mbox 194,
                    pool alloc+free 176, evgroup set+wait 137, timer start+stop 124,
                    getTick 21
    ram: tcb 72 B/task; sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4;
         min stack 512 B; the benchmark tasks use 300-464 B of stack

The ESP32 figures are from the board (2026-11-09; the switch was 456 cycles before the
hot path work of 2026-11-04 and 405 after it, the audit fixes since then cost a few
cycles each, see nornInternal/nornCore/measurements/); the entry minimum reads 0 because the probe timer's
40 MHz counter is latched at the alarm. With the run time statistics off the switch is
361 cycles, with assertions off as well 351 (1.46 us). The interrupt entry costs the
same 850 ns whether the interrupted task is 0 or 32 call frames deep (tc12): the spill
at entry is not depth dependent beyond the register file: the entry spills the live
windows, at most about seven (126 cycles), which only the first interrupt after a deep
descent pays in full. The worst entry seen is about 1.8 us, that spill behind a tick
handler. nornInternal/nornCore/esp32_hotpath.md has the static review and the measurements.

Where the cycles go on the AVR, per switch: ~105 in port_context.S (19 registers each
way), ~45 in the run time statistics (RTOS_CFG_RUNTIME_STATS 0 saves them), ~20 in
assertions (RTOS_CFG_ASSERT 0), the rest in the scheduler, the wait list and the
primitive. With statistics and assertions off: 322 cycles (20 us). The max of the ISR
entry is the longest interrupt-off window: a switch (~27 us) that a tick with a wake-up
lands behind. tc04 is the older, coarser test (4 us counter) and still runs everywhere:

    board      switch avg   isrLatency max   jitter max   notes
    uno        24 us        24 us            16 us        simavr
    mega2560   24 us        24 us            16 us        simavr and real board
    esp32      10.9 us      12 us            5 us         40 MHz crystal, before the hot path work
    esp32      2.9 us       4 us             2 us         160 MHz, before the hot path work
    esp32      1.7 us       2 us             0 us         240 MHz (default), board 2026-11-04

QEMU is not cycle accurate and stalls when the host is busy: its tc04/tc10 figures mean
nothing, and a heavy parallel build on the host can even fail their latency limits.

## Applications (applications/)

    hello  bring-up without the scheduler: banner, one line per second, LED, UART echo
    blink  the smallest running nornOS: one task toggles the LED every 500 ms (blue LED
           on the ESP32 devkit, pin 13 on the AVR boards), everything else off. Image:
           esp32 6089 text / 1500 data+bss, mega2560 2656 / 319, uno 2514 / 319 bytes
    tc01   periodic task, events, semaphore from an ISR, ceiling mutex, stack check
    tc04   timing: context switch, interrupt latency, periodic jitter
    tc05   panic, reset and recovery: application panic, stack overflow, late task,
           starved idle; the stage survives each reset and the record is checked
    tc06   floating point kept in registers across preemption, lazy FPU on the ESP32
    tc07   ESP32 only: a second nornOS instance on the APP CPU answering requests over
           the shared memory link (ONLY_BOARDS in app.mk keeps it off the AVR boards)
    tc08   heap, event groups, suspend/resume and tickless idle together
    tc09   ESP32 only: code and constants running from flash through the cache
    tc10   kernel benchmark in cycles: API cost, switch, ISR entry and ISR-to-task latency,
           RAM per object (Mega and ESP32; the UNO lacks the free timers)
    tc11   stack need per task: what a task that only blocks, one that is woken from an
           interrupt and one that is interrupted while running leave on their stacks
    tc12   the same on top of a call chain 0..32 frames deep, in four phases: the chain
           alone, preempted at the bottom, blocking at the bottom, floating point at the
           bottom (Mega and ESP32)
    tc13   dynamic tasks under the watchdog and the stack monitor: a supervisor starts,
           waits for and reaps a worker over and over (rtos_exit on even cycles, a return
           on odd ones) while a contender fights for the worker's mutex; about 640 cycles
           in 3 s on every board, TC13_STOP_AFTER_MS=3600000ul is the soak (ESP32,
           2026-11-15: 771429 cycles in an hour, no error)
    tc17   ESP32: the SMP kernel on both cores (RTOS_CFG_SMP, affinity): a task woken by
           the interrupt on core 0 runs on core 1, tasks move between the cores, a mutex is
           handed over between them, tasks are suspended from the other core; board and QEMU
    tc18   ESP32: the hardware stack guard (RTOS_CFG_STACK_GUARD): a store into the bottom
           32 bytes of the running task's stack is RTOS_PANIC_STACK_OVERFLOW at that
           instruction (the PC in arg2), another task's bottom is not, a store just above
           the window is not; board and QEMU
    tc19   ESP32: deep sleep (hal/power.h): three sleeps of 2 s woken by the RTC timer, the
           reset cause RTOS_RESET_DEEP_SLEEP, a record in RTC slow memory (POWER_RTC_DATA)
           intact, the sleep length by the RTC clock within 5 %; board only, QEMU has no
           sleep controller
    tc16   ESP32: tickless idle at 100 Hz with a wake-up source that is not the tick: the
           missed ticks are counted before the interrupt's handler runs, a delay started
           right after such a wake-up lasts its full length, tick count, getTimeUs and the
           cycle counter agree after 10 s (audit findings R3 and R4)
    tc14   stream buffers: the application timer interrupt writes 8 byte bursts at 500 Hz
           into a stream whose reader wakes at 32 bytes, and a producer task keeps a 64
           byte stream full against a reader taking 7 bytes every 2 ms; sequence checked
           on both, nothing dropped (every board)
           (tc01..tc04 use the application timer of hal/<arch>/board.h and run on every board)
    tc02   queue fed from an ISR with deliberate overflow, mailbox between tasks
    tc03   task notifications from ISR and timers, one shot and periodic software timers
    bTool  the brewing controller: DHT11 sensor, HD44780 LCD, thermostat relay, console

All test cases stop after 3 s so that `make APP=tcNN sim` terminates; set TCNN_STOP_AFTER_MS
to 0 for hardware. bTool runs forever.

## Context switch cost

Voluntary switch (a task blocks): 18 call-saved registers + SREG, about 105 cycles on the
AVR. Preemption by an ISR: the RTOS_ISR wrapper saves the 15 call-clobbered registers, SREG
and RAMPZ, and only if a switch is needed the call-saved half as well. Interrupts stay off
from the kernel call that decides to switch until the task that is switched to leaves
its own kernel call, so the switch cost is also the worst interrupt-off window; tc10
prints both.
