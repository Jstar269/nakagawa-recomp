# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: the AOT/interpreter cosimulation, snapshot execution, form sweep and verify gates.
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
COSIM_LDFLAGS ?= $(filter -fuse-ld=lld,$(LDFLAGS))

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
	$(CC) $(CFLAGS) $(COSIM_LDFLAGS) $(COSIM_FP_CONVERT_PRELUDE_FLAG) -DSR_INSTRUCTION_TRACE \
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

# ---- Form sweep (#1844) ---------------------------------------------------------------
#
# tools/form_sweep.py asks tools/codegen.py which scalar instruction forms it translates,
# expands each form into encodings (every aliasing pattern x immediate bucket, sampled to
# FORM_SWEEP_CAP per form) and writes a synthetic module with one tiny guest function per
# encoding. The ordinary pipeline translates it; src/rt/form_sweep.c then runs every
# function on the translated body and on the reference interpreter (src/ref) from the same
# seeded states and compares the architectural results. A form the reference cannot run is
# NOT_CHECKED and named in the report, never a pass.
#
# form-sweep-selftest builds the sweep twice. The first build is the public sweep and must
# report zero mismatches. The second uses a generated mutant of the code generator (a
# wrapper that emits one wrong statement for addu: it reads rs after writing rd) over only
# that form, and the check requires a MISMATCH on exactly the instances where rd is rs and
# rs differs from rt. FORM_SWEEP_STATES and FORM_SWEEP_CAP size a deeper run
# (`form-sweep-deep`); the pull request defaults are small and the seed is fixed.
FORM_SWEEP_SEED   ?= 1844
FORM_SWEEP_CAP    ?= 40
FORM_SWEEP_STATES ?= 64
FORM_SWEEP_CHUNK  ?= 400
FORM_SWEEP_CODEGEN_ARGS ?=
FORM_SWEEP_ROOT   := $(BUILD_ROOT)/form-sweep
FORM_SWEEP_BASE_ADDR := 0x08C00000
FORM_SWEEP_TOOL   := tools/form_sweep.py
FORM_SWEEP_REF_FLAGS := -std=c++17 -O1 -Isrc/ref -Isrc/rt -fno-exceptions -Dsr_bitrev=sr_ref_bitrev
FORM_SWEEP_SRCS   := src/rt/form_sweep.c src/rt/nksnap.c src/rt/guest_interp.c \
	src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/cpu_lle.c src/rt/domain_mode.c \
	src/rt/stale_code.c src/rt/title_config.c src/rt/vfpu_tables.c src/rt/vfpu_interp.c \
	src/rt/perf.c src/rt/nested_frames.c
# One sweep: FORM_SWEEP_DIR holds fixture/ (the module and plan) and the build tree.
FORM_SWEEP_DIR    ?= $(FORM_SWEEP_ROOT)/public
FORM_SWEEP_ONLY   ?=
FORM_SWEEP_CODEGEN ?= tools/codegen.py
FORM_SWEEP_GAME_VARS = GAME_NAME=formsweep GAME_ELF=$(FORM_SWEEP_DIR)/fixture/guest.prx \
	GAME_BASE=$(FORM_SWEEP_BASE_ADDR) GAME_ENTRY=$(FORM_SWEEP_BASE_ADDR) \
	GAME_PSP_HEADER=$(FORM_SWEEP_DIR)/fixture/guest.psp GAME_EXTRA_ELFS= TITLE_MANIFEST= \
	BUILD_DIR=$(FORM_SWEEP_DIR) FUNCS_PER_CHUNK=$(FORM_SWEEP_CHUNK) PUBLIC_SAFE=1 \
	CODEGEN_USER_ARGS="$(FORM_SWEEP_CODEGEN_ARGS)"

form-sweep-build:
	"$(PYTHON)" $(FORM_SWEEP_TOOL) generate --out-dir $(FORM_SWEEP_DIR)/fixture \
		--seed $(FORM_SWEEP_SEED) --cap $(FORM_SWEEP_CAP) --states $(FORM_SWEEP_STATES) \
		$(foreach k,$(FORM_SWEEP_ONLY),--only $(k))
	$(MAKE) $(FORM_SWEEP_DIR)/formsweep_image.bin $(FORM_SWEEP_DIR)/formsweep_recomp.c \
		$(FORM_SWEEP_GAME_VARS) CODEGEN_TOOL=$(FORM_SWEEP_CODEGEN)
	$(MAKE) form-sweep-link $(FORM_SWEEP_GAME_VARS) CODEGEN_TOOL=$(FORM_SWEEP_CODEGEN)

