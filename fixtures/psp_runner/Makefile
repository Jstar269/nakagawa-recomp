# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/nakagawa_runner
OBJS = $(BUILD_DIR)/runner.o

$(shell mkdir -p "$(BUILD_DIR)")

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT_TITLE = Nakagawa PSP Runner
PSP_EBOOT_ICON = NULL

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

$(BUILD_DIR)/runner.o: runner.c
	$(CC) $(CFLAGS) -c $< -o $@
