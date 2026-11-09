# Measurement step8-tie-before

Taken 2026-11-09 09:57, nornCore d7864ed + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6672 | 170 | 833 | 9711 | 8 | 240 |
| mega2560 | 6818 | 170 | 833 | 9737 | 8 | 240 |
| esp32 | 9974 | 24 | 3920 | 12755 | 13 | 815 |

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
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 440/476 cycles = 27500/29750 ns
    notify: 433/473 cycles = 27062/29562 ns
    queue: 545/592 cycles = 34062/37000 ns
    events: 445/479 cycles = 27812/29937 ns
  isr (min/avg/max):
    entry:            50/63/523 counts = 3125/3937/32687 ns (n=2355)
    isr->task sem:    463/479/810 cycles = 28937/29937/50625 ns (n=1000)
    isr->task notify: 478/493/715 cycles = 29875/30812/44687 ns (n=1355)
    hw->task best:    33000 ns
  ram (bytes):
    tcb 42 x 4, sem 4, mutex 4, queue 12, mbox 7, pool 12, timer 18, evgroup 3
    min stack 96, stack used of 192: wake 38, high 55, low 133
  cpuLoad 7 %
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
  cpuLoad 3 %
RESULT tc10 PASS
```

## tc10 esp32 qemu

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 8
    getTick: 18
    sem give+take: 62
    notify set+wait: 63
    mutex lock+unlock: 48
    queue send+recv: 104
    mbox post+fetch: 80
    pool alloc+free: 75
    evgroup set+wait: 54
    timer start+stop: 53
    events post+wait: 58
    getStackFree: 209
    getTimeUs: 39
    getCpuLoad: 46
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 1254/1423 cycles = 5225/5929 ns
    notify: 924/1381 cycles = 3850/5754 ns
    queue: 815/1136 cycles = 3395/4733 ns
    events: 249/505 cycles = 1037/2104 ns
  isr (min/avg/max):
    entry:            141/1154/8330 counts = 3525/28850/208250 ns (n=2294)
    isr->task sem:    271/2079/55575 cycles = 1129/8662/231562 ns (n=1000)
    isr->task notify: 257/2348/44973 cycles = 1070/9783/187387 ns (n=1294)
    hw->task best:    4595 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 464
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
  wake   300
  busy   336
  script 336
  irqs 1060 spins 113215743
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
  A recursion only        used 192 192 268 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 224 352 480 736 1248  kernel+port 224 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 4350/29275 4200/13850 4175/8150 4325/7075 4175/8775
  D + float, preempt      isr entry avg/max (ns) 3800/5150 3800/5200 3850/6250 4075/8600 4000/6825
  probes 495 spread over the tick period 1902..241128 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 29275 ns entered 163380 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 4750 ns entered 13563 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 2975 ns entered 109659 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 2225 ns entered 205979 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 3950 ns entered 63130 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 2100 ns entered 159196 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 3875 ns entered 16249 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 3975 ns entered 112912 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 2125 ns entered 209059 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 3850 ns entered 66220 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 2025 ns entered 162418 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 4350 ns entered 19856 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 4200 ns entered 116501 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 3650 ns entered 212969 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 3975 ns entered 69662 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 4875 ns entered 166505 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 608 woken 496, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
