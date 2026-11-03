# stm32isp-ad4 — Flashforge Adventurer 4 MCU IAP tool (SoC side)
#
# Build ON the SoC (armv7l Linux, root):
#   make            # native build on the SoC (simplest, recommended)
#
# Cross-compile from a dev box to the SoC (armv7l, hard-float VFPv3):
#   make CROSS=arm-linux-gnueabihf-
#
# The result is a single static-friendly static binary with no non-stdlib deps,
# so it can be scp'd straight to the SoC.

CC      ?= gcc
CROSS   ?=
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
TARGET  = stm32isp-ad4
SRC     = stm32isp-ad4.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRC)
	$(CROSS)$(CC) $(CFLAGS) -static -o $@ $<

clean:
	rm -f $(TARGET)
