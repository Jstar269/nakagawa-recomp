# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

FIXTURE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD_DIR ?= build
TARGET = $(BUILD_DIR)/nakagawa_psp_oracle
OBJS = $(BUILD_DIR)/probe.o
PSP_EBOOT = $(BUILD_DIR)/EBOOT.PBP
PSP_EBOOT_SFO = $(BUILD_DIR)/PARAM.SFO

# Build one deterministic probe case at a time.  A physical session should
# rebuild with CASE=callback-notify-check, CASE=wait-cancel, or
# CASE=thread-lifecycle rather than mixing unrelated waits in one launch.  The
# Keep each Tier S/B launch isolated; an unexpected kernel response could
# terminate it before the complete record set is emitted.
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
else ifeq ($(CASE),dma-invalid-tail-s0)
CASE_ID = 60
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
else ifeq ($(CASE),dma-size-matrix-cell)
CASE_ID = 55
else ifeq ($(CASE),mbx-delete-wait)
CASE_ID = 56
else ifeq ($(CASE),ge-nan)
CASE_ID = 57
else ifeq ($(CASE),dma-cells)
CASE_ID = 58
else ifeq ($(CASE),delay-zero)
CASE_ID = 59
else ifeq ($(CASE),kernel-alarm)
CASE_ID = 61
else ifeq ($(CASE),thread-scheduler)
CASE_ID = 62
else ifeq ($(CASE),wait-outcomes)
CASE_ID = 63
else ifeq ($(CASE),ge-break-continue)
CASE_ID = 64
else ifeq ($(CASE),refer-status-size)
CASE_ID = 65
else ifeq ($(CASE),registry-readonly)
CASE_ID = 66
else ifeq ($(CASE),kernel-misc)
CASE_ID = 67
# 90-96: the H-oracle HLE measurement families (probe_hle_measure.c). They are
# numbered apart from the sequential 1-67 probe.c cases so that new sequential
# cases cannot collide with them.
else ifeq ($(CASE),hle-kernel-status)
CASE_ID = 90
else ifeq ($(CASE),hle-vtimer)
CASE_ID = 91
else ifeq ($(CASE),hle-power-clock)
CASE_ID = 92
else ifeq ($(CASE),hle-hprm)
CASE_ID = 93
else ifeq ($(CASE),hle-ctrl-latch)
CASE_ID = 94
else ifeq ($(CASE),hle-sysparam)
CASE_ID = 95
else ifeq ($(CASE),hle-ge-edram)
CASE_ID = 96
else ifeq ($(CASE),llsc-link)
CASE_ID = 69
else
$(error unsupported CASE=$(CASE); see fixtures/psp_oracle/README.md for the supported case names)
endif

CFLAGS = -O2 -G0 -Wall -Wextra -std=c11 -DPSP_ORACLE_CASE=$(CASE_ID)
ifeq ($(CASE),dma-size-matrix-cell)
ifeq ($(strip $(DMAC_SIZE_REQUEST)),)
$(error CASE=dma-size-matrix-cell requires DMAC_SIZE_REQUEST=<supported size>)
endif
endif
ifneq ($(filter dma-size-matrix dma-size-matrix-cell,$(CASE)),)
ifneq ($(strip $(DMAC_SIZE_REQUEST)),)
CFLAGS += -DDMAC_SIZE_REQUEST=$(DMAC_SIZE_REQUEST)
endif
endif

