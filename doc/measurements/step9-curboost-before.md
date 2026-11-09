# Measurement step9-curboost-before

Taken 2026-11-09 10:23, nornCore d7864ed + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6752 | 170 | 833 | 9797 | 8 | 240 |
| mega2560 | 6898 | 170 | 833 | 9823 | 8 | 240 |
| esp32 | 10070 | 24 | 3920 | 12863 | 13 | 815 |

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
    getTimeUs: 47
    getCpuLoad: 847
    sem give+take, ceiling held: 338
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 440/478 cycles = 27500/29875 ns
    notify: 433/469 cycles = 27062/29312 ns
    queue: 545/596 cycles = 34062/37250 ns
    events: 445/488 cycles = 27812/30500 ns
  isr (min/avg/max):
    entry:            50/64/820 counts = 3125/4000/51250 ns (n=2353)
    isr->task sem:    463/479/810 cycles = 28937/29937/50625 ns (n=1000)
    isr->task notify: 478/493/818 cycles = 29875/30812/51125 ns (n=1121)
    isr->task notify, ceiling held: 632/651/850 cycles = 39500/40687/53125 ns (n=232)
    hw->task best:    33000 ns
  ram (bytes):
    tcb 42 x 4, sem 4, mutex 4, queue 12, mbox 7, pool 12, timer 18, evgroup 3
    min stack 96, stack used of 192: wake 39, high 55, low 133
  cpuLoad 17 %
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
    getTimeUs: 122
    getCpuLoad: 188
    sem give+take, ceiling held: 227
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 412/413 cycles = 1716/1720 ns
    notify: 422/425 cycles = 1758/1770 ns
    queue: 466/467 cycles = 1941/1945 ns
    events: 412/414 cycles = 1716/1725 ns
  isr (min/avg/max):
    entry:            0/35/70 counts = 0/875/1750 ns (n=2353)
    isr->task sem:    420/420/767 cycles = 1750/1750/3195 ns (n=1000)
    isr->task notify: 447/448/835 cycles = 1862/1866/3479 ns (n=1121)
    isr->task notify, ceiling held: 555/586/920 cycles = 2312/2441/3833 ns (n=232)
    hw->task best:    1862 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 464
  cpuLoad 13 %
RESULT tc10 PASS
```

## tc10 esp32 qemu

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 14
    getTick: 29
    sem give+take: 88
    notify set+wait: 89
    mutex lock+unlock: 75
    queue send+recv: 160
    mbox post+fetch: 111
    pool alloc+free: 90
    evgroup set+wait: 75
    timer start+stop: 72
    events post+wait: 78
    getStackFree: 282
    getTimeUs: 44
    getCpuLoad: 55
    sem give+take, ceiling held: 117
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 1186/1396 cycles = 4941/5816 ns
    notify: 808/1208 cycles = 3366/5033 ns
    queue: 610/880 cycles = 2541/3666 ns
    events: 399/532 cycles = 1662/2216 ns
  isr (min/avg/max):
    entry:            123/739/7559 counts = 3075/18475/188975 ns (n=2336)
    isr->task sem:    292/1596/54878 cycles = 1216/6650/228658 ns (n=1000)
    isr->task notify: 268/1770/38050 cycles = 1116/7375/158541 ns (n=1106)
    isr->task notify, ceiling held: 367/735/29194 cycles = 1529/3062/121641 ns (n=230)
    hw->task best:    4191 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 480
  cpuLoad 11 %
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
  wake   300
  busy   336
  script 192
  irqs 1060 spins 113191638
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
  A recursion only        used 192 192 192 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 224 352 480 736 1248  kernel+port 224 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 7050/50650 4225/6275 3775/7500 4500/18025 3725/5600
  D + float, preempt      isr entry avg/max (ns) 4725/23250 3750/4750 3700/7125 3775/5600 4875/16000
  probes 495 spread over the tick period 1576..240557 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 50650 ns entered 240557 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 6275 ns entered 85141 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 4400 ns entered 181112 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 9425 ns entered 38850 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 4925 ns entered 134317 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 3700 ns entered 230545 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 4500 ns entered 87332 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 3850 ns entered 183766 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 4725 ns entered 40556 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 5875 ns entered 137526 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 4425 ns entered 233788 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 9550 ns entered 92375 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 4875 ns entered 187990 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 11000 ns entered 46151 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 6450 ns entered 141805 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 3300 ns entered 237541 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 611 woken 497, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
