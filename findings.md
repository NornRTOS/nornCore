# nornCore – oberoende djupgranskning

Datum: 2026-11-05. Granskad revision: `85fbf52448139a4e041fc9b2255f423ac92a7192`.

Produktionskod och befintliga tester har inte ändrats. Denna rapport finns även som `doc/deep-audit.md`. Experiment och byggartefakter ligger i `/tmp/norn-audit/`; reproduktionssekvenser och observerade resultat återges nedan så att rapporten inte förutsätter att temporärfilerna finns kvar.

## Resultat och klassificering

**Confirmed** betyder reproducerat fel eller ett direkt verifierat implementationsfel. **Likely** betyder en spårad möjlig exekveringssekvens som ännu inte körts på målplattformen. **Needs verification** betyder en konkret kvarstående fråga som kräver mer riktad verifiering. Allvarlighet avser konsekvens inom angivna förutsättningar, inte hur ofta felet inträffar.

| ID | Klass | Allvarlighet | Fynd och förutsättning |
|---|---|---|---|
| F1 | Confirmed | Hög | Privata taskhändelser avbryter eventgruppsväntan vid giltig API-användning. |
| F2 | Confirmed | Hög | Återväckt mutexägare schemaläggs efter basprioritet; prioritetstaket följer inte med vid återinträde. |
| F3 | Confirmed | Hög på plattform med alignmentskrav | Poolens bytebuffert används som pointerobjekt utan alignmentgaranti. |
| F4 | Confirmed | Medel | Flera AMP-mottagare kan lämnas blockerade trots köade meddelanden. |
| F5 | Confirmed | Hög konsekvens, kräver felaktig image | Core 1-loaderns adresskontroll släpper igenom adresser utanför tilldelat RAM. |
| F6 | Confirmed | Medel | Stora ändliga väntetider/timerintervall feltolkas genom signed differens. |
| F7 | Confirmed | Medel | Giltiga poolblock över 64 KiB kan inte lämnas tillbaka. |
| F8 | Confirmed | Medel för verifiering/CI | `make host-test` maskerar testernas felstatus. |
| F9 | Confirmed | Medel | AVR:s mikrosekundklocka hoppar bakåt efter 65,536 s med 16-bitars tick. |
| F10 | Confirmed | Låg | CPU-last blir fel på AVR vid långa mätintervall. |
| F11 | Confirmed | Låg | Utskrift av `INT32_MIN` innehåller undefined behavior. |

Prioritera F1–F3 för kärnans korrekthet och F8 för tillförlitlig regressionstestning. F4 bör rättas innan flera AMP-mottagare används. F5 är en liten, angelägen rättning av en säkerhetskontroll, men normala korrekta images utlöser inte felet.

## Bekräftade fynd, i prioritetsordning

### F1. Privata händelser väcker task som väntar på eventgrupp

**Plats:** `kernel/rtos_kernel.c:626–632`, `kernel/rtos_evgroup.c:100–118`, `kernel/rtos_kernel.h` (TCB-fältet `evtWait`).

`rtos_evgroupWait` lagrar gruppens väntmask i `evtWait`. `postEventsInner` tolkar samma fält som en väntan på privata händelser utan att kontrollera `waitKind`. Om privata pending-bitar matchar masken nollställs `evtWait`, tasken görs ready och tas bort ur gruppens waiter-mask och timeoutlista. Gruppväntan återvänder utan överlämnat gruppresultat. I den verifierade sekvensen är resultatet 0, även med `RTOS_FOREVER` och utan förfluten tid. Ingen oinitialiserad resultatvariabel behöver antas: `got` var redan 0 från första gruppmatchningen.

**Minimal reproduktion:** Tom grupp; task 2 gör `rtos_evgroupWait(&g, 1, RTOS_WAIT_ANY, RTOS_FOREVER)`. Task 1 gör `rtos_postEvents(2, 1)`. Task 2 återvänder direkt:

```text
eventgroup returned=0 group=0 privatePending=1 tick=0
```

Samma väg nås från `rtos_postEventsFromIsr` och `rtos_broadcastEvents`. Kräver `RTOS_CFG_EVGROUPS=1`, inga felaktiga anropsargument och inget minnesfel.

