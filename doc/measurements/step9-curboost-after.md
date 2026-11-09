# Measurement step9-curboost-after

Taken 2026-11-09 10:38, nornCore d7864ed + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6790 | 170 | 833 | 9835 | 8 | 240 |
| mega2560 | 6936 | 170 | 833 | 9861 | 8 | 240 |
| esp32 | 10086 | 24 | 3920 | 12897 | 13 | 815 |

## tc10 mega2560

```
tc10 Mega F_CPU=16000000 (cycles; 1 cycle = 62500 ps)
  api (one call pair, no switch, min of 64):
    critical section: 3
    getTick: 21
    sem give+take: 145
    notify set+wait: 220
    mutex lock+unlock: 218
    queue send+recv: 453
    mbox post+fetch: 282
    pool alloc+free: 487
    evgroup set+wait: 272
    timer start+stop: 241
    events post+wait: 217
    getStackFree: 1694
    getTimeUs: 47
    getCpuLoad: 847
    sem give+take, ceiling held: 153
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 439/479 cycles = 27437/29937 ns
    notify: 432/470 cycles = 27000/29375 ns
    queue: 544/587 cycles = 34000/36687 ns
    events: 444/484 cycles = 27750/30250 ns
  isr (min/avg/max):
    entry:            50/65/548 counts = 3125/4062/34250 ns (n=2352)
    isr->task sem:    471/488/826 cycles = 29437/30500/51625 ns (n=1000)
    isr->task notify: 486/501/834 cycles = 30375/31312/52125 ns (n=1120)
    isr->task notify, ceiling held: 493/511/719 cycles = 30812/31937/44937 ns (n=232)
    hw->task best:    33500 ns
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
    sem give+take: 112
    notify set+wait: 145
    mutex lock+unlock: 156
    queue send+recv: 285
    mbox post+fetch: 194
    pool alloc+free: 176
    evgroup set+wait: 137
    timer start+stop: 124
    events post+wait: 140
    getStackFree: 6438
    getTimeUs: 122
    getCpuLoad: 188
    sem give+take, ceiling held: 115
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 408/409 cycles = 1700/1704 ns
    notify: 418/420 cycles = 1741/1750 ns
    queue: 461/462 cycles = 1920/1925 ns
    events: 408/410 cycles = 1700/1708 ns
  isr (min/avg/max):
    entry:            0/35/106 counts = 0/875/2650 ns (n=2353)
    isr->task sem:    418/418/765 cycles = 1741/1741/3187 ns (n=1000)
    isr->task notify: 445/446/792 cycles = 1854/1858/3300 ns (n=1121)
    isr->task notify, ceiling held: 449/480/814 cycles = 1870/2000/3391 ns (n=232)
    hw->task best:    1854 ns
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
    critical section: 10
    getTick: 22
    sem give+take: 70
    notify set+wait: 70
    mutex lock+unlock: 61
    queue send+recv: 102
    mbox post+fetch: 77
    pool alloc+free: 67
    evgroup set+wait: 53
    timer start+stop: 53
    events post+wait: 57
    getStackFree: 208
    getTimeUs: 39
    getCpuLoad: 46
    sem give+take, ceiling held: 54
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 423/512 cycles = 1762/2133 ns
    notify: 338/429 cycles = 1408/1787 ns
    queue: 318/416 cycles = 1325/1733 ns
    events: 272/324 cycles = 1133/1350 ns
  isr (min/avg/max):
    entry:            122/927/8421 counts = 3050/23175/210525 ns (n=2343)
    isr->task sem:    269/2226/54968 cycles = 1120/9275/229033 ns (n=1000)
    isr->task notify: 267/1886/14723 cycles = 1112/7858/61345 ns (n=1112)
    isr->task notify, ceiling held: 310/575/8212 cycles = 1291/2395/34216 ns (n=231)
    hw->task best:    4162 ns
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
  irqs 1060 spins 669839
RESULT tc11 PASS
```

## tc11 esp32

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   192
  busy   336
  script 368
  irqs 1054 spins 20894881
RESULT tc11 PASS
```

## tc11 esp32 qemu

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   300
  busy   336
  script 336
  irqs 1058 spins 113063214
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
  B + probe/tick preempt  isr entry avg/max (ns) 4775/29900 4475/14250 4425/8050 4225/7875 4825/18125
  D + float, preempt      isr entry avg/max (ns) 5275/14075 4725/6250 4900/8275 4825/8350 4925/9975
  probes 494 spread over the tick period 1738..262710 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 29900 ns entered 163833 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 6350 ns entered 13965 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 5400 ns entered 110332 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 2300 ns entered 206140 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 4450 ns entered 63333 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 4200 ns entered 159885 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 2450 ns entered 16064 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 4600 ns entered 113240 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 3825 ns entered 209645 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 4150 ns entered 66356 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 3950 ns entered 162959 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 4050 ns entered 19583 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 4450 ns entered 116305 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 3650 ns entered 212708 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 4550 ns entered 69595 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 3875 ns entered 166135 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 607 woken 495, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
