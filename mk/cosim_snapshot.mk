# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: the AOT/interpreter cosimulation and snapshot execution gates.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# ---- AOT <-> interpreter cosimulation gate ---------------------------------------
#
# One pipeline run over a source-owned synthetic guest produces BOTH execution
# lanes for the same guest bytes: real codegen output, and the production
# interpreter floor over the loaded image. The harness runs each cell twice and
# compares the canonical instruction trace, the ordered guest writes, the guest
# memory window and the full architectural state vector.
#
# TRACE=1 is not optional here. The release preprocessor removes sr_begin/sr_end
# from the generated chunks, and those records are how a divergence is localized
# to one guest instruction -- generated code does not maintain an architectural
# PC in CpuState, so the trace is the only per-instruction attribution the AOT
# lane has.
COSIM_DIR        := $(BUILD_ROOT)/cosim
COSIM_FIXTURE    := $(COSIM_DIR)/fixture
COSIM_TRACES     := $(COSIM_FIXTURE)/traces
COSIM_GENERATOR  := fixtures/cosim/generate.py
COSIM_HARNESS    := fixtures/cosim/cosim_selftest.c
COSIM_BASE_ADDR  := 0x08900000
# Overridable so the mutation campaign can build the harness against a mutated
# copy of the interpreter without touching the tree. A pre-included mutated
# fp_convert.h wins over recomp.h's same-directory include in the final harness
# and direct production-helper translation units.
COSIM_INTERP_SRC ?= src/rt/guest_interp.c
COSIM_FP_CONVERT_PRELUDE ?=
COSIM_FP_CONVERT_PRELUDE_FLAG = $(if $(strip $(COSIM_FP_CONVERT_PRELUDE)),-include "$(COSIM_FP_CONVERT_PRELUDE)",)
COSIM_FPU_REFERENCE_SRC ?= fixtures/cosim/fpu_reference.c
COSIM_CODEGEN_ARGS ?= --stack-census

# The loader and codegen rules are named directly rather than through `pipeline`.
# This guest deliberately imports nothing, and tools/imports.py fails closed on an
# empty stub table -- correctly, for a title. Manufacturing an unused import stub
# just to satisfy that gate would put a fiction in the fixture; the import path is
# already covered end to end by fixtures/production_smoke.
cosim-selftest:
	"$(PYTHON)" $(COSIM_GENERATOR) generate --out-dir $(COSIM_FIXTURE)
	$(MAKE) $(COSIM_DIR)/cosim_image.bin $(COSIM_DIR)/cosim_recomp.c \
		GAME_NAME=cosim GAME_ELF=$(COSIM_FIXTURE)/guest.prx \
		GAME_BASE=$(COSIM_BASE_ADDR) GAME_ENTRY=$(COSIM_BASE_ADDR) \
		GAME_PSP_HEADER=$(COSIM_FIXTURE)/guest.psp \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(COSIM_DIR) FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 TRACE=1 \
		CODEGEN_USER_ARGS="$(COSIM_CODEGEN_ARGS)" \
		CODEGEN_TOOL=$(CODEGEN_TOOL)
	"$(PYTHON)" $(COSIM_GENERATOR) verify --build-dir $(COSIM_DIR)
	$(MAKE) cosim-selftest-run \
		GAME_NAME=cosim GAME_ELF=$(COSIM_FIXTURE)/guest.prx \
		GAME_BASE=$(COSIM_BASE_ADDR) GAME_ENTRY=$(COSIM_BASE_ADDR) \
		GAME_PSP_HEADER=$(COSIM_FIXTURE)/guest.psp \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(COSIM_DIR) FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 TRACE=1 \
		COSIM_INTERP_SRC=$(COSIM_INTERP_SRC) \
		COSIM_FP_CONVERT_PRELUDE=$(COSIM_FP_CONVERT_PRELUDE) \
		COSIM_FPU_REFERENCE_SRC=$(COSIM_FPU_REFERENCE_SRC) \
		CODEGEN_USER_ARGS="$(COSIM_CODEGEN_ARGS)"

# The same comparison with every generated body split at other entries
# (codegen --region-split-threshold=0): each conditional edge into another entry,
# taken or not taken, is a TAIL transfer to that entry's own body (the xing_* cells).
cosim-selftest-split:
	$(MAKE) cosim-selftest COSIM_DIR=$(COSIM_DIR)-split \
		COSIM_CODEGEN_ARGS="--stack-census --region-split-threshold=0"

# The same comparison with the generated code compiled at the package level (#1730): the
# optimizer must not change what a cell does. TRACE=1 keeps a call between instructions, so
# this lane proves the per-statement code; production-smoke and platform-ladder at the same
# level cover release builds without the trace.
COSIM_OPT_LEVEL ?= -O1
cosim-selftest-opt:
	$(MAKE) cosim-selftest COSIM_DIR=$(BUILD_ROOT)/cosim-opt RECOMP_OPT=$(COSIM_OPT_LEVEL)

