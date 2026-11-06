# Measurement step5-timeout

After audit fix F6: RTOS_MAX_TIMEOUT (half the tick range), RTOS_CHECK in the timed
branch of rtos_kernel_blockCurrent, rtos_timerStart, rtos_waitPeriodic (1..MAX, was an
assert) and rtos_wdtMonitor; rtos_delay splits delays of MAX or more. Against
step4-small: switch, ISR to task and stacks unchanged on both boards; timer start+stop
230->241 cycles on the Mega, 122->124 on the ESP32; image text -2 B AVR, +12 B ESP32
(kernel+port objects +125 / +57 B, mostly the panic calls of the new checks).

Taken 2026-11-06 12:36, nornCore 85fbf52 + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6668 | 170 | 833 | 9675 | 8 | 240 |
| mega2560 | 6814 | 170 | 833 | 9701 | 8 | 240 |
| esp32 | 9974 | 24 | 3920 | 12710 | 13 | 815 |

## tc10 mega2560

```
tc10 Mega F_CPU=16000000 (cycles; 1 cycle = 62500 ps)
  api (one call pair, no switch, min of 64):
    critical section: 3
    getTick: 21
    sem give+take: 146
    notify set+wait: 221
    mutex lock+unlock: 219
    queue send+recv: 455
    mbox post+fetch: 283
    pool alloc+free: 488
    evgroup set+wait: 273
    timer start+stop: 241
    events post+wait: 218
    getStackFree: 1694
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 440/476 cycles = 27500/29750 ns
    notify: 432/470 cycles = 27000/29375 ns
    queue: 545/596 cycles = 34062/37250 ns
    events: 445/487 cycles = 27812/30437 ns
  isr (min/avg/max):
    entry:            50/65/561 counts = 3125/4062/35062 ns (n=2352)
    isr->task sem:    463/479/810 cycles = 28937/29937/50625 ns (n=1000)
    isr->task notify: 478/493/825 cycles = 29875/30812/51562 ns (n=1352)
    hw->task best:    33000 ns
  ram (bytes):
    tcb 42 x 4, sem 4, mutex 4, queue 12, mbox 7, pool 12, timer 18, evgroup 3
    min stack 96, stack used of 192: wake 39, high 55, low 133
  cpuLoad 11 %
RESULT tc10 PASS
```

## tc10 esp32

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 6
    getTick: 21
    sem give+take: 114
    notify set+wait: 147
    mutex lock+unlock: 158
    queue send+recv: 289
    mbox post+fetch: 196
    pool alloc+free: 178
    evgroup set+wait: 139
    timer start+stop: 124
    events post+wait: 142
    getStackFree: 6438
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 412/413 cycles = 1716/1720 ns
    notify: 422/424 cycles = 1758/1766 ns
    queue: 466/467 cycles = 1941/1945 ns
    events: 412/414 cycles = 1716/1725 ns
  isr (min/avg/max):
    entry:            0/35/73 counts = 0/875/1825 ns (n=2385)
    isr->task sem:    420/420/767 cycles = 1750/1750/3195 ns (n=1000)
    isr->task notify: 447/448/794 cycles = 1862/1866/3308 ns (n=1385)
    hw->task best:    1862 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 448
  cpuLoad 4 %
RESULT tc10 PASS
```

## tc10 esp32 qemu

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 8
    getTick: 18
    sem give+take: 61
    notify set+wait: 62
    mutex lock+unlock: 48
    queue send+recv: 103
    mbox post+fetch: 78
    pool alloc+free: 74
    evgroup set+wait: 53
    timer start+stop: 53
    events post+wait: 63
    getStackFree: 210
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 243/272 cycles = 1012/1133 ns
    notify: 244/290 cycles = 1016/1208 ns
    queue: 261/316 cycles = 1087/1316 ns
    events: 245/272 cycles = 1020/1133 ns
  isr (min/avg/max):
    entry:            124/1000/8875 counts = 3100/25000/221875 ns (n=2362)
    isr->task sem:    282/1879/58671 cycles = 1175/7829/244462 ns (n=1000)
    isr->task notify: 268/2352/46255 cycles = 1116/9800/192729 ns (n=1362)
    hw->task best:    4216 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 412
  cpuLoad 1 %
RESULT tc10 PASS
```

## tc11 mega2560

```
tc11 Mega stack used (bytes) of 192, min stack 96
  block  36
  wake   39
  busy   44
  script 66
  irqs 1061 spins 671114
RESULT tc11 PASS
```

## tc11 esp32

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   192
  busy   336
  script 368
  irqs 1054 spins 20893228
RESULT tc11 PASS
```

## tc11 esp32 qemu

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   192
  busy   336
  script 368
  irqs 1058 spins 112651759
RESULT tc11 PASS
```

## tc12 mega2560

```
tc12 Mega stack used (bytes) and kernel+port share at depth 0/4/8/16/32, frame per level 12, min stack 96
  A recursion only        used 40 63 111 207 399  (the chain alone)
  B + probe/tick preempt  used 59 107 155 251 443  kernel+port 59 59 59 59 59
  C + block at bottom     used 48 96 144 240 432  kernel+port 48 48 48 48 48
  D + float, preempt      used 65 113 161 257 449  kernel+port 65 65 65 65 65
  B + probe/tick preempt  isr entry avg/max (ns) 0/0 0/0 0/0 0/0 0/0
  D + float, preempt      isr entry avg/max (ns) 0/0 0/0 0/0 0/0 0/0
  irqs 735 woken 499, worst kernel+port 65 (incl. the task's two bottom frames)
RESULT tc12 PASS
```

## tc12 esp32

```
tc12 ESP32 stack used (bytes) and kernel+port share at depth 0/4/8/16/32, frame per level 32, min stack 512
  A recursion only        used 192 300 300 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 300 352 480 736 1248  kernel+port 300 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 850/850 850/850 850/850 850/850 850/1375
  D + float, preempt      isr entry avg/max (ns) 850/850 850/850 850/1375 850/1375 850/1375
  probes 500 spread over the tick period 629..198971 cycles after a tick boundary; spikes > 1100 ns: 4
    spike 1375 ns entered 3161 cycles after the tick boundary, probe #0 of phase B depth 32
    spike 1375 ns entered 101321 cycles after the tick boundary, probe #0 of phase D depth 8
    spike 1375 ns entered 101927 cycles after the tick boundary, probe #0 of phase D depth 16
    spike 1375 ns entered 198551 cycles after the tick boundary, probe #0 of phase D depth 32
  irqs 820 woken 503, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```

## tc12 esp32 qemu

```
tc12 ESP32 stack used (bytes) and kernel+port share at depth 0/4/8/16/32, frame per level 32, min stack 512
  A recursion only        used 268 268 268 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 332 352 480 736 1248  kernel+port 332 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 6025/44550 8225/194050 4600/7025 4375/7500 5100/7000
  D + float, preempt      isr entry avg/max (ns) 4775/6975 5300/8000 4725/6425 4775/7075 4700/6325
  probes 494 spread over the tick period 1571..277951 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 44550 ns entered 169513 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 7800 ns entered 16356 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 7650 ns entered 113001 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 7075 ns entered 209746 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 5675 ns entered 66408 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 4575 ns entered 163374 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 7325 ns entered 20548 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 11800 ns entered 121130 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 15325 ns entered 220346 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 6950 ns entered 77593 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 4500 ns entered 174020 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 4100 ns entered 30671 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 5150 ns entered 127822 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 4225 ns entered 224478 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 4375 ns entered 81411 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 4500 ns entered 178383 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 605 woken 495, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
