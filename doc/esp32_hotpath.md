# ESP32 hot path: static review and measurement plan

Written 2026-11-04 from the disassembly of the tc10 build (xtensa-esp32-elf-gcc 14,
-Os), before the board was free to measure. The Mega figures in the README came from the
same method; the ESP32 numbers below are instruction counts, not cycles, and the
hypotheses are what tc10 on the board has to confirm or refute. Last board figures
(README, before the hot path work): 1.9 us per switch at 240 MHz, i.e. about 456 cycles.

## The paths

Voluntary switch, semaphore give on the low task to the high task and back:

    function                           instr  calls  sync  special regs  ld/st  rotw
    rtos_semGive                          10      2     1       2           0
    rtos_semGiveFromIsr                   20      1                         7
    rtos_kernel_releaseHighestWaiter      29      2                         1
    rtos_kernel_makeReady                 32      1                        12
    rtos_kernel_schedule                  47      2                        15
    rtos_port_switchContext               29      0     1       2           9     5
    rtos_semTake                          31      1     1       2           9
    rtos_kernel_blockCurrent              78      4                        25     (schedule inlined)

Interrupt to task, probe timer to the woken task:

    _exceptionEntry (asm)                 90      1     3       3          49     5
    rtos_port_exception                  145      4     2                  24     (dispatch and tail inlined)
    appTimerIsr (HAL wrapper)             15      1                         3
    board_appTimer_rtosBody (tc10)        48      5                         5
    rtos_notifyFromIsr                    57      2                         9
    rtos_kernel_isrPrepareSwitch          28      1                        10
    rtos_port_switchContext               29                                9     5
    _WindowOverflow8 / Underflow8         10 each, 9 ld/st each; Overflow12 14 / 13

## Where the cycles must be

1. **Window spills.** `rtos_port_switchContext` and `_exceptionEntry` both run
   SPILL_ALL_WINDOWS (five `rotw` plus an `and a12` each): every live window takes a
   window overflow exception, about 15-25 cycles plus 9-13 stores each. With the
   kernel's own nesting of four to six call8 frames at the switch, that is 100-150 cycles
   per switch and again per interrupt entry, probably the largest single item. The entry
   spill is what ESP-IDF's port does too (xthal_window_spill_nw in _xt_context_save); a
   lazy variant that spills only when a switch is needed would save it on every interrupt
   that does not switch, at the price of the handler running on live windows of the
   interrupted task (the pseudo base save area trick in port_context.S would have to
   cover the whole chain). Measure first: ISR entry latency against the depth of the
   interrupted task (tc12's phases B and D, read tc10's entry figure at depth 0, 8, 16).
2. **Interrupt frame.** 49 stores and 3 `rsync` at entry, about as many loads and 2
   `rsync` at exit: 120-150 cycles fixed. The 16 address registers are all saved because
   the handler reuses the interrupted window; LBEG/LEND/LCOUNT/SAR because gcc uses the
   loop instructions. Nothing cheap to remove.
3. **Run time accounting.** Was a 32x32-bit to 64-bit multiplication per switch (tick
   count times cycles per tick) plus 64-bit adds; now (2026-11-04) an inline addition
   from a per tick base, like the AVR. Expect 20-30 cycles less per switch; the 64-bit
   `runTime += now - last` stays (six instructions on this CPU).
4. **Special register traffic.** `rsil`/`wsr.ps`/`rsync` pairs in every critical section
   and the PS/CPENABLE writes in the switch: a few cycles each, perhaps 20 per switch in
   all. Not worth touching.
5. **Call levels.** Each call8 is an `entry` plus a possible window overflow on the way
   down and underflow on the way back. The blocking path lost one level (schedule
   inlined), the interrupt path two (dispatch and tail inlined); on the AVR this kind of
   change was worth nothing, here each level saved avoids an exception pair when the
   register file is full. Expect a small, measurable gain.

## Plan on the board

    make APP=tc10 BOARD=esp32 upload && make APP=tc10 BOARD=esp32 monitor
    make APP=tc10 BOARD=esp32 EXTRA_CFLAGS=-DRTOS_CFG_RUNTIME_STATS=0 ...   (item 3)
    make APP=tc10 BOARD=esp32 EXTRA_CFLAGS=-DRTOS_CFG_ASSERT=0 ...
    make APP=tc12 BOARD=esp32 ...   with the probe's entry figure printed per depth (item 1)

Then the same four lines for tc11 and tc12 (stack figures, QEMU numbers to confirm) and
the full regression with the board: `AVR_HW=mega2560:/dev/ttyACM0 tools/test_all.sh`.
Results go into the README timing section next to the Mega.

## Measured on the board, 2026-11-04 evening

tc10 at 240 MHz, default configuration, after items 3 and 5 above were done:

    switch, sem give/take         405 cycles  1.69 us   (456 cycles, 1.9 us before)
    switch, notify                415         1.73 us
    switch, queue                 457         1.90 us
    isr entry, avg / max          35 / 61 counts of 40 MHz = 0.9 / 1.5 us
    isr -> task, min / max        417 / 764 cycles = 1.74 / 3.2 us
    hw event -> task, best        1.85 us

Stack (tc11, tc12): a task that only blocks leaves 192 bytes (QEMU said 300: it spills
windows earlier), one interrupted while running 336 as in QEMU; the kernel and port
share at depth 0..32 is 416 / 240 / 416 bytes for preempted / blocking / preempted with
float, identical to QEMU.

Still to run: the RUNTIME_STATS=0 and ASSERT=0 variants (item 3's share), and the entry
latency against the interrupted task's depth (item 1's share). Lesson from the run: QEMU
timing tests fail spuriously when a heavy build runs on the host at the same time.
