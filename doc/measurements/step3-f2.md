# Measurement step3-f2

After audit fix F2 (boosted-ready mask: rtos_kernel_boostedMask, cold out-of-line
rtos_kernel_scheduleBoosted / rtos_kernel_isrTailBoosted, hot path one mask test more).
Against step2-f1: switch +13 cycles on both boards (Mega 427->440, ESP32 405->418),
mutex lock+unlock +33 / +28 cycles, flash +360 B AVR / +364 B ESP32, bss +1 B.
tc11 "block" on the ESP32 reads 300 instead of 192: that is an interrupt frame landing on
the block task during its run (verified from the stack dump: exception frame with PS,
EPC, a1 = 3FFB1520 and the handler's [HSP-12] pointer), a timing phase effect that any
code size change flips; tc12 phase C and the "busy"/worst figures are unchanged (336/368).

Taken 2026-11-05 20:22, nornCore 85fbf52 + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6666 | 168 | 835 | 9578 | 6 | 242 |
| mega2560 | 6812 | 168 | 835 | 9604 | 6 | 242 |
| esp32 | 9998 | 24 | 3928 | 12689 | 11 | 817 |

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
    pool alloc+free: 481
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
    sem give+take: 122
    notify set+wait: 155
    mutex lock+unlock: 166
    queue send+recv: 305
    mbox post+fetch: 204
    pool alloc+free: 166
    evgroup set+wait: 147
    timer start+stop: 122
    events post+wait: 150
    getStackFree: 7077
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 418/419 cycles = 1741/1745 ns
    notify: 428/429 cycles = 1783/1787 ns
    queue: 476/479 cycles = 1983/1995 ns
    events: 419/420 cycles = 1745/1750 ns
  isr (min/avg/max):
    entry:            0/35/95 counts = 0/875/2375 ns (n=2386)
    isr->task sem:    428/428/775 cycles = 1783/1783/3229 ns (n=1000)
    isr->task notify: 455/456/804 cycles = 1895/1900/3350 ns (n=1386)
    hw->task best:    1895 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 464
  cpuLoad 4 %
RESULT tc10 PASS
```

## tc10 esp32 qemu

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 9
    getTick: 18
    sem give+take: 62
    notify set+wait: 57
    mutex lock+unlock: 50
    queue send+recv: 112
    mbox post+fetch: 80
    pool alloc+free: 57
    evgroup set+wait: 55
    timer start+stop: 68
    events post+wait: 59
    getStackFree: 213
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 247/271 cycles = 1029/1129 ns
    notify: 244/279 cycles = 1016/1162 ns
    queue: 269/323 cycles = 1120/1345 ns
    events: 248/285 cycles = 1033/1187 ns
  isr (min/avg/max):
    entry:            121/1072/10748 counts = 3025/26800/268700 ns (n=2357)
    isr->task sem:    276/1548/45922 cycles = 1150/6450/191341 ns (n=1000)
    isr->task notify: 285/2801/32530 cycles = 1187/11670/135541 ns (n=1357)
    hw->task best:    4212 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 448
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
  irqs 1054 spins 20891386
RESULT tc11 PASS
```

## tc11 esp32 qemu

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   300
  busy   336
  script 368
  irqs 1060 spins 113231745
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
  probes 500 spread over the tick period 635..198965 cycles after a tick boundary; spikes > 1100 ns: 5
    spike 1375 ns entered 99173 cycles after the tick boundary, probe #0 of phase B depth 32
    spike 1375 ns entered 101315 cycles after the tick boundary, probe #0 of phase D depth 8
    spike 1375 ns entered 197939 cycles after the tick boundary, probe #0 of phase D depth 16
    spike 1375 ns entered 53834 cycles after the tick boundary, probe #1 of phase D depth 16
    spike 1375 ns entered 198545 cycles after the tick boundary, probe #0 of phase D depth 32
  irqs 841 woken 502, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```

## tc12 esp32 qemu

```
tc12 ESP32 stack used (bytes) and kernel+port share at depth 0/4/8/16/32, frame per level 32, min stack 512
  A recursion only        used 192 268 268 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 224 352 480 736 1248  kernel+port 224 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 4500/29025 6825/129100 4200/7500 4150/5625 4800/11675
  D + float, preempt      isr entry avg/max (ns) 4000/5400 4050/5600 3925/7050 4100/4900 3975/5525
  probes 495 spread over the tick period 1973..246412 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 29025 ns entered 162661 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 5375 ns entered 13007 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 2925 ns entered 108934 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 2375 ns entered 205318 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 4125 ns entered 62416 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 3625 ns entered 158808 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 4025 ns entered 15519 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 4500 ns entered 112368 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 3525 ns entered 208759 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 4150 ns entered 65606 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 3525 ns entered 162073 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 4150 ns entered 18878 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 3850 ns entered 115416 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 3575 ns entered 211938 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 4000 ns entered 68740 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 2175 ns entered 164919 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 608 woken 496, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
