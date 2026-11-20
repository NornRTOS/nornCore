# Deep sleep with RTC timer wake-up on the ESP32 (hal/power.h). Board only: QEMU does not
# emulate the RTC sleep controller.
BOARD       = esp32
ONLY_BOARDS = esp32
NO_QEMU     = 1
