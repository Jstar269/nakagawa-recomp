# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/nakagawa_psp_oracle
OBJS = $(BUILD_DIR)/probe.o $(BUILD_DIR)/mutex_imports.o
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO

$(shell mkdir -p "$(BUILD_DIR)")

# Build one deterministic probe case at a time.  A physical session should
# rebuild with CASE=callback-notify-check, CASE=wait-cancel, or
# CASE=thread-lifecycle rather than mixing unrelated waits in one launch.  The
# DMAC invalid-tail matrix is intentionally one case per launch because an
# unexpected firmware response could terminate the probe before it emits a
# record.
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
else ifeq ($(CASE),dma-concurrency)
CASE_ID = 8
else ifeq ($(CASE),dma-invalid-tail-memcpy-dst)
CASE_ID = 9
else ifeq ($(CASE),dma-invalid-tail-memcpy-src)
CASE_ID = 10
else ifeq ($(CASE),dma-invalid-tail-try-dst)
CASE_ID = 11
else ifeq ($(CASE),dma-invalid-tail-try-src)
CASE_ID = 12
else ifeq ($(CASE),mutex-refer-unlocked)
CASE_ID = 13
else ifeq ($(CASE),mutex-timeout-quanta)
CASE_ID = 14
else ifeq ($(CASE),mutex-priority-inheritance)
CASE_ID = 15
else ifeq ($(CASE),mutex-interrupt-context)
CASE_ID = 16
else
$(error unsupported CASE=$(CASE); see fixtures/psp_oracle/README.md for the supported case names)
endif

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11 -DPSP_ORACLE_CASE=$(CASE_ID)
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 500
# Keep only the invalid-tail variants at the public 24 MiB baseline. They
# reserve the final valid prefix with the system-memory allocator and prove
# that an allocation beginning at the next address is rejected before issuing
# the syscall. Existing probes and the concurrency case retain their prior
# memory request.
ifneq ($(filter dma-invalid-tail-%,$(CASE)),)
PSP_LARGE_MEMORY = 0
endif
PSP_EBOOT_TITLE = Nakagawa PSP Oracle
PSP_EBOOT_ICON = NULL
LIBS = -lpspdmac
# Case 16 (mutex-interrupt-context) uses sceKernelIsIntrContext() from the
# kernel interrupt manager library for independent ISR execution proof.
ifeq ($(CASE),mutex-interrupt-context)
LIBS += -lpspinterruptmanager_kernel_660
endif

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

$(BUILD_DIR)/mutex_imports.o: mutex_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

# Keep the documented `make EBOOT.PBP` invocation while storing generated files
# under the ignored build directory.
.PHONY: EBOOT.PBP
EBOOT.PBP: $(PSP_EBOOT)
