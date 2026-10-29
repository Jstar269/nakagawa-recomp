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
# The hand-authored mutex imports are needed by mutex probes and cpu-timing's
# synchronization cells. Linking them into unrelated PRXs leaves PSP SDK
# import stubs out of order.
MUTEX_CASES = mutex-refer-unlocked mutex-timeout-quanta \
              mutex-priority-inheritance mutex-interrupt-context cpu-timing
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
else ifeq ($(CASE),vfpu-compare)
CASE_ID = 68
else ifeq ($(CASE),vfpu-transcendental-corpus)
CASE_ID = 71
# 90-103: the H-oracle HLE measurement families (probe_hle_measure.c). They are
# numbered apart from the sequential probe.c cases so that new sequential
# cases cannot collide with them; 97-103 are single-launch NOT_RUN cases.
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
else ifeq ($(CASE),hle-vtimer-unit)
CASE_ID = 97
else ifeq ($(CASE),hle-vtimer-errors)
CASE_ID = 98
else ifeq ($(CASE),hle-kernel-pool-bounds)
CASE_ID = 99
else ifeq ($(CASE),hle-power-float-v0)
CASE_ID = 100
else ifeq ($(CASE),hle-null-out-pointers)
CASE_ID = 101
else ifeq ($(CASE),hle-audio-src)
CASE_ID = 102
else ifeq ($(CASE),hle-timer-order)
CASE_ID = 103
# 106-108: the HLE issue cells (probe_hle_issues.c). 106 is the #356 model-getter and
# #326 vrnd carry / vscmp batch; 107 the #325 interrupt and timer delivery cells; 108 the
# #326 VFPU form words (vh2f, vuc2i, vsocp, vwbn, vrndf1/2).
else ifeq ($(CASE),hle-issues-batch)
CASE_ID = 106
else ifeq ($(CASE),hle-intr-cells)
CASE_ID = 107
else ifeq ($(CASE),vfpu-forms)
CASE_ID = 108
# 109: the GE texture sample-point cells (probe_ge_sample.c, #282): which texel a
# through-mode triangle pair and sprite show, nearest and bilinear.
else ifeq ($(CASE),ge-sample)
CASE_ID = 109
else ifeq ($(CASE),llsc-link)
CASE_ID = 69
else ifeq ($(CASE),fpu-cc)
CASE_ID = 70
else ifeq ($(CASE),registry-font)
CASE_ID = 72
else ifeq ($(CASE),font-files)
CASE_ID = 73
else ifeq ($(CASE),umd-io)
CASE_ID = 74
else ifeq ($(CASE),umd-raw-read)
CASE_ID = 77
else ifeq ($(CASE),font-list)
CASE_ID = 75
else ifeq ($(CASE),vram-mirror)
CASE_ID = 76
else ifeq ($(CASE),late-prx-target)
CASE_ID = 110
else ifeq ($(CASE),at3-decode)
CASE_ID = 111
else ifeq ($(CASE),fake-success)
CASE_ID = 112
else ifeq ($(CASE),font-userpgf)
CASE_ID = 113
else ifeq ($(CASE),ge-timing)
CASE_ID = 114
else ifeq ($(CASE),cpu-timing)
CASE_ID = 115
else ifeq ($(CASE),thread-k0-block)
CASE_ID = 116
else ifeq ($(CASE),sas-init)
CASE_ID = 117
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
else ifeq ($(CASE),hle-vtimer-unit)
TARGET = $(BUILD_DIR)/hle_vtimer_unit
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-vtimer-errors)
TARGET = $(BUILD_DIR)/hle_vtimer_errors
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-kernel-pool-bounds)
TARGET = $(BUILD_DIR)/hle_kernel_pool_bounds
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-power-float-v0)
TARGET = $(BUILD_DIR)/hle_power_float_v0
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o $(BUILD_DIR)/hle_power_imports.o
else ifeq ($(CASE),hle-null-out-pointers)
TARGET = $(BUILD_DIR)/hle_null_out_pointers
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-audio-src)
TARGET = $(BUILD_DIR)/hle_audio_src
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-timer-order)
TARGET = $(BUILD_DIR)/hle_timer_order
OBJS = $(BUILD_DIR)/probe_hle_measure.o $(BUILD_DIR)/threadman_user_imports.o
# HLE issue cells: their own source, which installs and removes interrupt and timer
# handlers (107) and reaches a kernel getter through the kernel bridge (106), so they
# stay out of probe_hle_measure.c. Same ThreadManForUser block and module lifecycle.
else ifeq ($(CASE),hle-issues-batch)
TARGET = $(BUILD_DIR)/hle_issues_batch
OBJS = $(BUILD_DIR)/probe_hle_issues.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),hle-intr-cells)
TARGET = $(BUILD_DIR)/hle_intr_cells
OBJS = $(BUILD_DIR)/probe_hle_issues.o $(BUILD_DIR)/threadman_user_imports.o
else ifeq ($(CASE),vfpu-forms)
TARGET = $(BUILD_DIR)/vfpu_forms
OBJS = $(BUILD_DIR)/probe_hle_issues.o $(BUILD_DIR)/threadman_user_imports.o
# The GE sample-point cells: their own source (raw display lists, no libpspgu) with the
# same writer, ThreadManForUser block and module lifecycle as the HLE issue cells.
else ifeq ($(CASE),ge-sample)
TARGET = $(BUILD_DIR)/ge_sample
OBJS = $(BUILD_DIR)/probe_ge_sample.o $(BUILD_DIR)/threadman_user_imports.o
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
ifneq ($(filter ge-nan vram-mirror ge-timing cpu-timing,$(CASE)),)
LIBS := -lpspgum -lpspgu $(LIBS)
endif
# #1096: the AT3 decode probe links the user-mode ATRAC and utility stubs it calls.
ifeq ($(CASE),at3-decode)
LIBS += -lpspatrac3 -lpsputility
endif
# #1595: the SAS re-init probe links the user-mode sceSasCore and utility stubs it calls.
ifeq ($(CASE),sas-init)
LIBS += -lpspsascore -lpsputility
endif
# Fake-success audit: the OSK calls live in libpsputility; the volatile-memory calls are in SuspendForUser.
ifeq ($(CASE),fake-success)
LIBS += -lpsputility
# FAKE_SUCCESS_OSK_CYCLE=1 adds the attended on-screen-keyboard cycle (a person must finish the dialog).
ifeq ($(FAKE_SUCCESS_OSK_CYCLE),1)
CFLAGS += -DFAKE_SUCCESS_OSK_CYCLE
endif
endif
# #1049 caveat: an extra depth-buffer offset for CASE=vram-mirror (default 0 = the measured zbp 0x0cc000).
VRAM_MIRROR_DEPTH_BIAS ?= 0x00000000
ifeq ($(CASE),vram-mirror)
CFLAGS += -DVRAM_MIRROR_DEPTH_BIAS=$(VRAM_MIRROR_DEPTH_BIAS)
ifeq ($(VRAM_MIRROR_FULL_SCAN),1)
CFLAGS += -DVRAM_MIRROR_FULL_SCAN
endif
endif
ifeq ($(CASE),model-profile)
LIBS += -lpspkubridge
endif
# hle-issues-batch: kuKernelGetModel, the hand-written sceKernelGetModel user import, and the
# kernel getter through ARK's sctrlHENFindFunction + kuKernelCall.
ifeq ($(CASE),hle-issues-batch)
LIBS += -lpspkubridge -lpspsystemctrl_user
# Exactly kuKernelGetModel + kuKernelCall, sctrlHENFindFunction, and SysMemUserForUser's
# sceKernelGetModel (the hand stub) and sceKernelDevkitVersion plus the four memory calls
# the PSPSDK libc glue imports; no interrupt-manager import at all.
USER_MODE_IMPORT_GATE_ARGS += --exact-library KUBridge=0x24331850,0x9060F69D
USER_MODE_IMPORT_GATE_ARGS += --exact-library SystemCtrlForUser=0x159AF5CC
USER_MODE_IMPORT_GATE_ARGS += --exact-library SysMemUserForUser=0x6373995D,0x3FC9AE6A,0xA291F107,0x237DBD4F,0xB6D61D02,0x9D9A5BA1
USER_MODE_IMPORT_GATE_ARGS += --forbid-library InterruptManager
endif
# hle-intr-cells: exactly the four VBLANK sub-interrupt calls, and from Kernel_Library the
# suspend/resume pair plus the two LwMutex calls the PSPSDK libc glue imports; nothing from
# the kernel bridge.
ifeq ($(CASE),hle-intr-cells)
USER_MODE_IMPORT_GATE_ARGS += --exact-library InterruptManager=0xCA04A2B9,0xD61E6961,0xFB8E22EC,0x8A389411
USER_MODE_IMPORT_GATE_ARGS += --exact-library Kernel_Library=0x092968F4,0x5F10D406,0xBEA46419,0x15B6446B
USER_MODE_IMPORT_GATE_ARGS += --forbid-library KUBridge --forbid-library SystemCtrlForUser
endif
ifeq ($(CASE),cpu-timing)
# The shared probe cleanup checks interrupt state; cpu-timing also calls the
# lightweight-mutex lock/unlock pair through Kernel_Library.
USER_MODE_IMPORT_GATE_ARGS += --exact-library Kernel_Library=0x092968F4,0x5F10D406,0x47A0B729,0xB55249D2,0xBEA46419,0x15B6446B
endif
# The registry cases may import only the eight read-only sceReg calls, exactly: the gate
# below fails the build on any other sceReg NID (a set, create, remove or flush call).
REGISTRY_CASES = registry-readonly registry-font
ifneq ($(filter $(REGISTRY_CASES),$(CASE)),)
LIBS += -lpspreg
USER_MODE_IMPORT_GATE_ARGS += --exact-library sceReg=0x92E41280,0xFA8A5739,0x1D8A762E,0x0CAE832B,0x2C0DB9DD,0x2D211135,0xD4475AA8,0x28A8E98A
endif
# The UMD case reads the drive state only: exactly sceUmdCheckMedium and sceUmdGetDriveStat,
# so a build that imported sceUmdActivate (or any other UMD call) fails the gate.
ifeq ($(CASE),umd-io)
LIBS += -lpspumd
USER_MODE_IMPORT_GATE_ARGS += --exact-library sceUmdUser=0x46EBB729,0x6B4A146C
endif
# Raw UMD reads are read-only on the UMD descriptor. The IoFileMgrForUser pin covers the case's
# seek/read/async/stat calls, the durable host0 logger and round-trip cleanup, and the four
# IoFileMgr stubs pulled by PSPSDK libc glue. The source test forbids write/control calls in the
# raw UMD case itself, and the emulator devctl/stdout route is compiled out for this PSP-only case.
ifeq ($(CASE),umd-raw-read)
LIBS += -lpspumd
USER_MODE_IMPORT_GATE_ARGS += --exact-library sceUmdUser=0x46EBB729,0x6B4A146C
USER_MODE_IMPORT_GATE_ARGS += --exact-library IoFileMgrForUser=0x109F50BC,0x27EB27B8,0x3251EA56,0x42EC03AC,0x55F4717D,0x68963324,0x6A638D83,0x810C4BC3,0xA0B5A7C2,0xACE946E8,0xB29DDF9C,0xE23EEC33,0xE3EB004C,0xEB092469,0xF27A9C51
endif
# The font-list case asks the console's own font library for the system font list. It links
# the hand-authored sceLibFont stubs (libfont_imports.S) and may import exactly those five
# calls; it may import nothing from sceReg, so the probe itself can neither open nor create a
# registry category (the only registry reads are the library's own).
ifeq ($(CASE),font-list)
OBJS += $(BUILD_DIR)/libfont_imports.o
USER_MODE_IMPORT_GATE_ARGS += --exact-library sceLibFont=0x67F17ED7,0x574B6FBC,0x27F6E642,0xBC75D85B,0x5333322D --forbid-library sceReg
endif
# The font-userpgf case (#1055) opens project and console PGFs in the resident font library and
# draws glyphs. libfont_imports.S gains its eight extra imports only under -DLIBFONT_USERPGF, so
# the font-list pin above is unchanged; this case is pinned to exactly the thirteen NIDs it calls.
ifeq ($(CASE),font-userpgf)
OBJS += $(BUILD_DIR)/libfont_imports.o
LIBFONT_ASFLAGS = -DLIBFONT_USERPGF
USER_MODE_IMPORT_GATE_ARGS += --exact-library sceLibFont=0x67F17ED7,0x574B6FBC,0x27F6E642,0xBC75D85B,0x5333322D,0xA834319D,0x57FCB733,0xBB8E7FE6,0x3AEA8CB6,0x0DA7535E,0xDCC80C2F,0x980F4895,0xAA3DE7B5 --forbid-library sceReg
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
# The HLE issue cells (probe_hle_issues.c) repeat that lifecycle.
ifneq ($(filter $(BUILD_DIR)/probe_hle_issues.o,$(OBJS)),)
PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp
endif
# So do the GE sample-point cells (probe_ge_sample.c).
ifneq ($(filter $(BUILD_DIR)/probe_ge_sample.o,$(OBJS)),)
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
FIXUP = bash -c 'out=$$(psp-fixup-imports "$$1" 2>&1); rc=$$?; test -n "$$out" && echo "$$out"; echo "$$out" | grep -q "stubs out of order" && exit 1; test $$rc -eq 0 || exit $$rc; exec "$(PYTHON)" "$(USER_MODE_IMPORT_GATE)" $(USER_MODE_IMPORT_GATE_ARGS) "$$1"' --
.DELETE_ON_ERROR:

# Make judges staleness by file times alone, but the case selects the flags every
# object is compiled with (PSP_ORACLE_CASE, the embedded build commit, A3_CASE,
# DMAC_SIZE_REQUEST), the libraries the ELF links, and the PARAM.SFO memory size.
# Without a dependency on that selection, `make CASE=a` then `make CASE=b` linked
# case a's objects into case b's PRX, or failed to link when case b dropped a
# library case a's objects import. The stamp holds the selection and the FORCE
# rule rewrites it only when its content changes, so a new CASE (or commit)
# rebuilds every linked object and PARAM.SFO while an unchanged rebuild stays a
# no-op; the ELF and PRX follow from the objects. The stamp is not an ELF
# prerequisite, so the SDK link rule's $^ still names only objects.
BUILD_CONFIG = CASE=$(CASE) CFLAGS=$(CFLAGS) LIBS=$(LIBS) SFOFLAGS=$(SFOFLAGS)
BUILD_CONFIG_STAMP = $(BUILD_DIR)/build-config.stamp
EXTRA_CLEAN += $(BUILD_CONFIG_STAMP)
$(OBJS) $(PSP_EBOOT_SFO): $(BUILD_CONFIG_STAMP)
$(BUILD_CONFIG_STAMP): FORCE
	@printf '%s\n' '$(subst ','\'',$(BUILD_CONFIG))' > $@.tmp; \
	if cmp -s $@.tmp $@; then rm -f $@.tmp; \
	else echo "build configuration changed (CASE=$(CASE)): rebuilding objects"; mv -f $@.tmp $@; fi
.PHONY: FORCE
FORCE:

# The SDK's generic `%.o: %.c` rule expects source and object to share a
# directory. Keep the source at the fixture root while placing all generated
# objects under the ignored build directory.
$(BUILD_DIR)/probe.o: probe.c registry_walk.h font_files.h umd_io.h font_list.h
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

$(BUILD_DIR)/libfont_imports.o: libfont_imports.S
	$(CC) $(ASFLAGS) $(LIBFONT_ASFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_hle_measure.o: probe_hle_measure.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_hle_issues.o: probe_hle_issues.c hle_vfpu_cells.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/probe_ge_sample.o: probe_ge_sample.c
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