**Minsta rättning:** Kräv `pT->waitKind == RTOS_WAIT_EVENTS` i den privata händelseväckningen. Behåll postningen till `evtPending`, så att privata händelser fortfarande kan hämtas senare. Nytt regressionstest bör blanda privata händelser med en pågående gruppväntan; befintliga separata funktionstester verifierar inte detta. Stresstestet tillåter ett nollresultat och upptäcker därför inte den felaktiga väckningen.

### F2. Scheduler beaktar endast aktuell tasks effektiva prioritet

**Plats:** `kernel/rtos_kernel.c:167–184`, `kernel/rtos_kernel.h` (`rtos_kernel_isrTailInline`), `kernel/rtos_sync.c:106–170`.

Ready-maskens bitar representerar basprioriteter. Både schemaläggaren och ISR-svansen väljer högsta basprioritet och jämför den med endast den körande taskens `effPrio`. En blockerad mutexägare behåller sitt tak men får inte företräde när den åter blir ready. Att blockera med mutex är uttryckligen understött av fallbackvägen; felet kräver varken för lågt tak eller fel unlockordning.

**Reproduktion:** Task 1 tar mutex med tak 3 och gör `rtos_delay(1)` vid tick 0. Task 2 har gjort `rtos_delay(0)`, återkommer vid tick 1 och kör vidare. När nästa tick väcker task 1 fortsätter task 2:

```text
after tick 2: ownerRan=0 ownerReady=1 ownerEff=3 current=2
```

Task 2 kan därmed fördröja ägaren obegränsat om den fortsätter beräkna. Samma fel finns vid val av nästa task när den aktuella blockerar, och vid handover till en redan prioritetshöjd waiter. Befintliga mutextester låter konkurrenten blockera på mutexen och döljer detta fall.

**Minsta korrekta rättning:** Låt valet av ready task och preemptionsjämförelsen använda effektiv prioritet för samtliga kandidater, i både task- och ISR-vägen. Behåll gärna den nuvarande högsta-bit-vägen när inga ready tasks är prioritetshöjda. En begränsad skanning av höjda kandidater är tillräcklig; en ny generell schedulerarkitektur behövs inte. Lägg inte bara till en jämförelse för en enskild återväckt task: den felaktiga kandidatselektionen finns på flera vägar.

### F3. Minnespoolen gör oalignerade pointeraccesser

**Plats:** `kernel/rtos_pool.c:21`, `:38–55`, `:104`; offentligt kontrakt `kernel/rtos.h:488–490`.

Poolen accepterar `uint8_t *storage`, valfri blockstorlek minst `sizeof(void *)` och positivt blockantal. Den skriver/läser sedan fria listans länkar genom `void **`. Varken startadress eller blockstride garanterar alignment. En alignad start löser inte en udda stride. Deklarerade bytearrayer ger dessutom anledning att undvika typade pointeraccesser genom arrayen enligt C:s aliasregler.

**Reproduktion:** Pointeralignad union med bytearray; `rtos_poolInit(&p, u.bytes + 1, 9, 3)` på host. Alla nuvarande argumentkontroller passerar. UBSan avbryter:

```text
kernel/rtos_pool.c:52:21: runtime error: store to misaligned address ...
for type 'void *', which requires 8 byte alignment
```

På Xtensa har problemet motsvarande 4-bytekrav; AVR kräver inte samma alignment. Exakt hårdvarufelsymptom har inte verifierats på fysisk ESP32. Att C-accessen är ogiltig är bekräftat.

**Minsta rättning:** Läs/skriv länkar med `memcpy` mellan blockets bytes och en lokal pointervariabel. Det behåller nuvarande API och godtyckliga blockstorlekar och undviker både alignment- och aliasproblemet. Ett alternativ är ett uttryckligt alignmentkontrakt plus obligatoriska kontroller och uppdaterade lagringsdeklarationer, men det ändrar vad API:t accepterar. Befintliga pooltester använder jämna blockstorlekar och råkar få tillräckligt alignade globala buffertar.

### F4. AMP tappar väckningar mellan flera mottagare

**Plats:** `hal/xtensa/amp.c:31`, `:40–44`, `:110–118`; stöd för flera mottagare anges i `hal/xtensa/amp.h`.