# Cases 23-28 and 31-43 are standalone probes, each in its own source file, that
# record the build commit in their output. The exception probes raise exactly
# one exception and never return, except exception-a3-unaligned,
# exception-a3-vfpu-aligned and exception-a3-vfpu-lvl-odd which are the negative
# controls that return and write a results file; the kernel-object
# probes use their own complete ThreadManForUser import block
# (threadman_user_imports.S). CASE_IDs 29/30 are reserved for open PR #244
# (exception-a3-mload/mstore) so a future merge cannot collide.
PROBE_BUILD_COMMIT ?= $(shell git rev-parse --short HEAD)
# The main probe emits this full object id in source_commit for host comparison.
# Refuse an unknown, overridden, or dirty source tree instead of producing a
# PRX that claims to represent a commit it does not contain.
PROBE_BUILD_COMMIT_FULL ?= $(shell git rev-parse --verify HEAD 2>/dev/null)
PROBE_BUILD_COMMIT_HEAD := $(shell git rev-parse --verify HEAD 2>/dev/null)
PROBE_BUILD_TREE_STATUS := $(shell git status --porcelain --untracked-files=normal 2>/dev/null || echo __GIT_STATUS_UNAVAILABLE__)
ifeq ($(strip $(PROBE_BUILD_COMMIT_FULL)),)
$(error PROBE_BUILD_COMMIT could not be determined; refusing probe build)
endif
ifneq ($(PROBE_BUILD_COMMIT_FULL),$(PROBE_BUILD_COMMIT_HEAD))
$(error PROBE_BUILD_COMMIT must match the clean checkout HEAD)
endif
ifneq ($(filter __GIT_STATUS_UNAVAILABLE__,$(PROBE_BUILD_TREE_STATUS)),)
$(error source tree status could not be determined; refusing probe build)
endif
ifneq ($(strip $(PROBE_BUILD_TREE_STATUS)),)
$(error source tree is dirty; refusing to embed PROBE_BUILD_COMMIT)
endif
$(shell mkdir -p "$(BUILD_DIR)")
CFLAGS += -DPROBE_BUILD_COMMIT=\"$(PROBE_BUILD_COMMIT_FULL)\"
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
# HLE measurement families (hle-*): one PRX per family, all in
# probe_hle_measure.c. Each links the complete ThreadManForUser block (module
# lifecycle, and the ReferSystemStatus / VTimer / FPL NIDs) plus only its own
# library block, so every PRX's import table is exactly its family's.
else ifeq ($(CASE),hle-kernel-status)
TARGET = $(BUILD_DIR)/hle_kernel_status
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-vtimer)
TARGET = $(BUILD_DIR)/hle_vtimer
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-power-clock)
TARGET = $(BUILD_DIR)/hle_power_clock
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/hle_power_imports.o
else ifeq ($(CASE),hle-hprm)
TARGET = $(BUILD_DIR)/hle_hprm
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/hle_hprm_imports.o
else ifeq ($(CASE),hle-ctrl-latch)
TARGET = $(BUILD_DIR)/hle_ctrl_latch
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/hle_ctrl_imports.o
else ifeq ($(CASE),hle-sysparam)
TARGET = $(BUILD_DIR)/hle_sysparam
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/hle_utility_imports.o
else ifeq ($(CASE),hle-ge-edram)
TARGET = $(BUILD_DIR)/hle_ge_edram
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/hle_ge_imports.o
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
ifeq ($(CASE),ge-nan)
LIBS := -lpspgum -lpspgu $(LIBS)
endif
ifeq ($(CASE),model-profile)
LIBS += -lpspkubridge
endif
ifeq ($(CASE),registry-readonly)
LIBS += -lpspreg
endif
ifeq ($(CASE),kernel-misc)
OBJS += $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/display_user_imports.o
LIBS += -lpspctrl
endif
ifeq ($(CASE),wait-outcomes)
OBJS += $(BUILD_DIR)/threadman_user_imports.o
endif

# The main probe exports module_stop (see "Module lifecycle" in probe.c), so it
# replaces the SDK default syslib export table, which names only module_start
# and module_info. The table is generated into the ignored build directory and
# compiled by the SDK's %.c: %.exp and %.o: %.c rules.
ifneq ($(filter $(BUILD_DIR)/probe.o,$(OBJS)),)
PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp
endif
# The HLE measurement probe (probe_hle_measure.c) has the same lifecycle, so it
# exports module_stop from the same generated table.
ifneq ($(filter $(BUILD_DIR)/probe_hle_measure.o,$(OBJS)),)
PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp
endif

PSPSDK = $(shell psp-config --pspsdk-path)
ifeq ($(strip $(PSPSDK)),)
$(error PSPDEV/PSPSDK is not available: psp-config --pspsdk-path failed)
endif
include $(PSPSDK)/lib/build.mak

# Fail closed if psp-fixup-imports reports out-of-order stubs (issue #400),
# then run the user-mode import gate on the fixed-up ELF: a user-mode probe
# that imports a kernel-only library (*_driver, *ForKernel, ...) cannot load
# on the PSP, so the build stops and names the library and its NIDs.
# .DELETE_ON_ERROR removes a rejected ELF so a later make cannot package it.
PYTHON ?= python3
USER_MODE_IMPORT_GATE = $(FIXTURE_DIR)../../tools/psp_oracle/user_mode_imports.py
FIXUP = bash -c 'out=$$(psp-fixup-imports "$$1" 2>&1); rc=$$?; test -n "$$out" && echo "$$out"; echo "$$out" | grep -q "stubs out of order" && exit 1; test $$rc -eq 0 || exit $$rc; exec "$(PYTHON)" "$(USER_MODE_IMPORT_GATE)" "$$1"' --
.DELETE_ON_ERROR:

# The SDK's generic `%.o: %.c` rule expects source and object to share a
# directory. Keep the source at the fixture root while placing all generated
# objects under the ignored build directory.
$(BUILD_DIR)/probe.o: probe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_exports.exp: Makefile
	printf '%s\n' 'PSP_BEGIN_EXPORTS' 'PSP_EXPORT_START(syslib, 0, 0x8000)' \
		'PSP_EXPORT_FUNC_HASH(module_start)' 'PSP_EXPORT_FUNC_HASH(module_stop)' \
		'PSP_EXPORT_VAR_HASH(module_info)' 'PSP_EXPORT_END' 'PSP_END_EXPORTS' > $@

$(BUILD_DIR)/mutex_imports.o: mutex_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/threadman_user_imports.o: threadman_user_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/display_user_imports.o: display_user_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_hle_measure.o: probe_hle_measure.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/hle_power_imports.o: hle_power_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/hle_hprm_imports.o: hle_hprm_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/hle_ctrl_imports.o: hle_ctrl_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/hle_utility_imports.o: hle_utility_imports.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/hle_ge_imports.o: hle_ge_imports.S
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
