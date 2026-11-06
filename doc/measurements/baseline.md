# Measurement baseline

Taken 2026-11-05 19:39, nornCore 85fbf52, tools/measure.sh.

## Size (bytes), tc01

| board | image text | data | bss | kernel+port objects text | data | bss |
|---|---|---|---|---|---|---|
| uno | 6302 | 168 | 834 | 9194 | 6 | 241 |
| mega2560 | 6450 | 168 | 834 | 9220 | 6 | 241 |
| esp32 | 9634 | 24 | 3928 | 12180 | 11 | 816 |

## tc10 mega2560

```
tc10 Mega F_CPU=16000000 (cycles; 1 cycle = 62500 ps)
  api (one call pair, no switch, min of 64):
    critical section: 3
    getTick: 21
    sem give+take: 133
    notify set+wait: 208
    mutex lock+unlock: 186
    queue send+recv: 429
    mbox post+fetch: 269
    pool alloc+free: 468
    evgroup set+wait: 260
    timer start+stop: 230
    events post+wait: 208
    getStackFree: 1694
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 427/461 cycles = 26687/28812 ns
    notify: 419/455 cycles = 26187/28437 ns
    queue: 526/574 cycles = 32875/35875 ns
    events: 432/466 cycles = 27000/29125 ns
  isr (min/avg/max):
    entry:            50/63/499 counts = 3125/3937/31187 ns (n=2348)
    isr->task sem:    450/465/687 cycles = 28125/29062/42937 ns (n=1000)
    isr->task notify: 465/480/702 cycles = 29062/30000/43875 ns (n=1348)
    hw->task best:    32187 ns
  ram (bytes):
    tcb 42 x 4, sem 4, mutex 4, queue 12, mbox 7, pool 12, timer 18, evgroup 3
    min stack 96, stack used of 192: wake 38, high 55, low 133
  cpuLoad 11 %
RESULT tc10 PASS
```

## tc10 esp32

```
tc10 ESP32 F_CPU=240000000 (cycles; 1 cycle = 4166 ps)
  api (one call pair, no switch, min of 64):
    critical section: 6
    getTick: 21
    sem give+take: 109
    notify set+wait: 142
    mutex lock+unlock: 138
    queue send+recv: 279
    mbox post+fetch: 191
    pool alloc+free: 153
    evgroup set+wait: 134
    timer start+stop: 122
    events post+wait: 137
    getStackFree: 6438
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 405/406 cycles = 1687/1691 ns
    notify: 415/417 cycles = 1729/1737 ns
    queue: 457/458 cycles = 1904/1908 ns
    events: 405/407 cycles = 1687/1695 ns
  isr (min/avg/max):
    entry:            0/35/79 counts = 0/875/1975 ns (n=2385)
    isr->task sem:    417/417/764 cycles = 1737/1737/3183 ns (n=1000)
    isr->task notify: 444/445/793 cycles = 1850/1854/3304 ns (n=1385)
    hw->task best:    1850 ns
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
    critical section: 14
    getTick: 31
    sem give+take: 94
    notify set+wait: 95
    mutex lock+unlock: 82
    queue send+recv: 159
    mbox post+fetch: 113
    pool alloc+free: 84
    evgroup set+wait: 78
    timer start+stop: 71
    events post+wait: 80
    getStackFree: 274
  switch (per switch incl. signalling, min/avg over 500 round trips):
    sem: 1245/1426 cycles = 5187/5941 ns
    notify: 970/1380 cycles = 4041/5750 ns
    queue: 657/1019 cycles = 2737/4245 ns
    events: 431/578 cycles = 1795/2408 ns
  isr (min/avg/max):
    entry:            123/1011/11541 counts = 3075/25275/288525 ns (n=2295)
    isr->task sem:    323/1990/55354 cycles = 1345/8291/230641 ns (n=1000)
    isr->task notify: 287/2079/39149 cycles = 1195/8662/163120 ns (n=1295)
    hw->task best:    4270 ns
  ram (bytes):
    tcb 72 x 4, sem 4, mutex 4, queue 16, mbox 12, pool 20, timer 28, evgroup 4
    min stack 512, stack used of 1024: wake 300, high 384, low 448
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
  irqs 1070 spins 680034
RESULT tc11 PASS
```

## tc11 esp32

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  192
  wake   192
  busy   336
  script 368
  irqs 1054 spins 20895715
RESULT tc11 PASS
```

## tc11 esp32 qemu

```
tc11 ESP32 stack used (bytes) of 1024, min stack 512
  block  300
  wake   300
  busy   336
  script 336
  irqs 1060 spins 113448115
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
  A recursion only        used 192 268 268 448 960  (the chain alone)
  B + probe/tick preempt  used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  C + block at bottom     used 224 352 480 736 1248  kernel+port 224 224 224 224 224
  D + float, preempt      used 368 496 624 880 1392  kernel+port 368 368 368 368 368
  B + probe/tick preempt  isr entry avg/max (ns) 4350/29750 4725/13225 4925/9075 4900/7575 5075/14775
  D + float, preempt      isr entry avg/max (ns) 4875/17900 4950/7400 4850/7250 4775/6325 4425/13175
  probes 495 spread over the tick period 3088..241611 cycles after a tick boundary; spikes > 1100 ns: 16
    spike 29750 ns entered 165236 cycles after the tick boundary, probe #0 of phase B depth 0
    spike 4725 ns entered 15325 cycles after the tick boundary, probe #1 of phase B depth 0
    spike 2800 ns entered 111411 cycles after the tick boundary, probe #2 of phase B depth 0
    spike 2175 ns entered 207768 cycles after the tick boundary, probe #3 of phase B depth 0
    spike 4050 ns entered 64963 cycles after the tick boundary, probe #4 of phase B depth 0
    spike 3600 ns entered 161400 cycles after the tick boundary, probe #5 of phase B depth 0
    spike 4200 ns entered 18150 cycles after the tick boundary, probe #6 of phase B depth 0
    spike 4000 ns entered 114725 cycles after the tick boundary, probe #7 of phase B depth 0
    spike 3650 ns entered 211266 cycles after the tick boundary, probe #8 of phase B depth 0
    spike 4125 ns entered 68081 cycles after the tick boundary, probe #9 of phase B depth 0
    spike 3500 ns entered 164558 cycles after the tick boundary, probe #10 of phase B depth 0
    spike 3900 ns entered 21328 cycles after the tick boundary, probe #11 of phase B depth 0
    spike 3950 ns entered 117943 cycles after the tick boundary, probe #12 of phase B depth 0
    spike 3625 ns entered 214497 cycles after the tick boundary, probe #13 of phase B depth 0
    spike 3800 ns entered 71203 cycles after the tick boundary, probe #14 of phase B depth 0
    spike 3575 ns entered 167774 cycles after the tick boundary, probe #15 of phase B depth 0
  irqs 608 woken 496, worst kernel+port 368 (incl. the task's two bottom frames)
RESULT tc12 PASS
```