Två tasks väntar i `amp_receive(..., RTOS_FOREVER)` på tom ring. Två meddelanden anländer innan mottagarkärnan hanterar cross-core-interruptet. Signalen kan vara sammanslagen till ett interrupt. ISR:n ger en binär semafor en gång och väcker en mottagare. Den tar ett meddelande och återvänder utan att signalera att mer data finns. Den andra mottagaren förblir blockerad om inga fler interrupt kommer.

**Verifiering:** Samma `amp.c` kördes med hostkärnan; endast `memw` ersattes med compilerbarriär och hårdvarufunktioner stubbbades i en temporär kopia. Två mottagartasks blockerades, ringens två slots publicerades, och den riktiga `crossCoreIsr` anropades en gång:

```text
received=1 pending=1 blockedReceiversMask=4
```

Detta bekräftar väckningslogiken, inte minnesordningen mellan fysiska kärnor. Nuvarande tc07 har en mottagare per kärna och täcker inte fallet.

**Minsta rättning:** Efter dequeue, om ringen fortfarande innehåller data, för signalen vidare till nästa mottagare via taskvariant av semaforgive medan lokal ringstatus fortfarande skyddas. Den måste kunna schemalägga den väckta tasken. Att bara byta binär till räknande semafor räcker inte: ett sammanslaget hårdvaruinterrupt räknar fortfarande inte antalet meddelanden.

### F5. Core 1-loaderns fönsterkontroll har unsigned underflow

**Plats:** `port/xtensa/port_esp32.c:395–398`, anrop `:428`, skrivning `:443–445`.

Predikatet kontrollerar undre gräns men inte att adressen är högst fönstrets slut innan `END - addr` beräknas. Adresser över slutet ger en stor unsigned differens. OR mellan IRAM- och DRAM-villkoret gör även kontrollen vid IRAM-slutet felaktig: DRAM-villkoret accepterar adressen genom underflow.

**Verifiering med direkt extraherat predikat:**

```text
outside IRAM addr=400a0000 len=4 accepted=1
MMIO addr=60000000 len=4 accepted=1
```

På målplattformens 32-bitars unsigned arithmetic blir exempelvis `0x3ffdc000 - 0x60000000` värdet `0xdffdc000`; fyra bytes godtas. En 36-byte image med magic `0xE9`, ett segment, giltig entry `0x40090000`, segmentadress `0x60000000` och längd 4 passerar kontrollen. Andra passet skriver sedan till den adressen. Den farliga skrivningen kördes inte.

**Konsekvens och räckvidd:** Godtyckliga felaktiga segmentadresser ovanför RAM-fönstren kan medföra otillåtna minnes-/registerskrivningar eller CPU-fel. Felet kräver felaktig image eller fel metadata; normal genererad tc07-image behöver inte utlösa det.

**Minsta rättning:** Kontrollera även `addr <= END` före subtraktionen i respektive fönster, eller använd explicit halvt öppet adressintervall med separat hantering av längd 0. Behåll `len <= END - addr` för att undvika overflow i `addr + len`.

### F6. Halva tickintervallet är en odokumenterad övre gräns

**Plats:** `kernel/rtos_kernel.c:230–239`, `:271`, `:537–545`, `:558`; `kernel/rtos_timer.c:138`, `:200–212`; motsvarande signed deadlinejämförelse i watchdog.

Sortering och förfallotest använder `rtos_tickdiff_t`, som är signed med samma bredd som ticktypen. Det fungerar bara inom halva räknarintervallet. API:t tar hela unsigned typen utan att ange eller kontrollera denna begränsning.

**Reproduktion med 32-bitars tick och en enda task:**

```c
rtos_tick_t before = rtos_getTick();
rtos_delay((rtos_tick_t)0x80000000u);
/* hostens idle hook levererar ett tick i taget */
```

```text
requested=2147483648 elapsed=1
```

En engångstimer med delay `0x80000001` gav:

```text
before tick remaining=0
after one tick fired=1 active=0
```

Med 16-bitars tick blir motsvarande gräns ungefär 32,768 sekunder vid 1 kHz, vilket är praktiskt relevant. Dessutom omvandlar `rtos_delay(RTOS_FOREVER - 1)` sitt ändliga argument till `RTOS_FOREVER` vid `++ticks` och lägger därför inte in någon timeout alls.

