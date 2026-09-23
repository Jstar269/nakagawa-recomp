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
# CASE=thread-lifecycle rather than mixing unrelated waits in one launch.  The
# DMAC invalid-tail matrix is intentionally one case per launch because an
# unexpected firmware response could terminate the probe before it emits a
# record.
CASE ?= smoke
# The hand-authored mutex imports are needed only by the mutex probes. Linking
# them into unrelated PRXs leaves PSP SDK import stubs out of order.
MUTEX_CASES = mutex-refer-unlocked mutex-timeout-quanta \
              mutex-priority-inheritance mutex-interrupt-context
ifneq ($(filter $(MUTEX_CASES),$(CASE)),)
OBJS += $(BUILD_DIR)/mutex_imports.o
endif

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
else ifeq ($(CASE),display-mask-vcount)
CASE_ID = 13
else ifeq ($(CASE),display-mask-duty)
CASE_ID = 14
else ifeq ($(CASE),display-ge-mask)
CASE_ID = 15
else ifeq ($(CASE),display-wait-late)
CASE_ID = 16
else ifeq ($(CASE),display-wait-priority)
CASE_ID = 17
else ifeq ($(CASE),display-vblank-window)
CASE_ID = 18
else ifeq ($(CASE),mutex-refer-unlocked)
CASE_ID = 19
else ifeq ($(CASE),mutex-timeout-quanta)
CASE_ID = 20
else ifeq ($(CASE),mutex-priority-inheritance)
CASE_ID = 21
else ifeq ($(CASE),mutex-interrupt-context)
CASE_ID = 22
else ifeq ($(CASE),exception-a1)
CASE_ID = 23
else ifeq ($(CASE),exception-a2)
CASE_ID = 24
else ifeq ($(CASE),exception-a3)
CASE_ID = 25
else ifeq ($(CASE),exception-a3-mload)
CASE_ID = 29
else ifeq ($(CASE),exception-a3-mstore)
CASE_ID = 30
else ifeq ($(CASE),exception-a3-delayslot)
CASE_ID = 31
else ifeq ($(CASE),exception-a3-half-load)
CASE_ID = 32
else ifeq ($(CASE),exception-a3-half-store)
CASE_ID = 33
else ifeq ($(CASE),exception-a3-unaligned)
CASE_ID = 34
else ifeq ($(CASE),exception-a3-vfpu-lvs)
CASE_ID = 35
else ifeq ($(CASE),exception-a3-vfpu-lvq4)
CASE_ID = 36
else ifeq ($(CASE),exception-a3-vfpu-lvq8)
CASE_ID = 37
else ifeq ($(CASE),exception-a3-vfpu-svq4)
CASE_ID = 38
else ifeq ($(CASE),exception-a3-vfpu-aligned)
CASE_ID = 39
else ifeq ($(CASE),exception-a3-vfpu-svs)
CASE_ID = 40
else ifeq ($(CASE),exception-a3-vfpu-svq8)
CASE_ID = 41
else ifeq ($(CASE),exception-a3-vfpu-lvq-delay)
CASE_ID = 42
else ifeq ($(CASE),exception-a3-vfpu-lvl-odd)
CASE_ID = 43
else ifeq ($(CASE),kobj-b1)
CASE_ID = 26
else ifeq ($(CASE),wait-b2)
CASE_ID = 27
else ifeq ($(CASE),kernel-b3)
CASE_ID = 28
else ifeq ($(CASE),transport-write)
CASE_ID = 44
else ifeq ($(CASE),thread-exit-delete)
CASE_ID = 45
else ifeq ($(CASE),dmac-survey)
CASE_ID = 46
else ifeq ($(CASE),ctrl-clock)
CASE_ID = 47
else ifeq ($(CASE),fpu-vector)
CASE_ID = 48
else ifeq ($(CASE),teardown-test)
CASE_ID = 49
else ifeq ($(CASE),io-matrix)
CASE_ID = 50
else ifeq ($(CASE),audio-query)
CASE_ID = 51
else ifeq ($(CASE),cache-alias)
CASE_ID = 52
else ifeq ($(CASE),dma-size-matrix)
CASE_ID = 53
else ifeq ($(CASE),model-profile)
CASE_ID = 54
else
$(error unsupported CASE=$(CASE); see fixtures/psp_oracle/README.md for the supported case names)
endif

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11 -DPSP_ORACLE_CASE=$(CASE_ID)

