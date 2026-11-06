# Measurement step4-small

After audit fixes F3 (pool links via memcpy), F7 (pool offsets in size_t), F5 (core 1
window upper bound), F11 (INT32_MIN print), R2 (event group snapshot), R1 (UART callback
in a critical section), R3 (tickless bound in cycles). Against step3b-sched: switch, ISR
to task and stacks unchanged on both boards; pool alloc+free 481->488 cycles on the Mega
and 158->178 on the ESP32 (memcpy of the link, byte wise where alignment is unknown);
kernel+port objects -32 B AVR, +83 B ESP32, images unchanged.

Taken 2026-11-06 08:31, nornCore 85fbf52 + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6670 | 170 | 833 | 9550 | 8 | 240 |
| mega2560 | 6816 | 170 | 833 | 9576 | 8 | 240 |
| esp32 | 9962 | 24 | 3920 | 12653 | 13 | 815 |

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
    mbox post+fetch: 282
    pool alloc+free: 488
    evgroup set+wait: 273
    timer start+stop: 230
    events post+wait: 218
    getStackFree: 1694
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 440/476 cycles = 27500/29750 ns
    notify: 432/467 cycles = 27000/29187 ns
    queue: 545/594 cycles = 34062/37125 ns
    events: 445/487 cycles = 27812/30437 ns
  isr (min/avg/max):
    entry:            50/64/609 counts = 3125/4000/38062 ns (n=2352)
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
    timer start+stop: 122
    events post+wait: 142
    getStackFree: 7077
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 412/413 cycles = 1716/1720 ns
    notify: 422/424 cycles = 1758/1766 ns
    queue: 466/467 cycles = 1941/1945 ns
    events: 412/414 cycles = 1716/1725 ns
  isr (min/avg/max):
    entry:            0/35/73 counts = 0/875/1825 ns (n=2385)
    isr->task sem:    420/420/767 cycles = 1750/1750/3195 ns (n=1000)
    isr->task notify: 447/448/835 cycles = 1862/1866/3479 ns (n=1385)
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
    critical section: 16
    getTick: 31
    sem give+take: 93
    notify set+wait: 95
    mutex lock+unlock: 82
    queue send+recv: 174
    mbox post+fetch: 113
    pool alloc+free: 105
    evgroup set+wait: 75
    timer start+stop: 80
    events post+wait: 80
    getStackFree: 296
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 1256/1451 cycles = 5233/6045 ns
    notify: 948/1328 cycles = 3950/5533 ns
    queue: 616/954 cycles = 2566/3975 ns
    events: 429/555 cycles = 1787/2312 ns
  isr (min/avg/max):
    entry:            131/1160/8478 counts = 3275/29000/211950 ns (n=2288)
    isr->task sem:    293/2716/36811 cycles = 1220/11316/153379 ns (n=1000)
    isr->task notify: 281/2145/22667 cycles = 1170/8937/94445 ns (n=1288)
    hw->task best:    4445 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 464
  cpuLoad 2 %
RESULT tc10 PASS
```

## tc11 mega2560

```
tc11 Mega stack used (bytes) of 192, min stack 96
  block  36
  wake   39
  busy   44
  script 66
  irqs 1061 spins 671258
RESULT tc11 PASS
```

## tc11 esp32

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   192
  busy   336
  script 368
  irqs 1054 spins 20893546
RESULT tc11 PASS
```

## tc11 esp32 qemu

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   192
  busy   336
  script 368
  irqs 1059 spins 112996389
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
  D + float, preempt      isr entry avg/max (ns) 850/850 850/1300 850/1375 850/1375 850/1375
  probes 500 spread over the tick period 635..198965 cycles after a tick boundary; spikes > 1100 ns: 5
    spike 1375 ns entered 99173 cycles after the tick boundary, probe #0 of phase B depth 32
    spike 1300 ns entered 196604 cycles after the tick boundary, probe #1 of phase D depth 4
    spike 1375 ns entered 101315 cycles after the tick boundary, probe #0 of phase D depth 8
    spike 1375 ns entered 197939 cycles after the tick boundary, probe #0 of phase D depth 16
    spike 1375 ns entered 198545 cycles after the tick boundary, probe #0 of phase D depth 32
  irqs 841 woken 502, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```

## tc12 esp32 qemu

```
tc12 ESP32 stack used (bytes) and kernel+port share at depth 0/4/8/16/32, frame per level 32, min stack 512
  A recursion only        used 300 300 300 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 268 352 480 736 1248  kernel+port 268 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 6650/27075 4725/8225 4300/5050 4275/6050 4300/6525
  D + float, preempt      isr entry avg/max (ns) 4150/5100 3700/4875 3625/4775 3775/6025 3650/5650
  probes 494 spread over the tick period 2577..240511 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 27075 ns entered 30759 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 20650 ns entered 124839 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 6775 ns entered 218989 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 14475 ns entered 78077 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 11425 ns entered 174508 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 11125 ns entered 32218 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 6200 ns entered 128465 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 4625 ns entered 225395 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 9075 ns entered 83567 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 8700 ns entered 180674 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 8150 ns entered 37965 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 9725 ns entered 135932 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 7950 ns entered 232744 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 7375 ns entered 89839 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 7375 ns entered 186851 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 7150 ns entered 43983 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 611 woken 495, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