# The cosimulation corpus with the LLVM IR backend (#330): every cell the backend lowers
# runs as an LLVM body in the AOT lane, against the same interpreter lane and on every
# channel. The manifest check fails the gate if no cell was actually lowered.
cosim-selftest-llvm:
	$(MAKE) cosim-selftest COSIM_DIR=$(BUILD_ROOT)/cosim-llvm CODEGEN_BACKEND=llvm
	"$(PYTHON)" tools/codegen_llvm.py check-manifest $(BUILD_ROOT)/cosim-llvm/cosim_recomp_llvm.json --min-lowered 20

# The LLVM backend's mutation campaign: wrong branch, lost or always-run delay slot,
# missing state writes, a late-latched computed target and a wrong call resume, each a
# defect in tools/codegen_llvm.py that the cosimulation must report.
cosim-mutants-llvm:
	"$(PYTHON)" fixtures/cosim/mutate.py --backend=llvm --build-dir $(BUILD_ROOT)/cosim-llvm-mutants
# The cosimulation corpus with local guest-register promotion (#284): the same cells,
# translated with codegen.py --promote-registers into their own build folder. The
# trace build mirrors every promoted write (SR_PROMO_SYNC), so every channel compares.
cosim-selftest-promoted:
	$(MAKE) cosim-selftest COSIM_DIR=$(BUILD_ROOT)/cosim-promoted \
		COSIM_CODEGEN_ARGS="--stack-census --promote-registers"

