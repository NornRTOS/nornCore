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
    port/host/     Linux port on ucontext, used by the unit tests only
    hal/           drivers with one API for all architectures: uart.h, gpio.h, delay.h,
                   hd44780 (LCD), dht11; hal/<arch>/ holds hal_arch.h (flash strings,
                   delays), gpio_arch.h, uart.c and board.h (pin map per board)
    config/        rtos_config.template.h, copy to the application as rtos_config.h
    test/          host unit tests of the kernel, one program per test_*.c
    applications/<app>/  one folder per application: rtos_config.h and app.mk (BOARD)

Adding an architecture means a new port/<arch>/ and hal/<arch>/ plus one line in the
Makefile mapping its boards to the architecture. The kernel and the generic HAL are not
touched; doc/porting.md describes the port interface.

## Build

    make APP=tc01             ELF/HEX in build/tc01-<board>, prints memory use
    make APP=tc01 BOARD=uno   build for another board than app.mk says
    make APP=tc01 sim         run in simavr, UART0 output on the terminal
    make APP=tc01 upload PORT=/dev/ttyACM0
    make host-test            kernel unit tests on the PC, with ASan and UBSan; a test
                              passes only if it exits 0 and ends with a PASS line
    tools/measure.sh LABEL    size and timing snapshot (tc01 size per board, tc10/tc11/
                              tc12 on the Mega, the ESP32 and QEMU) -> doc/measurements/
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
dispatched to RTOS_ISR bodies. A context switch spills every live register window first
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

## Three configurations to start from

config/rtos_config.template.h documents every option. These three sets are starting
points to copy into an application's rtos_config.h, measured with tc10 on the boards
(cycles per context switch: ESP32 at 240 MHz / Mega at 16 MHz):

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

Common to all: TICK_HZ 1000, IDLE_SLEEP 1, TIMER_TASK_PRIO 0, PANIC_ACTION 0 (halt).
nornWIFI needs the heap, event groups and 32 event bits (default or large on the ESP32);
the original Mega bootloader does not survive a watchdog reset, hence no watchdog on
the AVR. A suggested priority map puts the module tasks at the top: with 15 tasks the
nornWIFI radio at 15, its service task at 14, the nornNET net task at 13, the
application from 1 to 12.

The hooks an application may define are functions with weak defaults: rtos_idleHook
(RTOS_CFG_IDLE_HOOK as a macro still works), rtos_stackWarnHook, rtos_panicHook,
rtos_assertFailed, rtos_traceHook.

rtos_init passes RTOS_CONFIG_SIGNATURE, every layout changing option folded into one
number, to the kernel; an application compiled against another rtos_config.h than the
kernel objects it is linked with (a stale build directory, a library) panics at once
with RTOS_PANIC_CONFIG instead of corrupting memory.

## Model

* One task per priority. Task id = priority, 1..RTOS_CFG_NUM_TASKS, higher runs first.
  Task 0 is the implicit idle task (the context that called rtos_start).
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
* Run time statistics: at every context switch the tick timer is read and the elapsed
  time charged to the task that ran. rtos_getTaskRunTime gives the per task total,
  rtos_getCpuLoad the non-idle share since the previous call. Costs about 60 cycles per
  switch; disable with RTOS_CFG_RUNTIME_STATS 0.
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
  scheduled by it when it wakes; so is a waiter the mutex is handed over to.
* Interrupts that talk to the kernel are written with RTOS_ISR(vector) { ... } and use
  the *FromIsr functions; a task switch, if needed, happens once at the end of the ISR.
* Critical sections are short cli/sei pairs with the previous state saved (nestable).

## Primitives

    rtos_delay, rtos_waitPeriodic      time; periodic tasks with overrun detection
    events                             per task flags, wait any/all with timeout
    event groups                       flags shared by tasks, wait any/all, clear on exit
    notifications                      one 16 bit value per task: set, or, count
    semaphore                          counting, with an upper limit; binary as a case of it
    mutex                              immediate priority ceiling, deadlock detection
    queue, mailbox                     copies of fixed size items; the mailbox overwrites
    memory pool                        fixed blocks, O(1), blocking allocation
    heap                               first fit with coalescing, optional
    software timers                    one shot and periodic, in the tick or in a task
    suspend / resume                   take a task out of scheduling and back