**Minsta rättning:** Definiera och kontrollera ett uttryckligt högsta ändligt intervall som respekterar signed differens och delayfunktionens extra tick. Gör motsvarande validering för perioder, timerdelay och watchdogintervall. Att stödja hela unsigned intervallet kräver annan tidsrepresentation eller uppdelade långa väntningar och är en större ändring. Befintliga wraptester passerar räknarvarv med korta väntetider; de verifierar inte långa intervall.

### F7. Poolens returkontroll trunkerar giltiga offset till 16 bitar

**Plats:** `kernel/rtos_pool.c:91–96`, särskilt casten före modulo på rad 95.

På ESP32/host kan poolen vara större än 65535 bytes: `uint16_t blockSize * uint8_t numBlocks` är inte begränsat till en 16-bitars totalsumma där. Returkontrollen konverterar däremot blockets offset till `uint16_t` före `% blockSize`.

**Reproduktion:** Alignad 80000-bytebuffert, blockstorlek 400, 200 block. Allokera 166 gånger; sista blocket har offset 66000. Lämna tillbaka just detta korrekt erhållna block:

```text
allocated offset=66000
panic reason=7 while freeing valid offset 66000
```

`66000 % 400 == 0`, men `464 % 400 != 0` efter trunkering. Inget dubbel-free eller ogiltigt pointerargument behövs. Denna poolstorlek kan rymmas i ESP32:s DRAM; den är inte realistisk på nuvarande AVR-kort.

**Minsta rättning:** Utför offset/modulo utan 16-bitars trunkering, exempelvis med `size_t` efter verifierad tillhörighet. Ändra inte blockstorlekstypen eller pooldesignen.

### F8. Hosttestrunnaren returnerar framgång när tester misslyckas

**Plats:** `Makefile:112`; vidare påverkan `tools/test_all.sh` (hostsektionen).

`test | tail -1 || exit 1` kontrollerar normalt status från `tail`, inte från testprocessen. Ett test som avslutas med assertion, sanitizerfel eller return 1/7 kan därför ge lyckad make-status. Jag såg detta även när LeakSanitizer avbröt processer i denna körmiljö.

**Minimal reproduktion utan ändring av projektet:** Skapa ett temporärt exekverbart skript som skriver en FAIL-rad och gör `exit 7`. Kör:

```sh
make host-test HOST_TESTS=/tmp/norn-audit/fail-test
```

```text
== /tmp/norn-audit/fail-test
FAIL: deliberate audit runner probe
make exit status: 0
```

**Minsta rättning:** Spara varje tests output till en temporär logg, fånga testets egen exitstatus och skriv sista raden efteråt. Eller använd explicit bash med `pipefail` för just denna recipe. Säkerställ att `tools/test_all.sh` får en felstatus. Lägg ett test av själva runnerns felpropagering; fler kärntester löser inte detta.

### F9. Mikrosekundklockan tappar kontinuitet vid 16-bitars tickwrap på AVR

**Plats:** `port/avr/port_avr.c:163–178`; kontrakt `kernel/rtos.h:568–570`.

`rtos_getTimeUs` multiplicerar den redan trunkerade `rtos_tick_t` med mikrosekunder per tick. Med `RTOS_CFG_TICK_BITS=16`, 1 kHz och timerfas 0 går klockan från 65535000 till 0 efter ett tick. Det är inte 32-bitars mikrosekundwrap efter cirka 71 minuter. Vanlig unsigned differens ger då ett enormt fel, inte 1000 mikrosekunder.

**Verifiering:** Oförändrad AVR-port länkad till temporär app med 16-bitars tick och körd i simavr. Tickräknaren sattes på vardera sidan av wrapgränsen, timern stod stilla:

```text
time before=65535000 after=0 difference=4229432296
```

**Minsta rättning:** Basera mikrosekundklockan på ett bredare portägt tidsunderlag, exempelvis den redan befintliga runtimebasen plus timerfas med korrekt omräkning. Behåll ett eget tillräckligt brett underlag även om runtime-statistik är avstängd. Hostportens motsvarande implementation har samma tickbreddsproblem. Xtensa använder sitt 64-bitars cykelunderlag och har inte just detta fel.

### F10. CPU-lastens procentberäkning overflowar på AVR

**Plats:** `kernel/rtos_kernel.c:875`, `port/avr/port.h` (`rtos_port_runtime_t` är `uint32_t`).