# Second phase: CHUNK_OBJS is derived with $(wildcard) at parse time, so the
# generated chunk sources must already exist before this target is parsed.
# No $(LIBS): the harness stubs the runtime's few host-library symbols, so
# the comparison links with neither SDL3 nor Vulkan. Keeping it that way is
# deliberate -- this gate should stay runnable anywhere the toolchain is,
# not inherit the graphics stack's environment requirements.
cosim-selftest-run: $(GENERIC_TITLE_CONFIG_HEADER) $(CHUNK_OBJS) $(LIBRARY_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CC) $(CFLAGS) $(COSIM_FP_CONVERT_PRELUDE_FLAG) -DSR_INSTRUCTION_TRACE \
		-I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) -I$(COSIM_FIXTURE) \
		-o $(BUILD_DIR)/cosim_selftest.exe \
		$(COSIM_HARNESS) $(COSIM_FPU_REFERENCE_SRC) $(CHUNK_OBJS) $(LIBRARY_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(COSIM_INTERP_SRC) src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/vfpu_tables.c src/rt/perf.c -lm
	$(BUILD_DIR)/cosim_selftest.exe $(BUILD_DIR)/$(GAME_NAME)_image.bin \
		$(COSIM_BASE_ADDR) $(COSIM_TRACES)

# Prove the comparator is load-bearing: each mutant is a semantic change to the
# interpreter, FPU helper or generator that must make the gate FAIL. A mutant
# that only breaks the build is not a kill and the driver rejects it.
cosim-mutants:
	"$(PYTHON)" fixtures/cosim/mutate.py --build-dir $(COSIM_DIR)

cosim-selftest-clean:
	$(MAKE) BUILD_DIR=$(COSIM_DIR) clean

# ---- Snapshot execution (#1842) -------------------------------------------------------
#
# src/rt/snapshot_runner.c runs one translated function from a snapshot (tools/nksnap.py,
# src/rt/nksnap.c) and the reference interpreter in src/ref from the same state, and
# prints MATCH, MISMATCH or NOT_CHECKED. This gate builds it against a source-owned
# synthetic module (fixtures/snapshot/generate.py), checks every case's verdict and the
# loader's refusals (fixtures/snapshot/selftest.py), then builds a second runner whose
# generated C has one changed immediate and requires the MISMATCH that change causes.
#
# The runner links recomp.c with SR_GATE_BUILD, which leaves sr_raw_syscall to the
# harness exactly as tools/codegen_gate.py does: the runner's definition ends the run at
# a raw syscall instead of running it. The reference interpreter carries its own
# sr_bitrev (src/ref/interp.cpp); it is renamed in that one object so it links beside the
# runtime's definition and the reference keeps using its own. The runner links no SDL, so
# its copy of recomp.c reads SDL_GetTicksNS under a name the runner defines.
SNAPSHOT_DIR      := $(BUILD_ROOT)/snapshot
SNAPSHOT_FIXTURE   := $(SNAPSHOT_DIR)/fixture
SNAPSHOT_GENERATOR := fixtures/snapshot/generate.py
SNAPSHOT_DRIVER    := fixtures/snapshot/selftest.py
SNAPSHOT_BASE_ADDR := 0x08A00000
SNAPSHOT_REF_FLAGS := -std=c++17 -O1 -Isrc/ref -Isrc/rt -fno-exceptions -Dsr_bitrev=sr_ref_bitrev
SNAPSHOT_RUNNER_SRCS := src/rt/snapshot_runner.c src/rt/nksnap.c src/rt/guest_interp.c \
	src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/cpu_lle.c src/rt/domain_mode.c \
	src/rt/stale_code.c src/rt/title_config.c src/rt/vfpu_tables.c src/rt/vfpu_interp.c \
	src/rt/perf.c src/rt/nested_frames.c
SNAPSHOT_GAME_VARS = GAME_NAME=snapshot GAME_ELF=$(SNAPSHOT_FIXTURE)/guest.prx \
	GAME_BASE=$(SNAPSHOT_BASE_ADDR) GAME_ENTRY=$(SNAPSHOT_BASE_ADDR) \
	GAME_PSP_HEADER=$(SNAPSHOT_FIXTURE)/guest.psp GAME_EXTRA_ELFS= TITLE_MANIFEST= \
	BUILD_DIR=$(SNAPSHOT_DIR) FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1

snapshot-selftest:
	"$(PYTHON)" $(SNAPSHOT_GENERATOR) generate --out-dir $(SNAPSHOT_FIXTURE)
	$(MAKE) $(SNAPSHOT_DIR)/snapshot_image.bin $(SNAPSHOT_DIR)/snapshot_recomp.c \
		$(SNAPSHOT_GAME_VARS) CODEGEN_TOOL=$(CODEGEN_TOOL)
	$(MAKE) snapshot-selftest-run $(SNAPSHOT_GAME_VARS)

# Second phase: CHUNK_OBJS and the mutant's chunk exist only after codegen ran.
SNAPSHOT_MUTANT_CHUNK = $(shell "$(PYTHON)" $(SNAPSHOT_DRIVER) find-chunk --build-dir $(BUILD_DIR) --game $(GAME_NAME) --cases $(SNAPSHOT_FIXTURE)/cases.json)
SNAPSHOT_MUTANT_DIR = $(BUILD_DIR)/mutant
SNAPSHOT_LINK_OBJS = $(BUILD_DIR)/snapshot_rt_recomp.o $(LIBRARY_OBJS) \
	$(BUILD_DIR)/$(GAME_NAME)_recomp.o $(BUILD_DIR)/snapshot_ref_interp.o \
	$(BUILD_DIR)/snapshot_ref.o -lstdc++ -lm

snapshot-selftest-run: $(GENERIC_TITLE_CONFIG_HEADER) $(CHUNK_OBJS) $(LIBRARY_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CXX) $(SNAPSHOT_REF_FLAGS) -c src/ref/interp.cpp -o $(BUILD_DIR)/snapshot_ref_interp.o
	$(CXX) $(SNAPSHOT_REF_FLAGS) -c src/ref/snapshot_ref.cpp -o $(BUILD_DIR)/snapshot_ref.o
	$(CC) $(CFLAGS) -DSR_GATE_BUILD -DSDL_GetTicksNS=snapshot_runner_ticks_ns \
		-I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-c src/rt/recomp.c -o $(BUILD_DIR)/snapshot_rt_recomp.o
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-o $(BUILD_DIR)/snapshot_runner.exe $(SNAPSHOT_RUNNER_SRCS) $(CHUNK_OBJS) $(SNAPSHOT_LINK_OBJS)
	"$(PYTHON)" $(SNAPSHOT_DRIVER) run --runner $(BUILD_DIR)/snapshot_runner.exe \
		--image $(BUILD_DIR)/$(GAME_NAME)_image.bin --cases $(SNAPSHOT_FIXTURE)/cases.json \
		--work $(BUILD_DIR)/cases
	"$(PYTHON)" $(SNAPSHOT_DRIVER) mutate --chunk $(SNAPSHOT_MUTANT_CHUNK) \
		--cases $(SNAPSHOT_FIXTURE)/cases.json --out $(SNAPSHOT_MUTANT_DIR)/$(notdir $(SNAPSHOT_MUTANT_CHUNK))
	$(CC) $(RECOMP_FLAGS) -I$(BUILD_DIR) -Isrc/rt -DSR_SDL3VK \
		-c $(SNAPSHOT_MUTANT_DIR)/$(notdir $(SNAPSHOT_MUTANT_CHUNK)) -o $(SNAPSHOT_MUTANT_DIR)/mutant_chunk.o
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-o $(BUILD_DIR)/snapshot_runner_mutant.exe $(SNAPSHOT_RUNNER_SRCS) \
		$(filter-out $(SNAPSHOT_MUTANT_CHUNK:.c=.o),$(CHUNK_OBJS)) $(SNAPSHOT_MUTANT_DIR)/mutant_chunk.o \
		$(SNAPSHOT_LINK_OBJS)
	"$(PYTHON)" $(SNAPSHOT_DRIVER) mutant --runner $(BUILD_DIR)/snapshot_runner_mutant.exe \
		--image $(BUILD_DIR)/$(GAME_NAME)_image.bin --cases $(SNAPSHOT_FIXTURE)/cases.json \
		--work $(SNAPSHOT_MUTANT_DIR)
	@echo "snapshot-selftest: PASS"


