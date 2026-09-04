# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/phaseb
OBJS = $(BUILD_DIR)/phaseb.o
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO

$(shell mkdir -p "$(BUILD_DIR)")

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT_TITLE = Nakagawa Phase-B Money Launch
PSP_EBOOT_ICON = NULL
LIBS = -lpspge -lpsppower -lpspthreadman_kernel_660

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

$(BUILD_DIR)/phaseb.o: phaseb.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: EBOOT.PBP
EBOOT.PBP: $(PSP_EBOOT)
