# Measurement step9-curboost-after-unguarded

Taken 2026-11-09 10:28, nornCore d7864ed + uncommitted changes, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6782 | 170 | 833 | 9827 | 8 | 240 |
| mega2560 | 6928 | 170 | 833 | 9853 | 8 | 240 |
| esp32 | 10082 | 24 | 3920 | 12889 | 13 | 815 |

## tc10 mega2560

```
tc10 Mega F_CPU=16000000 (cycles; 1 cycle = 62500 ps)
  api (one call pair, no switch, min of 64):
    critical section: 3
    getTick: 21
    sem give+take: 149
    notify set+wait: 224
    mutex lock+unlock: 222
    queue send+recv: 461
    mbox post+fetch: 286
    pool alloc+free: 491
    evgroup set+wait: 276
    timer start+stop: 241
    events post+wait: 221
    getStackFree: 1694
    getTimeUs: 47
    getCpuLoad: 847
    sem give+take, ceiling held: 151
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 443/486 cycles = 27687/30375 ns
    notify: 436/476 cycles = 27250/29750 ns
    queue: 550/597 cycles = 34375/37312 ns
    events: 448/485 cycles = 28000/30312 ns
  isr (min/avg/max):
    entry:            50/65/572 counts = 3125/4062/35750 ns (n=2353)
    isr->task sem:    476/493/836 cycles = 29750/30812/52250 ns (n=1000)
    isr->task notify: 491/506/851 cycles = 30687/31625/53187 ns (n=1121)
    isr->task notify, ceiling held: 491/509/717 cycles = 30687/31812/44812 ns (n=232)
    hw->task best:    33812 ns
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
    sem give+take: 115
    notify set+wait: 148
    mutex lock+unlock: 159
    queue send+recv: 291
    mbox post+fetch: 197
    pool alloc+free: 179
    evgroup set+wait: 140
    timer start+stop: 124
    events post+wait: 143
    getStackFree: 6438
    getTimeUs: 122
    getCpuLoad: 188
    sem give+take, ceiling held: 115
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 411/412 cycles = 1712/1716 ns
    notify: 421/423 cycles = 1754/1762 ns
    queue: 465/466 cycles = 1937/1941 ns
    events: 411/413 cycles = 1712/1720 ns
  isr (min/avg/max):
    entry:            0/35/100 counts = 0/875/2500 ns (n=2353)
    isr->task sem:    421/421/768 cycles = 1754/1754/3200 ns (n=1000)
    isr->task notify: 448/449/795 cycles = 1866/1870/3312 ns (n=1121)
    isr->task notify, ceiling held: 448/479/813 cycles = 1866/1995/3387 ns (n=232)
    hw->task best:    1866 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 480
  cpuLoad 13 %
RESULT tc10 PASS
```

## tc10 esp32 qemu

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 15
    getTick: 32
    sem give+take: 92
    notify set+wait: 94
    mutex lock+unlock: 81
    queue send+recv: 175
    mbox post+fetch: 119
    pool alloc+free: 103
    evgroup set+wait: 82
    timer start+stop: 77
    events post+wait: 83
    getStackFree: 303
    getTimeUs: 55
    getCpuLoad: 64
    sem give+take, ceiling held: 77
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 247/271 cycles = 1029/1129 ns
    notify: 245/284 cycles = 1020/1183 ns
    queue: 263/707 cycles = 1095/2945 ns
    events: 430/557 cycles = 1791/2320 ns
  isr (min/avg/max):
    entry:            118/819/10227 counts = 2950/20475/255675 ns (n=2346)
    isr->task sem:    276/2257/54983 cycles = 1150/9404/229095 ns (n=1000)
    isr->task notify: 265/1492/41330 cycles = 1104/6216/172208 ns (n=1115)
    isr->task notify, ceiling held: 284/381/2374 cycles = 1183/1587/9891 ns (n=231)
    hw->task best:    4054 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 464
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
  irqs 1061 spins 669687
RESULT tc11 PASS
```

## tc11 esp32

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   192
  busy   336
  script 368
  irqs 1054 spins 20893852
RESULT tc11 PASS
```

## tc11 esp32 qemu

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   300
  busy   336
  script 300
  irqs 1058 spins 113198562
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
  probes 500 spread over the tick period 629..198971 cycles after a tick boundary; spikes > 1100 ns: 5
    spike 1375 ns entered 3161 cycles after the tick boundary, probe #0 of phase B depth 32
    spike 1375 ns entered 101321 cycles after the tick boundary, probe #0 of phase D depth 8
    spike 1375 ns entered 101927 cycles after the tick boundary, probe #0 of phase D depth 16
    spike 1375 ns entered 198551 cycles after the tick boundary, probe #0 of phase D depth 32
    spike 1375 ns entered 54446 cycles after the tick boundary, probe #1 of phase D depth 32
  irqs 840 woken 503, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```

## tc12 esp32 qemu

```
tc12 ESP32 stack used (bytes) and kernel+port share at depth 0/4/8/16/32, frame per level 32, min stack 512
  A recursion only        used 192 192 192 448 960  (the chain alone)
  B + probe/tick preempt  used 300 428 556 812 1324  kernel+port 300 300 300 300 300
  C + block at bottom     used 224 352 480 736 1248  kernel+port 224 224 224 224 224
  D + float, preempt      used 300 428 556 812 1324  kernel+port 300 300 300 300 300
  B + probe/tick preempt  isr entry avg/max (ns) 0/0 0/0 0/0 0/0 0/0
  D + float, preempt      isr entry avg/max (ns) 0/0 0/0 0/0 0/0 0/0
  probes 0 spread over the tick period 4294967295..0 cycles after a tick boundary; spikes > 1100 ns: 0
  irqs 1 woken 0, worst kernel+port 300 (incl. the task's two bottom frames)
RESULT tc12 FAIL
```
