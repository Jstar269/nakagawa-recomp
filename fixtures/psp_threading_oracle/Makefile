# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/psp_threading_oracle
OBJS = $(BUILD_DIR)/thread_oracle.o $(BUILD_DIR)/thread_shim.o
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO

$(shell mkdir -p "$(BUILD_DIR)")

# Campaign phase selection. One launch per phase is the canonical 5-launch minimum;
# building with CASE=all runs all 28 gating cases in one execution (convenient for
# host-side parser tests). The 5-phase split is the hardware handoff.
CASE ?= all
ifeq ($(CASE),all)
CASE_ID = 100
else ifeq ($(CASE),ct-attr)
CASE_ID = 101
else ifeq ($(CASE),ct-prio-opt)
CASE_ID = 102
else ifeq ($(CASE),st-args)
CASE_ID = 103
else ifeq ($(CASE),st-life-ret)
CASE_ID = 104
else ifeq ($(CASE),st-sched)
CASE_ID = 105
else
$(error unsupported CASE=$(CASE); valid: all ct-attr ct-prio-opt st-args st-life-ret st-sched)
endif

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11 -DPSP_THREADING_CASE=$(CASE_ID)
ifdef PSP_THREADING_FULL
CFLAGS += -DPSP_THREADING_FULL=1
endif
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 500
PSP_EBOOT_TITLE = Nakagawa PSP Threading Oracle
PSP_EBOOT_ICON = NULL
LIBS = -lpsppower

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

$(BUILD_DIR)/thread_oracle.o: thread_oracle.c thread_shim.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/thread_shim.o: thread_shim.S thread_shim.h
	$(CC) $(ASFLAGS) -c $< -o $@

.PHONY: EBOOT.PBP
EBOOT.PBP: $(PSP_EBOOT)