# Cases 23-28 and 31-43 are standalone probes, each in its own source file, that
# record the build commit in their output. The exception probes raise exactly
# one exception and never return, except exception-a3-unaligned,
# exception-a3-vfpu-aligned and exception-a3-vfpu-lvl-odd which are the negative
# controls that return and write a results file; the kernel-object
# probes use their own complete ThreadManForUser import block
# (threadman_user_imports.S). CASE_IDs 29/30 are reserved for open PR #244
# (exception-a3-mload/mstore) so a future merge cannot collide.
PROBE_BUILD_COMMIT ?= $(shell git rev-parse --short HEAD)
ifeq ($(CASE),exception-a1)
TARGET = $(BUILD_DIR)/psp_a1
OBJS = $(BUILD_DIR)/probe_exception_a1.o
CFLAGS += -DA1_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\"
else ifeq ($(CASE),exception-a2)
TARGET = $(BUILD_DIR)/psp_a2
OBJS = $(BUILD_DIR)/probe_exception_a2.o
CFLAGS += -DA2_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\"
else ifeq ($(CASE),exception-a3)
TARGET = $(BUILD_DIR)/psp_a3
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\"
else ifeq ($(CASE),exception-a3-mload)
TARGET = $(BUILD_DIR)/psp_a3_mload
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=2
else ifeq ($(CASE),exception-a3-mstore)
TARGET = $(BUILD_DIR)/psp_a3_mstore
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=3
else ifeq ($(CASE),exception-a3-delayslot)
TARGET = $(BUILD_DIR)/psp_a3_delayslot
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=4
else ifeq ($(CASE),exception-a3-half-load)
TARGET = $(BUILD_DIR)/psp_a3_half_load
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=5
else ifeq ($(CASE),exception-a3-half-store)
TARGET = $(BUILD_DIR)/psp_a3_half_store
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=6
else ifeq ($(CASE),exception-a3-unaligned)
TARGET = $(BUILD_DIR)/psp_a3_unaligned
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=7
else ifeq ($(CASE),exception-a3-vfpu-lvs)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_lvs
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=8
else ifeq ($(CASE),exception-a3-vfpu-lvq4)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_lvq4
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=9
else ifeq ($(CASE),exception-a3-vfpu-lvq8)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_lvq8
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=10
else ifeq ($(CASE),exception-a3-vfpu-svq4)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_svq4
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=11
else ifeq ($(CASE),exception-a3-vfpu-aligned)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_aligned
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=12
else ifeq ($(CASE),exception-a3-vfpu-svs)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_svs
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=13
else ifeq ($(CASE),exception-a3-vfpu-svq8)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_svq8
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=14
else ifeq ($(CASE),exception-a3-vfpu-lvq-delay)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_lvq_delay
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=15
else ifeq ($(CASE),exception-a3-vfpu-lvl-odd)
TARGET = $(BUILD_DIR)/psp_a3_vfpu_lvl_odd
OBJS = $(BUILD_DIR)/probe_exception_a3.o
CFLAGS += -DA3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\" -DA3_CASE=16
else ifeq ($(CASE),kobj-b1)
TARGET = $(BUILD_DIR)/psp_b1
OBJS = $(BUILD_DIR)/psp_b1.o $(BUILD_DIR)/threadman_user_imports.o
CFLAGS += -DB1_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\"
else ifeq ($(CASE),wait-b2)
TARGET = $(BUILD_DIR)/psp_b2
OBJS = $(BUILD_DIR)/psp_b2.o $(BUILD_DIR)/threadman_user_imports.o
CFLAGS += -DB2_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\"
else ifeq ($(CASE),kernel-b3)
TARGET = $(BUILD_DIR)/psp_b3
OBJS = $(BUILD_DIR)/psp_b3.o $(BUILD_DIR)/threadman_user_imports.o
CFLAGS += -DB3_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT)\"
endif
CXXFLAGS = $(CFLAGS)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_FW_VERSION = 500
# Keep only the invalid-tail variants at the bounded public memory baseline.
# They discover and reserve a page-aligned high block at runtime, then prove
# that an allocation beginning at the next address is rejected before issuing
# the syscall. Existing probes and the concurrency case retain their prior
# memory request.
ifneq ($(filter dma-invalid-tail-%,$(CASE)),)
PSP_LARGE_MEMORY = 0
endif
PSP_EBOOT_TITLE = Nakagawa PSP Oracle
PSP_EBOOT_ICON = NULL
LIBS = -lpspdmac -lpsppower -lpspge -lpspaudio
ifeq ($(CASE),model-profile)
LIBS += -lpspkubridge
endif
# Case 22 (mutex-interrupt-context) uses sceKernelIsIntrContext() from the
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

$(BUILD_DIR)/threadman_user_imports.o: threadman_user_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_exception_a1.o: probe_exception_a1.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_exception_a2.o: probe_exception_a2.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_exception_a3.o: probe_exception_a3.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/psp_b3.o: psp_b3.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/psp_b2.o: psp_b2.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/psp_b1.o: psp_b1.c
	$(CC) $(CFLAGS) -c $< -o $@

# Keep the documented `make EBOOT.PBP` invocation while storing generated files
# under the ignored build directory.
.PHONY: EBOOT.PBP
EBOOT.PBP: $(PSP_EBOOT)