`idle * 100ul` beräknas i 32 bitar på AVR. När idle-differensen överstiger 42949672 counts blir procenten fel, långt före runtime-räknarens wrap. Vid 16 MHz och prescaler 64 motsvarar gränsen ungefär 171,8 sekunder ackumulerad idle-tid mellan avläsningar.

**Verifiering i simavr med riktade runtimevärden:** Ackumulera först 50000000 counts i idle via `rtos_getTaskRunTime(0)`, växla testets aktuella TCB till task 1, för runtimebasen till 100000000 och anropa `rtos_getCpuLoad`:

```text
CPU load expected=50 actual=93
```

AVR-assembly använder 32-bitars multiplikation och `__udivmodsi4`, vilket bekräftar att hostens 64-bitars `unsigned long` inte kan användas som bevis för AVR-resultatet. Detta påverkar diagnostik, inte taskvalet.

**Minsta rättning:** Beräkna procentkvoten med tillräcklig mellanbredd eller en overflowfri kvotmetod. Lägg eventuell bred arithmetic utanför critical section, som nuvarande kod redan gör. Xtensa har 64-bitars runtime och denna praktiska gräns gäller inte där.

### F11. Signed utskrift negerar `INT32_MIN`

**Plats:** `hal/uart_fmt.c:37–44`.

`n = -n` kan inte representeras för `INT32_MIN`. Detta är giltigt argument till det publika utskrifts-API:t.

**Reproduktion:** Länka oförändrad `uart_fmt.c` till en hoststub för `uart_putc` och anropa `uart_putI32(INT32_MIN)` med UBSan:

```text
hal/uart_fmt.c:42:11: runtime error: negation of -2147483648
cannot be represented in type 'int'
```

**Minsta rättning:** Konvertera till unsigned magnitud och beräkna negativa magnituder med unsigned subtraktion, exempelvis `0u - (uint32_t)n`. Behåll befintlig decimalrutin och buffertstorlek. Ingen ny formatteringsabstraktion behövs.

## Övriga spårade risker

### R1. Likely – UART-callback och argument publiceras osammanhängande

**Plats:** `hal/xtensa/uart.c:76–90`.

Vid byte av en redan aktiv callback skriver `uart_onReceive` först `_rxArg`, sedan `_rxFn`, utan critical section. Ett RX-interrupt mellan skrivningarna kan anropa gammal funktion med nytt argument. Om argumenttyperna skiljer kan följden bli minneskorruption trots två i sig giltiga registreringar. Att sätta callback till NULL har samma övergångsfönster med gamla funktionen och eventuellt NULL-argument. Kräver anrop efter att mottagning aktiverats och ett interrupt vid rätt tidpunkt; inte verifierat med fysisk UART.

**Minsta rättning:** Skydda publiceringen av callback/argument och interruptkonfigurationen med portens critical section. Verifiera byte och avregistrering med RX-trafik. Första registreringen före schedulerstart påverkas inte av denna sekvens.

### R2. Likely – eventgruppsläsaren kan ge sönderrivet värde på AVR

**Plats:** `kernel/rtos_evgroup.c:73–75`.

`rtos_evgroupGet` läser 16/32-bitars gruppbitar utan critical section på en 8-bitars CPU, medan ISR eller annan task kan ändra dem. Genererad AVR-assembly för standardens 16-bitars eventtyp innehåller två oskyddade läsningar:

```asm
movw r30,r24
ld   r24,Z
ldd  r25,Z+1
ret
```

Exempel: tasken läser lågbyte 0 ur grupp 0; ett ISR sätter 0x0101; tasken läser högbyte 1 och returnerar 0x0100. Detta är ingen sammanhängande ögonblicksbild av API-operationernas gruppvärden. Den exakta interleavingen har inte injicerats i simavr. `RTOS_CFG_EVT_BITS=8` har inte denna flerbytesrisk på AVR.

**Minsta rättning:** Ta en lokal snapshot inne i en nestbar critical section, precis som `rtos_getTick`. Det fungerar även för de tillåtna läsningarna inifrån ISR.

### R3. Needs verification – tickless förutsätter begränsad sömn i cykler, men begränsar i ticks

**Plats:** `port/xtensa/port_esp32.c:498–525`.

