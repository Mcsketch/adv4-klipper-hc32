# SoC binaries for the Adventurer 4 (Allwinner V3S, Linux 3.4, no libc worth
# linking against): static ARMv5T soft-float, which the Cortex-A7 runs as is.
#   make            -> ../bin/ad4-bridge ad4-screen ad4-buzz
#   make preview    -> host build of ad4-screen that writes PPM screenshots
# Debian/Ubuntu: apt install gcc-arm-linux-gnueabi
CROSS  ?= arm-linux-gnueabi-
CC      = $(CROSS)gcc
CFLAGS ?= -std=gnu11 -O2 -Wall
OUT    ?= ../bin
PREVIEW ?= ../build/preview

BINS = $(OUT)/ad4-bridge $(OUT)/ad4-screen $(OUT)/ad4-buzz

all: $(BINS)

$(OUT)/%: %.c ad4_buzzer.h ad4_font8x8.h stb_image.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -static -o $@ $<
	$(CROSS)strip $@

preview:
	@mkdir -p $(PREVIEW)
	cc -std=gnu11 -O2 -Wall -DAD4_DUMP -o $(PREVIEW)/ad4-screen-host ad4-screen.c
	cd $(PREVIEW) && export AD4_MCU=NA && ./ad4-screen-host splash --dump splash.ppm; \
	  ./ad4-screen-host boot --dump boot.ppm; \
	  ./ad4-screen-host confirm "Install Klipper?" "Flashes Klipper onto the mainboard and switches the panel to Klipper. Takes a few minutes. Do not power off while it runs.\nMainboard: NA (N32G455)" INSTALL --dump install-confirm.ppm; \
	  ./ad4-screen-host confirm "Restore stock firmware?" "Flashes FlashForge's stock firmware (2.3.0) onto the mainboard and removes Klipper from the printer. Takes a few minutes. Do not power off while it runs." RESTORE --dump rollback-confirm.ppm; \
	  ./ad4-screen-host status "Flashing Klipper" "frame 17 of 42. Do not power off." 40 --dump status.ppm; \
	  ./ad4-screen-host done "Klipper installed" "Rebooting into Klipper mode. Remove the stick." --dump done.ppm; \
	  ./ad4-screen-host fail "HD board" "This printer has the HC32F460 (HD) mainboard. This installer only supports the NA (N32G455) board. Nothing was changed. Remove the stick." --dump fail.ppm; \
	  true
	python3 ../tools/ppm2png.py $(PREVIEW)/*.ppm

clean:
	rm -f $(BINS)

.PHONY: all preview clean
