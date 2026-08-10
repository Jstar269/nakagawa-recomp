# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/nakagawa_psp_oracle
OBJS = $(BUILD_DIR)/probe.o
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO

$(shell mkdir -p "$(BUILD_DIR)")

# Build one deterministic probe case at a time.  A physical session should
# rebuild with CASE=callback-notify-check, CASE=wait-cancel, or
# CASE=thread-lifecycle rather than mixing unrelated waits in one launch.
CASE ?= smoke
ifeq ($(CASE),smoke)
CASE_ID = 0
else ifeq ($(CASE),callback-notify-check)
CASE_ID = 1
else ifeq ($(CASE),wait-cancel)
CASE_ID = 2
else ifeq ($(CASE),thread-lifecycle)
CASE_ID = 3
else ifeq ($(CASE),thread-delete-lifecycle)
CASE_ID = 4
else ifeq ($(CASE),thread-delete-followup)
CASE_ID = 5
else ifeq ($(CASE),thread-delete-explicit)
CASE_ID = 6
else ifeq ($(CASE),thread-delete-boundary)
CASE_ID = 7
else
$(error unsupported CASE=$(CASE); choose smoke, callback-notify-check, wait-cancel, thread-lifecycle, thread-delete-lifecycle, thread-delete-followup, thread-delete-explicit, or thread-delete-boundary)
endif

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11 -DPSP_ORACLE_CASE=$(CASE_ID)
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT_TITLE = Nakagawa PSP Oracle Smoke
PSP_EBOOT_ICON = NULL

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

# The SDK's generic `%.o: %.c` rule expects source and object to share a
# directory. Keep the source at the fixture root while placing all generated
# objects under the ignored build directory.
$(BUILD_DIR)/probe.o: probe.c
	$(CC) $(CFLAGS) -c $< -o $@

# Keep the documented `make EBOOT.PBP` invocation while storing generated files
# under the ignored build directory.
.PHONY: EBOOT.PBP
EBOOT.PBP: $(PSP_EBOOT)
