# tc09: code and constants running from flash through the cache (ESP32 only). The test's
# own object goes to flash as a whole; the kernel stays in IRAM.
BOARD       = esp32
ONLY_BOARDS = esp32
FLASH_OBJS  = *tc09_flash.o