# Second phase: the chunk objects exist only after codegen ran.
form-sweep-link: $(GENERIC_TITLE_CONFIG_HEADER) $(CHUNK_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CXX) $(FORM_SWEEP_REF_FLAGS) -c src/ref/interp.cpp -o $(BUILD_DIR)/sweep_ref_interp.o
	$(CXX) $(FORM_SWEEP_REF_FLAGS) -c src/ref/snapshot_ref.cpp -o $(BUILD_DIR)/sweep_ref.o
	$(CC) $(CFLAGS) -DSR_GATE_BUILD -DSDL_GetTicksNS=snapshot_runner_ticks_ns \
		-I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-c src/rt/recomp.c -o $(BUILD_DIR)/sweep_rt_recomp.o
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-o $(BUILD_DIR)/form_sweep.exe $(FORM_SWEEP_SRCS) $(CHUNK_OBJS) \
		$(BUILD_DIR)/sweep_rt_recomp.o $(LIBRARY_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(BUILD_DIR)/sweep_ref_interp.o $(BUILD_DIR)/sweep_ref.o -lstdc++ -lm
	-$(BUILD_DIR)/form_sweep.exe $(BUILD_DIR)/$(GAME_NAME)_image.bin $(FORM_SWEEP_BASE_ADDR) \
		$(BUILD_DIR)/fixture/plan.txt > $(BUILD_DIR)/results.txt

form-sweep-selftest:
	$(MAKE) form-sweep-build FORM_SWEEP_DIR=$(FORM_SWEEP_ROOT)/public
	"$(PYTHON)" $(FORM_SWEEP_TOOL) report --manifest $(FORM_SWEEP_ROOT)/public/fixture/manifest.json \
		--results $(FORM_SWEEP_ROOT)/public/results.txt
	"$(PYTHON)" $(FORM_SWEEP_TOOL) write-mutant --out $(FORM_SWEEP_ROOT)/mutant/mutant_codegen.py
	$(MAKE) form-sweep-build FORM_SWEEP_DIR=$(FORM_SWEEP_ROOT)/mutant FORM_SWEEP_ONLY=00.21 \
		FORM_SWEEP_CODEGEN=$(FORM_SWEEP_ROOT)/mutant/mutant_codegen.py
	"$(PYTHON)" $(FORM_SWEEP_TOOL) check-mutant --manifest $(FORM_SWEEP_ROOT)/mutant/fixture/manifest.json \
		--results $(FORM_SWEEP_ROOT)/mutant/results.txt
	@echo "form-sweep-selftest: PASS"

form-sweep-deep:
	$(MAKE) form-sweep-build FORM_SWEEP_DIR=$(FORM_SWEEP_ROOT)/deep FORM_SWEEP_CAP=0 FORM_SWEEP_STATES=2048
	"$(PYTHON)" $(FORM_SWEEP_TOOL) report --manifest $(FORM_SWEEP_ROOT)/deep/fixture/manifest.json \
		--results $(FORM_SWEEP_ROOT)/deep/results.txt


# ---- Sampled in-play differential checking (#1843) ------------------------------------
#
# src/rt/verify.c is compiled only into a verify build (-DSR_VERIFY, off by default): at
# sampled call entries it runs the reference interpreter (src/ref) over a copy of guest RAM,
# lets the real translated call run and compares the two end states. This gate builds
# src/rt/verify_harness.c against a synthetic module (fixtures/verify/generate.py: the
# snapshot module plus a counted-loop caller and a yielding loop), drives it through the
# sampler, rate, range, skip and mismatch cases (fixtures/verify/selftest.py), then builds a
# harness and a snapshot runner whose generated C has one changed immediate, requires the
# MISMATCH and replays its capture to the same verdict. Last it preprocesses the default
# runtime sources and requires that no verify hook is present in them.
#
# The hooks are the same C the default build compiles, behind SR_VERIFY: recomp.c's
# preprocessed text without the flag must contain no sr_verify token, so the default
# build's runtime objects cannot differ. Everything here links the same isolated runtime
# as the snapshot gate (see above for SR_GATE_BUILD, sr_bitrev and SDL_GetTicksNS).
VERIFY_DIR       := $(BUILD_ROOT)/verify
VERIFY_FIXTURE   := $(VERIFY_DIR)/fixture
VERIFY_GENERATOR := fixtures/verify/generate.py
VERIFY_DRIVER    := fixtures/verify/selftest.py
VERIFY_CORE_SRCS := $(filter-out src/rt/snapshot_runner.c,$(SNAPSHOT_RUNNER_SRCS)) src/rt/verify.c
# Preprocess one hooked runtime unit as the default build compiles it: no -DSR_VERIFY.
VERIFY_PREPROCESS = $(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) $(HLE_INCLUDES) -E -P src/rt/$(1).c -o $(BUILD_DIR)/default_$(1).i
VERIFY_GAME_VARS = GAME_NAME=verify GAME_ELF=$(VERIFY_FIXTURE)/guest.prx \
	GAME_BASE=$(SNAPSHOT_BASE_ADDR) GAME_ENTRY=$(SNAPSHOT_BASE_ADDR) \
	GAME_PSP_HEADER=$(VERIFY_FIXTURE)/guest.psp GAME_EXTRA_ELFS= TITLE_MANIFEST= \
	BUILD_DIR=$(VERIFY_DIR) FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1

verify-selftest:
	"$(PYTHON)" $(VERIFY_GENERATOR) generate --out-dir $(VERIFY_FIXTURE)
	"$(PYTHON)" $(VERIFY_DRIVER) recomp-flags --make "$(MAKE)" --build-dir $(VERIFY_DIR)/flags
	$(MAKE) $(VERIFY_DIR)/verify_image.bin $(VERIFY_DIR)/verify_recomp.c \
		$(VERIFY_GAME_VARS) CODEGEN_TOOL=$(CODEGEN_TOOL)
	$(MAKE) verify-selftest-run $(VERIFY_GAME_VARS) RECOMP_FLAGS="$(RECOMP_FLAGS) -DSR_VERIFY"

VERIFY_MUTANT_CHUNK = $(shell "$(PYTHON)" $(VERIFY_DRIVER) find-chunk --build-dir $(BUILD_DIR) --game $(GAME_NAME) --cases $(VERIFY_FIXTURE)/cases.json)
VERIFY_MUTANT_DIR = $(BUILD_DIR)/mutant
VERIFY_LINK_OBJS = $(BUILD_DIR)/verify_rt_recomp.o $(LIBRARY_OBJS) \
	$(BUILD_DIR)/$(GAME_NAME)_recomp.o $(BUILD_DIR)/verify_ref_interp.o \
	$(BUILD_DIR)/verify_ref.o -lstdc++ -lm

verify-selftest-run: $(GENERIC_TITLE_CONFIG_HEADER) $(CHUNK_OBJS) $(LIBRARY_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CXX) $(SNAPSHOT_REF_FLAGS) -c src/ref/interp.cpp -o $(BUILD_DIR)/verify_ref_interp.o
	$(CXX) $(SNAPSHOT_REF_FLAGS) -c src/ref/snapshot_ref.cpp -o $(BUILD_DIR)/verify_ref.o
	$(CC) $(CFLAGS) -DSR_VERIFY -DSR_GATE_BUILD -DSDL_GetTicksNS=snapshot_runner_ticks_ns \
		-I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-c src/rt/recomp.c -o $(BUILD_DIR)/verify_rt_recomp.o
	$(CC) $(CFLAGS) -DSR_VERIFY -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-o $(BUILD_DIR)/verify_harness.exe src/rt/verify_harness.c $(VERIFY_CORE_SRCS) \
		$(CHUNK_OBJS) $(VERIFY_LINK_OBJS)
	"$(PYTHON)" $(VERIFY_DRIVER) run --harness $(BUILD_DIR)/verify_harness.exe \
		--image $(BUILD_DIR)/$(GAME_NAME)_image.bin --cases $(VERIFY_FIXTURE)/cases.json \
		--work $(BUILD_DIR)/cases
	"$(PYTHON)" $(SNAPSHOT_DRIVER) mutate --chunk $(VERIFY_MUTANT_CHUNK) \
		--cases $(VERIFY_FIXTURE)/cases.json --out $(VERIFY_MUTANT_DIR)/$(notdir $(VERIFY_MUTANT_CHUNK))
	$(CC) $(RECOMP_FLAGS) -I$(BUILD_DIR) -Isrc/rt -DSR_SDL3VK \
		-c $(VERIFY_MUTANT_DIR)/$(notdir $(VERIFY_MUTANT_CHUNK)) -o $(VERIFY_MUTANT_DIR)/mutant_chunk.o
	$(CC) $(CFLAGS) -DSR_VERIFY -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-o $(BUILD_DIR)/verify_harness_mutant.exe src/rt/verify_harness.c $(VERIFY_CORE_SRCS) \
		$(filter-out $(VERIFY_MUTANT_CHUNK:.c=.o),$(CHUNK_OBJS)) $(VERIFY_MUTANT_DIR)/mutant_chunk.o \
		$(VERIFY_LINK_OBJS)
	$(CC) $(CFLAGS) -DSR_VERIFY -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) \
		-o $(BUILD_DIR)/verify_replay_mutant.exe src/rt/snapshot_runner.c $(VERIFY_CORE_SRCS) \
		$(filter-out $(VERIFY_MUTANT_CHUNK:.c=.o),$(CHUNK_OBJS)) $(VERIFY_MUTANT_DIR)/mutant_chunk.o \
		$(VERIFY_LINK_OBJS)
	"$(PYTHON)" $(VERIFY_DRIVER) mutant --harness $(BUILD_DIR)/verify_harness_mutant.exe \
		--replay $(BUILD_DIR)/verify_replay_mutant.exe \
		--image $(BUILD_DIR)/$(GAME_NAME)_image.bin --cases $(VERIFY_FIXTURE)/cases.json \
		--work $(VERIFY_MUTANT_DIR)
	$(call VERIFY_PREPROCESS,recomp)
	$(call VERIFY_PREPROCESS,hle)
	"$(PYTHON)" $(VERIFY_DRIVER) default-unchanged $(BUILD_DIR)/default_recomp.i $(BUILD_DIR)/default_hle.i
	"$(PYTHON)" $(VERIFY_DRIVER) exit-paths
	@echo "verify-selftest: PASS"