Maxgränsen är 1000 ticks oavsett tickfrekvens. Catch-up jämför CCOUNT-differenser som signed 32 bitar. Vid 240 MHz och 100 Hz motsvarar 1000 ticks 2,4 miljarder cykler, större än `INT32_MAX`; vid ännu lägre tickfrekvens kan produkten även slå runt. Sådana tickfrekvenser passerar portens delbarhetskontroll. Default 1 kHz ger 240 miljoner cykler och ligger inom gränsen.

**Nästa verifiering:** Kör ensam idle/timer med 100 Hz, tickless och inga andra wakeupkällor i QEMU och på kort, med jämförelse mot extern tid. Kontrollera även runtime under catch-up och timerstart från ett annat väckande ISR innan de missade tickarna bokförts.

**Avgränsad rättning om bekräftat:** Begränsa sömnens cykelavstånd till strikt mindre än halva CCOUNT-intervallet utöver befintlig watchdoggräns; använd overflowfri arithmetic. Ingen generell tickless-omskrivning är motiverad innan testet.

## Verifiering som genomförts

- Alla 11 befintliga hosttestprogram kördes direkt med kontrollerad egen exitstatus; alla passerade med AddressSanitizer/UBSan när LeakSanitizer inaktiverades för denna miljö. `make host-test` skriver över `ASAN_OPTIONS`, så direkt körning användes.
- Samtliga 11 byggdes dessutom färskt under `/tmp/norn-audit/` med `gcc -O2 -fsanitize=undefined -fno-sanitize-recover=all` och respektive befintlig testkonfiguration. Alla passerade. Detta innefattar stress samt 16-/32-bitars wraptester.
- Separata reproduktioner för F1, F2, F3, F6, F7, F8 och F11 kördes mot oförändrad implementation. F4 använde en temporär hostanpassning av hårdvarubarriär och hårdvaruanrop; F5 verifierades genom extraherat predikat och genomspårning av loaderns båda pass.
- F9 och F10 verifierades med oförändrad AVR-port i simavr och temporär testapplikation som sätter tick-/runtimevärden för att nå gränserna direkt.
- `tc01` byggdes färskt för Mega2560 och ESP32, inklusive respektive assembler och länkning. AVR-assembly för CPU-last och eventgruppsläsning undersöktes.
- ESP32 `tc01` kördes i QEMU. Första körningen med hårdvarans standardtolerans gav enbart för låg simulerad ISR-frekvens (1491 mot accepterat 1495–1505), vilket stämmer med den kända skillnaden i QEMU-timing. En färsk separat byggnad med projektets emulatorinställning `TC_TOLERANCE_PERMILLE=30` gav `RESULT tc01 PASS`.
- Ingen flashning, fysisk hårdvarukörning eller extern kommunikation utfördes.

Exempel på hostkompilering av ett riktat experiment:

```sh
gcc -std=c99 -O2 -Ikernel -Iport/host -Itest \
    /tmp/norn-audit/delay_long.c kernel/*.c port/host/port_host.c \
    -o /tmp/norn-audit/delay_long
```

För misalignment/formatteringskontroll användes `-fsanitize=undefined -fno-sanitize-recover=all`. Testernas egna konfigurationskataloger valdes före `-Itest` i den färska fullständiga hostkörningen.

## Granskade områden utan ytterligare bekräftade fel

