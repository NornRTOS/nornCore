# Xtensa LX6 architecture (ESP32). Toolchain: Espressif xtensa-esp-elf from crosstool-NG,
# image and flashing with esptool. No ESP-IDF.
#
#   make APP=hello BOARD=esp32 upload PORT=/dev/ttyUSB0
#   make APP=hello BOARD=esp32 monitor

ifeq ($(BOARD),esp32)
  # CPU_MHZ: 40 (crystal, as the ROM leaves it) or 80, 160, 240 from the PLL
  # (port_clock.c). The APB bus runs at 80 MHz whenever the PLL is used. 240 is the
  # default since 2026-11-03 (suite and nornWIF verified there); 160 and 80 work too.
  CPU_MHZ ?= 240
  F_CPU   = $(CPU_MHZ)000000UL
  ifeq ($(CPU_MHZ),40)
    APB_HZ = 40000000UL
  else
    APB_HZ = 80000000UL
  endif
  CHIP    = esp32
else
  $(error port/xtensa/arch.mk does not know BOARD '$(BOARD)')
endif

PORT        ?= /dev/ttyUSB0
XTENSA_ROOT ?= $(HOME)/dev/tools/xtensa-esp-elf-16.1.0
ESP_PYTHON  ?= $(HOME)/dev/tools/esp-venv/bin/python
ESPTOOL     ?= $(ESP_PYTHON) -m esptool

# The toolchain serves several Xtensa cores. The per chip wrapper (xtensa-esp32-elf-gcc)
# loads the core configuration plug-in and selects the matching libgcc multilib.
CC      = $(XTENSA_ROOT)/bin/xtensa-$(CHIP)-elf-gcc
SIZE    = $(XTENSA_ROOT)/bin/xtensa-$(CHIP)-elf-size
OBJCOPY = $(XTENSA_ROOT)/bin/xtensa-$(CHIP)-elf-objcopy
AR      = $(XTENSA_ROOT)/bin/xtensa-$(CHIP)-elf-ar
LDS     = $(BUILD)/esp32.ld
CORE    =

# Code that runs from flash (esp32.ld): the objects and archives in FLASH_OBJS are stored
# in $(TARGET).flash.bin at FLASH_BASE and mapped through the cache before main. The
# first 64 KB pages of the flash hold the RAM image, so FLASH_BASE leaves room for it.
FLASH_BASE ?= 0x80000

CFLAGS  = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wundef -Wstrict-prototypes \
          -Os -g $(CORE) -mlongcalls -DF_CPU=$(F_CPU) -DHAL_APB_HZ=$(APB_HZ) $(INC) \
          -ffunction-sections -fdata-sections -fno-common -MMD -MP
ASFLAGS = $(CORE) -mlongcalls -DF_CPU=$(F_CPU) -DHAL_APB_HZ=$(APB_HZ) $(INC) -x assembler-with-cpp -g -MMD -MP
LDFLAGS = $(CORE) -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-T,$(LDS) -Wl,-Map,$(TARGET).map $(MOD_LDFLAGS)
LDLIBS  = -lc -lgcc

# Memory layout: the whole chip for one core, or a half per core with AMP. An
# application that runs a second image on the APP CPU sets AMP_CORE1 = <subdir> in its
# app.mk (a directory under the application with its own sources and rtos_config.h);
# that image is built first with the core 1 layout and embedded in the core 0 image.
ifdef AMP_CORE1
  MEMLD = mem_core0.ld
else ifeq ($(AMP_CORE),1)
  MEMLD = mem_core1.ld
else
  MEMLD = mem_single.ld
endif

