#!/bin/bash
# Size and timing snapshot of the kernel, taken before and after every change that may
# touch the hot path, the stacks or the code size:
#
#   tools/measure.sh LABEL
#
# builds tc01 for every board and records the image size and the size of the kernel+port
# objects alone (every function compiled, before the linker drops the unused ones), then runs tc10 (cycles per API call, per switch and ISR to task), tc11
# (stack need per task) and tc12 (stack and ISR entry latency against call depth) on the
# Mega (AVR_PORT, default /dev/ttyACM0), the ESP32 (ESP_PORT, default /dev/ttyUSB0) and
# the ESP32 in QEMU. Raw logs go to build/measure/LABEL/, the summary to
# doc/measurements/LABEL.md. A port that is absent is skipped; ESP_PORT= disables the board.
#   MON_SECS=30     seconds to watch a board per application
#   REUSE=1         rebuild the summary from the logs already in build/measure/LABEL/
set -u
cd "$(dirname "$0")/.."
LABEL=${1:?usage: tools/measure.sh LABEL}
OUT=build/measure/$LABEL
SUM=doc/measurements/$LABEL.md
AVR_PORT=${AVR_PORT-/dev/ttyACM0}
ESP_PORT=${ESP_PORT-/dev/ttyUSB0}
ESP_PYTHON=${ESP_PYTHON:-$HOME/dev/tools/esp-venv/bin/python}
QEMU_ESP=${QEMU_ESP:-$HOME/dev/tools/qemu-esp}
MON_SECS=${MON_SECS:-30}
mkdir -p "$OUT" doc/measurements

clean_log() { sed 's/\x1b\[[0-9;]*m//g' | tr -d '\r' | sed 's/\.\.$//'; }

{
    echo "# Measurement $LABEL"
    echo
    echo "Taken $(date '+%Y-%m-%d %H:%M'), nornCore $(git rev-parse --short HEAD)$(git diff --quiet || echo ' + uncommitted changes'), tools/measure.sh."
    echo
    echo "## Size (bytes), tc01"
    echo
    echo "| board | image text | data | bss | kernel+port objects text | data | bss |"
    echo "|---|---|---|---|---|---|---|"
} > "$SUM"

for board in uno mega2560 esp32; do
    if [ "${REUSE:-0}" != 1 ] && ! make -s APP=tc01 BOARD="$board" > "$OUT/size-$board.log" 2>&1; then
        echo "| $board | build failed | | | | | |" >> "$SUM"; continue
    fi
    case $board in
        esp32) SIZE=$HOME/dev/tools/xtensa-esp-elf-16.1.0/bin/xtensa-esp32-elf-size;;
        *)     SIZE=avr-size;;
    esac
    img=$("$SIZE" build/tc01-$board/nornos_tc01.elf | tail -1 | awk '{print $1" | "$2" | "$3}')
    ker=$("$SIZE" -t build/tc01-$board/kernel/*.o build/tc01-$board/port/*/*.o | tail -1 | awk '{print $1" | "$2" | "$3}')
    echo "| $board | $img | $ker |" >> "$SUM"
done

# The lines of a tc10/tc11/tc12 log worth keeping in the summary.
KEY='^tc1[012] |^  |RESULT'

summarize() {   # title logfile
    {
        echo; echo "## $1"; echo; echo '```'
        if grep -q "RESULT" "$2"; then grep -E "$KEY" "$2"; else echo "no RESULT line; see $2"; fi
        echo '```'
    } >> "$SUM"
}

run_hw() {   # app board port
    local app=$1 board=$2 port=$3 log=$OUT/$1-$2.log
    if [ "${REUSE:-0}" = 1 ] && [ -s "$log" ]; then summarize "$app $board" "$log"; return; fi
    if ! make -s APP="$app" BOARD="$board" > "$log" 2>&1 \
       || ! make -s APP="$app" BOARD="$board" PORT="$port" upload >> "$log" 2>&1; then
        summarize "$app $board (build/upload failed)" "$log"; return
    fi
    "$ESP_PYTHON" tools/monitor.py "$port" 115200 "$MON_SECS" 2>&1 | clean_log > "$log"
    summarize "$app $board" "$log"
    echo "$app $board: $(grep -a -o "RESULT $app [A-Z]*" "$log" | tail -1)"
}

run_qemu() {   # app
    local app=$1 log=$OUT/$1-esp32-qemu.log extra="-DTC_TOLERANCE_PERMILLE=30"
    if [ "${REUSE:-0}" = 1 ] && [ -s "$log" ]; then summarize "$app esp32 qemu" "$log"; return; fi
    if ! make -s APP="$app" BOARD=esp32 EXTRA_CFLAGS="$extra" > "$log" 2>&1; then
        summarize "$app esp32 qemu (build failed)" "$log"; return
    fi
    make -s APP="$app" BOARD=esp32 EXTRA_CFLAGS="$extra" QEMU_ESP="$QEMU_ESP" SIM_SECONDS="$MON_SECS" sim 2>&1 | clean_log > "$log"
    summarize "$app esp32 qemu" "$log"
    echo "$app esp32 qemu: $(grep -a -o "RESULT $app [A-Z]*" "$log" | tail -1)"
}

for app in tc10 tc11 tc12; do
    [ -e "$AVR_PORT" ] && run_hw "$app" mega2560 "$AVR_PORT"
    [ -n "$ESP_PORT" ] && [ -e "$ESP_PORT" ] && run_hw "$app" esp32 "$ESP_PORT"
    [ -x "$QEMU_ESP/bin/qemu-system-xtensa" ] && run_qemu "$app"
done
echo "summary: $SUM"
