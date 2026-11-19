# The SMP kernel on both cores of the ESP32 (nornInternal/nornCore/smp_plan.md step 7). Runs on the board
# and in Espressif's QEMU (two emulated cores; its cross core latencies are the emulator's,
# hundreds of microseconds, so TC_TOLERANCE_PERMILLE widens that limit there).
BOARD       = esp32
ONLY_BOARDS = esp32