# The linker script: the memory layout, the flash windows, the section rules and the ROM
# symbols, plus the generated lists of what the modules put in flash, IRAM and DRAM.
# Each entry of FLASH_OBJS is a file name pattern of an object (*tc09_flash.o) or of an
# archive (*libnet80211.a, every member); the *_SECTIONS lists name input sections.
$(LDS): port/xtensa/$(MEMLD) port/xtensa/mem_flash.ld port/xtensa/esp32.ld port/xtensa/esp32_rom.ld $(MOD_LD) $(MAKEFILE_LIST)
	@mkdir -p $(BUILD)
	cat port/xtensa/$(MEMLD) > $@
	sed 's/@FLASH_BASE@/$(FLASH_BASE)/g' port/xtensa/mem_flash.ld >> $@
	cat port/xtensa/esp32.ld port/xtensa/esp32_rom.ld $(MOD_LD) | sed "s|@LD_DIR@|$(abspath $(BUILD))|g" >> $@
	@: > $(BUILD)/ld_flash_text.ld; for p in $(FLASH_OBJS); do case $$p in *.a) p="$$p:";; esac; echo "$$p(.literal .literal.* .text .text.*)" >> $(BUILD)/ld_flash_text.ld; done
	@for p in $(FLASH_SECTIONS); do echo "*($$p)" >> $(BUILD)/ld_flash_text.ld; done
	@: > $(BUILD)/ld_flash_rodata.ld; for p in $(FLASH_OBJS); do case $$p in *.a) p="$$p:";; esac; echo "$$p(.rodata .rodata.* .srodata .srodata.*)" >> $(BUILD)/ld_flash_rodata.ld; done
	@for p in $(FLASH_RODATA_SECTIONS); do echo "*($$p)" >> $(BUILD)/ld_flash_rodata.ld; done
	@: > $(BUILD)/ld_iram.ld; for p in $(IRAM_SECTIONS); do echo "*($$p)" >> $(BUILD)/ld_iram.ld; done
	@: > $(BUILD)/ld_dram.ld; for p in $(DRAM_SECTIONS); do echo "*($$p)" >> $(BUILD)/ld_dram.ld; done

ifdef AMP_CORE1
CORE1_BUILD  = build/$(APP)-core1-$(BOARD)$(if $(EXTRA_CFLAGS),-extra)
CORE1_BIN    = $(CORE1_BUILD)/nornos_$(APP)-core1.bin
OBJ += $(BUILD)/amp_core1_image.o

.PHONY: core1-image
core1-image:
	$(MAKE) --no-print-directory APP=$(APP)-core1 APPDIR=$(APPDIR)/$(AMP_CORE1) BOARD=$(BOARD) \
	        AMP_CORE=1 EXTRA_CFLAGS="$(EXTRA_CFLAGS)" image

$(CORE1_BIN): core1-image

$(BUILD)/amp_core1_image.c: $(CORE1_BIN)
	@mkdir -p $(BUILD)
	$(ESP_PYTHON) tools/bin2c.py $< $@ amp_core1_image

$(BUILD)/amp_core1_image.o: $(BUILD)/amp_core1_image.c
	$(CC) $(CFLAGS) -c -o $@ $<
endif

image: $(TARGET).bin $(TARGET).flash.bin
	$(SIZE) $(TARGET).elf
	@ls -l $(TARGET).flash.bin | awk '{print "flash image: " $$5 " bytes at $(FLASH_BASE)"}'

# The RAM image for the ROM loader is made from the ELF without the flash sections, which
# the loader could not place; those go to their own image by load address.
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -R .flash.rodata -R .flash.text $< $(TARGET).ram.elf
	$(ESPTOOL) --chip $(CHIP) elf2image -fm dio -ff 40m -fs 4MB -o $@ $(TARGET).ram.elf

$(TARGET).flash.bin: $(TARGET).elf
	$(OBJCOPY) -O binary -j .flash.rodata -j .flash.text $< $@

$(TARGET).elf: $(LDS)

# Espressif's QEMU (github.com/espressif/qemu releases, qemu-xtensa-softmmu) under
# QEMU_ESP. Its timers drift against the CPU clock, so test builds for it take
# EXTRA_CFLAGS=-DTC_TOLERANCE_PERMILLE=30 (tools/test_all.sh does).
QEMU_ESP    ?= $(HOME)/dev/tools/qemu-esp
sim: $(TARGET).bin $(TARGET).flash.bin
	QEMU_ESP=$(QEMU_ESP) $(ESP_PYTHON) tools/qemu_run.py $< $(or $(SIM_SECONDS),30) $(TARGET).flash.bin@$(FLASH_BASE)

upload: $(TARGET).bin $(TARGET).flash.bin
	$(ESPTOOL) --chip $(CHIP) --port $(PORT) --baud 460800 write-flash 0x1000 $< $(FLASH_BASE) $(TARGET).flash.bin

monitor:
	$(ESP_PYTHON) tools/monitor.py $(PORT) 115200
