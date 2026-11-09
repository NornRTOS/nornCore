# Measurement step10-lifecycle-after

Taken 2026-11-09 11:08, nornCore d7864ed + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6852 | 170 | 833 | 9909 | 8 | 240 |
| mega2560 | 6998 | 170 | 833 | 9935 | 8 | 240 |
| esp32 | 10138 | 24 | 3920 | 12975 | 13 | 815 |

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
    critical section: 15
    getTick: 34
    sem give+take: 111
    notify set+wait: 101
    mutex lock+unlock: 79
    queue send+recv: 165
    mbox post+fetch: 124
    pool alloc+free: 107
    evgroup set+wait: 80
    timer start+stop: 80
    events post+wait: 82
    getStackFree: 296
    getTimeUs: 55
    getCpuLoad: 68
    sem give+take, ceiling held: 87
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 588/714 cycles = 2450/2975 ns
    notify: 767/1013 cycles = 3195/4220 ns
    queue: 631/984 cycles = 2629/4100 ns
    events: 454/569 cycles = 1891/2370 ns
  isr (min/avg/max):
    entry:            121/771/10744 counts = 3025/19275/268600 ns (n=2342)
    isr->task sem:    278/1665/54573 cycles = 1158/6937/227387 ns (n=1000)
    isr->task notify: 266/1678/30169 cycles = 1108/6991/125704 ns (n=1111)
    isr->task notify, ceiling held: 290/476/3893 cycles = 1208/1983/16220 ns (n=231)
    hw->task best:    4133 ns
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
  wake   192
  busy   336
  script 368
  irqs 1059 spins 113024889
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
  B + probe/tick preempt  used 368 428 556 812 1324  kernel+port 368 300 300 300 300
  C + block at bottom     used 224 352 480 736 1248  kernel+port 224 224 224 224 224
  D + float, preempt      used 300 428 556 812 1324  kernel+port 300 300 300 300 300
  B + probe/tick preempt  isr entry avg/max (ns) 32450/699650 0/0 0/0 0/0 0/0
  D + float, preempt      isr entry avg/max (ns) 0/0 0/0 0/0 0/0 0/0
  probes 27 spread over the tick period 19937..231253 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 36275 ns entered 170872 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 7775 ns entered 19937 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 5225 ns entered 115811 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 3850 ns entered 212009 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 4175 ns entered 68798 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 3925 ns entered 165262 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 4075 ns entered 21948 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 4200 ns entered 118611 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 4150 ns entered 215272 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 4375 ns entered 72022 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 2025 ns entered 168069 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 4150 ns entered 25270 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 4225 ns entered 121921 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 3600 ns entered 218436 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 2650 ns entered 74892 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 4250 ns entered 171894 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 55 woken 27, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 FAIL
```