doc/design.md explains the model and what was left out on purpose.

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
                       timeout, priorities, stack, run time, overruns.
    object statistics  queues count drops and their high water mark, mailboxes their
                       overwrites, pools their low water mark and failed allocations,
                       semaphores the gives lost at the limit (RTOS_SEM_INIT_BINARY).
    timer task         RTOS_CFG_TIMER_TASK_PRIO: software timer callbacks in a kernel
                       task instead of the tick interrupt, so they may use the whole API.
    trace              RTOS_CFG_TRACE: rtos_traceHook at switches, blocks, wake-ups, timer
                       callbacks and panics; hal/trace.c keeps a ring buffer with time
                       stamps that rtos_panicHook can dump.
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
flash within the first second after plugging it in again.

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
and after with `tools/measure.sh <label>`; the summaries live in doc/measurements/
(baseline.md is the state before the 2026-11-05 audit fixes) and the raw logs in
build/measure/<label>/.

tc01..tc06 pass on a real Mega 2560 with the same counters as simavr, on the ESP32 board
and in QEMU. Not covered yet: a real UNO, and static analysis beyond the compiler warnings
(cppcheck is not installed here). .github/workflows/ci.yml runs the host tests, simavr
and QEMU on every push. doc/porting.md describes the port interface.

## Timing (tc04, tc10)

tc10 measures the kernel in CPU cycles with a cycle counter and a probe interrupt whose
period is dithered so that it lands at every phase of the tick and of the other activity
(hal/<arch>/board.h: timer 1 and timer 4 on the Mega, CCOUNT and the application timer
on the ESP32). All figures are taken with everything in the test running at once: the
switch benchmark, the probe at ~770 Hz, the tick, and the default configuration (stack
fill, run time statistics, assertions, software timers). simavr is cycle accurate and
gives the same numbers as the board.

    mega2560, 16 MHz                      cycles        time
    switch (sem give/take, per switch)    423 min       26.4 us   470 avg
    switch (notify)                       411 min       25.7 us
    switch (queue, 1 byte item)           522 min       32.6 us
    isr entry (event to RTOS_ISR body)    50 min        3.1 us    max seen 31 us
    isr -> task (give in ISR to task)     443 min       27.7 us   max seen 44 us
    hw event -> task, best case                         31 us
    api, no switch: sem give+take 133, notify 200, mutex lock+unlock 186,
                    queue send+recv 429, mbox 269, pool alloc+free 461,
                    evgroup set+wait 260, timer start+stop 222, getTick 21
    ram: tcb 42 B/task; sem 4, mutex 4, queue 12, mbox 7, pool 12, timer 18, evgroup 3;
         min stack 96 B; the benchmark tasks use 36-55 B of stack plus what they print with

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
    switch (sem give/take, per switch)    405 min       1.69 us   406 avg
    switch (notify)                       415 min       1.73 us
    switch (queue, 1 byte item)           457 min       1.90 us
    isr entry (event to RTOS_ISR body)    35 avg        0.9 us    max seen 1.5 us
    isr -> task (give in ISR to task)     417 min       1.74 us   max seen 3.2 us
    hw event -> task, best case                         1.85 us
    api, no switch: sem give+take 109, notify 141, mutex lock+unlock 138,
                    queue send+recv 279, mbox 191, pool alloc+free 153,
                    evgroup set+wait 134, timer start+stop 124, getTick 21
    ram: tcb 72 B/task; sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4;
         min stack 512 B; the benchmark tasks use 300-448 B of stack

The ESP32 figures are from the board after the hot path work (2026-11-04 evening, the
switch was 456 cycles before it); the entry minimum reads 0 because the probe timer's
40 MHz counter is latched at the alarm. With the run time statistics off the switch is
361 cycles, with assertions off as well 351 (1.46 us). The interrupt entry costs the
same 850 ns whether the interrupted task is 0 or 32 call frames deep (tc12): the spill
at entry is not depth dependent beyond the register file: the entry spills the live
windows, at most about seven (126 cycles), which only the first interrupt after a deep
descent pays in full. The worst entry seen is about 1.8 us, that spill behind a tick
handler. doc/esp32_hotpath.md has the static review and the measurements.

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