- Kärnans ready-/timeoutlistor, taskstart/retur, stacktrimning/canary, suspend/resume, privata events och notifications följdes över blockering och väckning. Direkthandover samt borttagning ur waiter- och timeoutlistor ligger i samma interruptskyddade flöde. Inga ytterligare fel bekräftades för korta intervall och giltiga objektlivstider.
- Semaforer, köer och mailboxar granskades för FIFO, full/tom-gränser, timeout kontra handover, ISR-anrop och suspenderad mottagare. Itemstorlek och kapacitet är båda 8-bitars; största slot-offset ryms även i AVR:s 16-bitars unsigned arithmetic. Inget nytt kö-/mailboxfel bekräftades.
- Heapens size-overflowkontroll, split, boundary tags, tvåsidig sammanslagning och statistik följdes. Ingen ny defekt bekräftades vid giltiga pointerargument och korrekt initierad arena. Felaktiga/interiöra free-pointers och dubbel-free medan andra block fortfarande är allokerade är inte fullständigt skyddade; de är ogiltig applikationsanvändning och rapporteras inte som vanliga allocatorfel.
- Software timers granskades för listmodifiering inifrån callback, periodisk återarmning, pendinglista, stop och timer-task. Ingen ytterligare bekräftad listkorruption hittades. Restart/stop av callback som redan är uttagen för exekvering behöver ett tydligare livstidskontrakt om applikationen vill återanvända/frigöra timerobjekt samtidigt.
- AVR:s context switch och ISR-wrapper följdes register för register mot den preparerade stacken, inklusive 3-byte-PC och RAMPZ. Xtensas startup, vector offsets, exception frame, registerfönsterspill, taskstart och lazy FPU följdes tillsammans med C-porten och `port_regs.h`. Ingen ny assemblerdefekt bekräftades; detta är inte ett bevis för alla möjliga exceptionstidpunkter eller framtida compiler/ABI-varianter.
- Minneslayouterna för single/core0/core1, delad AMP-yta, flashsektioner, ROM-symboler och BSS-init lästes tillsammans med makefragmenten. Core0/core1:s angivna RAM-fönster överlappar inte. Ingen ytterligare konkret länkfelorsak hittades. Idle/main-stackens marginal saknar explicit linkerreservation/kontroll och har inte verifierats för en image som nästan fyller DRAM.
- HAL:s GPIO, UART, LCD, DHT11, delays, trace och boardtimers samt samtliga applikationer/testkonfigurationer och verktyg granskades. Pollande periferitidsgränser och baud-/GPIO-argument förutsätter giltig konfiguration. DHT11-pulsmätning under preemption och samtidiga GPIO-toggle på ESP32 kräver applikationsmässig timing/synkronisering; ingen fysisk sensorverifiering gjordes.

## Viktiga kvarstående begränsningar

- Hostporten har explicita simulerade interrupts och egna stora ucontext-stackar. Den bevisar inte hårdvarutiming, verkligt stackbehov eller registerbevarande vid asynkrona avbrott. Färska optimerade hosttester kompletterar därför men ersätter inte porttesterna.
- Ingen fullständig testmatris kördes för varje kombination av valfria features, tickfrekvens och taskantal. Assertions-off täcks av befintliga `test_checks`; avstängd notifications i kombination med timer-task och ovanliga gränskonfigurationer behöver egna buildkontroller.
- Konfigurationssignaturen är inte en entydig identitet för konfigurationen: tickfrekvensen packas som `(HZ / 100) & 15`. Exempelvis 1000 och 1024 Hz kolliderar och båda delar 240 MHz exakt. Signaturematchning kan därför inte tas som bevis för identiska binärkonfigurationer. Ingen konkret minneskorruption från just denna frekvenskollision demonstrerades.
- PLL/analogregister, flashcache/MMU och resetbeteende verifierades inte elektriskt eller mot externa hårdvarumanualer i denna kodgranskning. Befintliga kommentarer om tidigare mätningar användes inte som bevis för korrekthet.
- Ingen generell prestandaförbättring rekommenderas utan mätning. F2 kräver en korrekt taskselektion även om den långsamma fallbackvägen kostar fler instruktioner. F6/R3 förbättrar tidsmässig förutsägbarhet genom uttryckliga giltighetsgränser. De befintliga O(1)-handovervägarna och högsta-bit-optimeringarna bör behållas där deras invariants gäller.

## Rekommenderad rättningsordning

1. Rätta F1 och inför regressionen med blandade events. Rätta samtidigt F8 så att framtida misslyckade regressioner faktiskt stoppar körningen.
2. Rätta F2 i båda schedulergränserna och verifiera återväckt ägare, handover och flera nästlade tak. Rätta F3 med en lokalt avgränsad representation av fria länkar.
3. Rätta F4 och F5 före utökad AMP-användning. Prova sammanslagna interrupts med flera blockerade mottagare samt imageadresser vid och utanför varje fönstergräns.
4. Rätta F6/F7/F9 och därefter diagnostikfelen F10/F11. Bevara de befintliga wraptesterna och lägg till just de saknade intervall-/storleksgränserna.
5. Verifiera R1–R3 med riktade avbrotts-/porttester. Undvik större omstrukturering innan de smala rättningarna är verifierade.
