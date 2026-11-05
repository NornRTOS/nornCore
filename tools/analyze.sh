#!/bin/bash
# Static analysis of the kernel, the ports and the HAL with gcc -fanalyzer (gcc 14):
# every source file is compiled alone with the analyzer on, for the host (unit test
# configuration), the AVR (tc01 configuration, ATmega2560) and the ESP32 (tc01
# configuration) when their compilers are present. Prints the diagnostics, indented,
# under the architecture they came from. The one known report, the deliberate endless
# loop after a task function returns (rtos_kernel.c), is filtered out.
set -u
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
XG=${XTENSA_ROOT:-$HOME/dev/tools/xtensa-esp-elf-16.1.0}/bin/xtensa-esp32-elf-gcc

run() {   # name compiler flags... ; files from stdin
    local name=$1; shift
    local f
    while read -r f; do
        "$@" -fanalyzer -c "$f" -o "$TMP/x.o" 2>&1 \
          | grep -v '^ *|' | grep 'warning:\|error:' | grep -v 'infinite loop' | sed "s/^/  /"
    done
    echo "$name: analyzed"
}

echo "== host (test/rtos_config.h)"
ls kernel/*.c port/host/*.c | run host gcc -std=c99 -Wall -Wextra -O1 -Ikernel -Iport/host -Itest
echo "== avr (applications/tc01, atmega2560)"
if command -v avr-gcc > /dev/null; then
    ls kernel/*.c port/avr/*.c hal/*.c hal/avr/*.c | run avr avr-gcc -std=c99 -Wall -Wextra -Os \
        -mmcu=atmega2560 -DF_CPU=16000000UL -Ikernel -Iport/avr -Ihal -Ihal/avr -Iapplications/tc01
else
    echo "avr-gcc not present, skipped"
fi
echo "== esp32 (applications/tc01)"
if [ -x "$XG" ]; then
    ls kernel/*.c port/xtensa/*.c hal/*.c hal/xtensa/*.c | run esp32 "$XG" -std=c99 -Wall -Wextra -Os \
        -mlongcalls -DF_CPU=240000000UL -DHAL_APB_HZ=80000000UL -Ikernel -Iport/xtensa -Ihal -Ihal/xtensa -Iapplications/tc01
else
    echo "xtensa gcc not present, skipped"
fi
