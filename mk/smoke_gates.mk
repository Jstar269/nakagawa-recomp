# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: production, showcase, display and title smoke gates and the platform ladders.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# Two-phase build: `pipeline` (codegen) must finish and write the chunk .c files
# BEFORE `compile` is parsed, because CHUNK_OBJS is derived via $(wildcard) and is
# resolved at parse time. A single "all: pipeline compile" pass expands CHUNK_OBJS to
# empty (no .c exist yet) and never compiles the chunks on a clean build. Splitting into
# two make invocations makes a from-scratch build regenerate correctly.
all:
	$(MAKE) pipeline
	$(MAKE) compile

production-smoke:
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) generate --out-dir $(PRODUCTION_SMOKE_FIXTURE)
	$(MAKE) all \
		GAME_NAME=production_smoke \
		GAME_ELF=$(PRODUCTION_SMOKE_PRX) \
		GAME_BASE=0x08804000 \
		GAME_ENTRY=0x08804000 \
		GAME_PSP_HEADER=$(PRODUCTION_SMOKE_PSP) \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PRODUCTION_SMOKE_DIR) \
		FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 \
		LDFLAGS="$(LDFLAGS) -Wl,-Map,$(PRODUCTION_SMOKE_MAP)"
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) verify --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) run --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot

# Run the same smoke with the executable staged into a fresh directory outside
# the build tree (#294): a launcher/staging path bug must fail here, not only in
# the build-tree spelling.
production-smoke-staged: production-smoke
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) run-staged --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot

production-smoke-clean:
	$(MAKE) BUILD_DIR=$(PRODUCTION_SMOKE_DIR) clean

showcase:
	"$(PYTHON)" fixtures/showcase/showcase.py build

showcase-smoke: showcase
	"$(PYTHON)" fixtures/showcase/showcase.py smoke

# The same showcase route on a POSIX host, with no WSL hop: PSPDEV builds the
# guest PRX directly, `make all` (GAME_NAME/GAME_ELF/... exactly as the package
# route drives it) links the runtime with gcc/SDL3/Vulkan, and the smoke runs
# both demos headlessly. This is the Linux CI gate: it is the one target that
# proves the recompiled runtime builds AND boots AND reaches its first frame on
# Linux rather than only compiling.
#
# NK_SHOWCASE_DEMO_ROOT / NK_SHOWCASE_BUILD_ROOT move the staged output out of
# the repository for a source tree that is not a Git checkout; a real checkout
# uses the default ignored build/ tree.
showcase-linux:
	"$(PYTHON)" fixtures/showcase/showcase.py build
	"$(PYTHON)" fixtures/showcase/showcase.py smoke

# display-smoke builds the guest and asserts the presented framebuffer word
# headlessly. display-smoke-gui is the same image in the SDL3/Vulkan window and
# is deliberately NOT part of any aggregate gate: it needs a display.
display-smoke: DISPLAY_SMOKE_BUILD_FRAMES ?= $(DISPLAY_SMOKE_FRAMES)
display-smoke:
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) generate --out-dir $(DISPLAY_SMOKE_FIXTURE) \
		--frames $(DISPLAY_SMOKE_BUILD_FRAMES)
	$(MAKE) all \
		GAME_NAME=$(DISPLAY_SMOKE_NAME) \
		GAME_ELF=$(DISPLAY_SMOKE_PRX) \
		GAME_BASE=$(DISPLAY_SMOKE_BASE) \
		GAME_ENTRY=$(DISPLAY_SMOKE_BASE) \
		GAME_PSP_HEADER=$(DISPLAY_SMOKE_PSP) \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(DISPLAY_SMOKE_DIR) \
		FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) verify --build-dir $(DISPLAY_SMOKE_DIR)

# display-smoke-run is the aggregate source-owned gate: it runs the guest-only
# scheduler route and the --sched --gui route through the explicit no-window
# presenter, then records and compares GE/present events from the same guest.
display-smoke-run: display-smoke
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) run --build-dir $(DISPLAY_SMOKE_DIR)
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) run --build-dir $(DISPLAY_SMOKE_DIR) --gui --offscreen
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) vramdump --build-dir $(DISPLAY_SMOKE_DIR)
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) flight --build-dir $(DISPLAY_SMOKE_DIR)

display-smoke-gui:
	$(MAKE) display-smoke DISPLAY_SMOKE_BUILD_FRAMES=$(DISPLAY_SMOKE_DEMO_FRAMES)
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) run --build-dir $(DISPLAY_SMOKE_DIR) --gui

# display-smoke-quit closes the window while the long guest still runs: the quit must stop the
# scheduler and end through the driver's teardown with exit 0 (#1377). It needs a display, so it
# is not part of any aggregate gate either. SR_VIDEO=gdi runs it on the GDI fallback.
display-smoke-quit:
	$(MAKE) display-smoke DISPLAY_SMOKE_BUILD_FRAMES=$(DISPLAY_SMOKE_DEMO_FRAMES)
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) quit --build-dir $(DISPLAY_SMOKE_DIR)

# Drive the actual native-player PLAY NOW path against the launchable fixture.
# The child opens a real window, so this is a developer/display gate rather than
# part of the headless CI aggregate. The fixture generator records the child's
# machine-readable window/first-frame evidence.
display-smoke-player: player display-smoke-run
	"$(PYTHON)" $(DISPLAY_SMOKE_GENERATOR) run-player --build-dir $(DISPLAY_SMOKE_DIR)

display-smoke-clean:
	$(MAKE) BUILD_DIR=$(DISPLAY_SMOKE_DIR) clean

# Public title qualification (issue #699). title-qualification validates the checked-in
# public manifests, input profiles and staging declarations without building or launching.
# title-qualification-smoke builds the source-owned display fixture and the native player,
# then launches the fixture through the player in a sandboxed user-data root and writes a
# bring-up-schema report under build/<title id>/title-qualification/.
title-qualification:
	"$(PYTHON)" tools/title_qualification.py validate

title-qualification-smoke:
	"$(PYTHON)" tools/title_qualification.py build-smoke --make "$(MAKE)" --compiler "$(CC)"

# AOT-gap mode of the same fixture: the helper is omitted from native emission
# (build-time codegen choice), so region A reaches it through the typed production
# dispatch_call() seam. Generated registration owns the executable span; the
# interpreter executes the helper and hands off to registered AOT region B, whose
# final value is asserted by the production driver.
production-smoke-gap:
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) generate --out-dir $(PRODUCTION_SMOKE_GAP_FIXTURE) --mode aot-gap
	$(MAKE) all \
		GAME_NAME=production_smoke_gap \
		GAME_ELF=$(PRODUCTION_SMOKE_GAP_FIXTURE)/guest.prx \
		GAME_BASE=0x08804000 \
		GAME_ENTRY=0x08804000 \
		GAME_PSP_HEADER=$(PRODUCTION_SMOKE_GAP_FIXTURE)/guest.psp \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PRODUCTION_SMOKE_GAP_DIR) \
		FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 \
		CODEGEN_USER_ARGS=$(PRODUCTION_SMOKE_GAP_CODEGEN_ARGS) \
		LDFLAGS="$(LDFLAGS) -Wl,-Map,$(PRODUCTION_SMOKE_GAP_MAP)"
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) verify --build-dir $(PRODUCTION_SMOKE_GAP_DIR) --mode aot-gap
	"$(PYTHON)" $(PRODUCTION_SMOKE_GENERATOR) run --build-dir $(PRODUCTION_SMOKE_GAP_DIR) --mode aot-gap

production-smoke-gap-clean:
	$(MAKE) BUILD_DIR=$(PRODUCTION_SMOKE_GAP_DIR) clean

perf-benchmark:
	"$(PYTHON)" tools/run_perf_benchmarks.py --make "$(MAKE)" --python "$(PYTHON)" --output "$(BUILD_DIR)/perf-benchmark" --overhead

# ---------------------------------------------------------------------------
# Source-owned second-platform workload ladder.
#
# fixtures/platform_ladder/generate.py emits seven deliberately non-HST guest
# identities (see that module's docstring) and drives each one through the
# ordinary two-phase `all` target with PUBLIC_SAFE=1, no title manifest, no
# SR_DATAROOT, no extra spans, and no compatibility overrides. Each workload
# has its own base address, entry placement, import identity, segment/BSS
# layout, and expected result word. Every positive workload, ladder-gap
# included, is an ordinary PASS; the Title-2 unsupported-NID control must exit
# through the production fatal boundary.
# ---------------------------------------------------------------------------
PLATFORM_LADDER_DIR       := $(BUILD_ROOT)/platform-ladder
PLATFORM_LADDER_GENERATOR := fixtures/platform_ladder/generate.py

PL_ZERO_BASE   := 0x08940000
PL_RELOC_BASE  := 0x088C0000
PL_SCHED_BASE  := 0x08900000
PL_FPU_BASE    := 0x08980000
PL_FS_BASE     := 0x089C0000
PL_TITLE2_BASE := 0x08A40000
PL_TITLE2_NEGATIVE_BASE := 0x08A80000
PL_OVERLAY_BASE := 0x08804000
PL_OVERLAY_FIXTURE := $(PLATFORM_LADDER_DIR)/ladder-overlay/fixture

platform-ladder: platform-ladder-zero platform-ladder-reloc platform-ladder-gap platform-ladder-sched platform-ladder-fpu platform-ladder-fs platform-ladder-fs-negative platform-ladder-title2 platform-ladder-title2-negative platform-ladder-overlay

platform-ladder-zero:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-zero --out-dir $(PLATFORM_LADDER_DIR)/ladder-zero/fixture
	$(MAKE) all \
		GAME_NAME=pl_zero \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-zero/fixture/guest.prx \
		GAME_BASE=$(PL_ZERO_BASE) \
		GAME_ENTRY=0x08940040 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-zero \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-zero --build-dir $(PLATFORM_LADDER_DIR)/ladder-zero
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-zero --build-dir $(PLATFORM_LADDER_DIR)/ladder-zero

platform-ladder-reloc:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-reloc --out-dir $(PLATFORM_LADDER_DIR)/ladder-reloc/fixture
	$(MAKE) all \
		GAME_NAME=pl_reloc \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-reloc/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-reloc/fixture/guest.psp \
		GAME_BASE=$(PL_RELOC_BASE) \
		GAME_ENTRY=0x088C0020 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-reloc \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-reloc --build-dir $(PLATFORM_LADDER_DIR)/ladder-reloc
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-reloc --build-dir $(PLATFORM_LADDER_DIR)/ladder-reloc

# AOT-gap mode of ladder-reloc's tier chain: the interior callee is omitted
# from native emission so the call reaches the ordinary production dispatch()
# interpreter seam. Expected result is byte-identical to the intended guest.
platform-ladder-gap:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-gap --out-dir $(PLATFORM_LADDER_DIR)/ladder-gap/fixture
	$(MAKE) all \
		GAME_NAME=pl_gap_chain \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-gap/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-gap/fixture/guest.psp \
		GAME_BASE=0x08A00000 \
		GAME_ENTRY=0x08A00010 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-gap \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1 \
		CODEGEN_USER_ARGS=--omit-aot=0x08A00060
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-gap --build-dir $(PLATFORM_LADDER_DIR)/ladder-gap
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-gap --build-dir $(PLATFORM_LADDER_DIR)/ladder-gap

platform-ladder-sched:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-sched --out-dir $(PLATFORM_LADDER_DIR)/ladder-sched/fixture
	$(MAKE) all \
		GAME_NAME=pl_sched \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-sched/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-sched/fixture/guest.psp \
		GAME_BASE=$(PL_SCHED_BASE) \
		GAME_ENTRY=0x08900010 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-sched \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-sched --build-dir $(PLATFORM_LADDER_DIR)/ladder-sched
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-sched --build-dir $(PLATFORM_LADDER_DIR)/ladder-sched

platform-ladder-fpu:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-fpu --out-dir $(PLATFORM_LADDER_DIR)/ladder-fpu/fixture
	$(MAKE) all \
		GAME_NAME=pl_fpu \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-fpu/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-fpu/fixture/guest.psp \
		GAME_BASE=$(PL_FPU_BASE) \
		GAME_ENTRY=0x08980008 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-fpu \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-fpu --build-dir $(PLATFORM_LADDER_DIR)/ladder-fpu
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-fpu --build-dir $(PLATFORM_LADDER_DIR)/ladder-fpu

platform-ladder-fs:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-fs --out-dir $(PLATFORM_LADDER_DIR)/ladder-fs/fixture
	$(MAKE) all \
		GAME_NAME=pl_fs \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-fs/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-fs/fixture/guest.psp \
		GAME_BASE=$(PL_FS_BASE) \
		GAME_ENTRY=0x089C0018 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-fs \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-fs --build-dir $(PLATFORM_LADDER_DIR)/ladder-fs
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-fs --build-dir $(PLATFORM_LADDER_DIR)/ladder-fs

# Negative control: same executable and guest, but the payload file is absent.
# sceIoOpen must fail visibly and the guest must store the failure sentinel.
platform-ladder-fs-negative: platform-ladder-fs
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-fs --build-dir $(PLATFORM_LADDER_DIR)/ladder-fs --negative

platform-ladder-title2:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-title2 --out-dir $(PLATFORM_LADDER_DIR)/ladder-title2/fixture
	$(MAKE) all \
		GAME_NAME=pl_title2 \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-title2/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-title2/fixture/guest.psp \
		GAME_BASE=$(PL_TITLE2_BASE) \
		GAME_ENTRY=0x08A40020 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-title2 \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1 \
		CODEGEN_USER_ARGS=--omit-aot=0x08A40220
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-title2 --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-title2 --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2

# Negative control: a real mapped import stub dispatches an absent NID through
# sr_syscall() and the production unknown-HLE scheduler boundary must exit 7.
platform-ladder-title2-negative:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-title2-negative --out-dir $(PLATFORM_LADDER_DIR)/ladder-title2-negative/fixture
	$(MAKE) all \
		GAME_NAME=pl_title2_negative \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-title2-negative/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-title2-negative/fixture/guest.psp \
		GAME_BASE=$(PL_TITLE2_NEGATIVE_BASE) \
		GAME_ENTRY=0x08A80020 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-title2-negative \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-title2-negative --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2-negative
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-title2-negative --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2-negative --negative

# Guest-placed modules (#704): three source-owned overlays are translated without a
# load address (@runtime) and declared runtime-placed by the fixture's title manifest.
# Together they exceed the user partition; the guest loads, starts, stops and unloads
# them in sequence, and each lands wherever the partition allocator puts it.
platform-ladder-overlay:
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-overlay --out-dir $(PL_OVERLAY_FIXTURE)
	$(MAKE) all \
		GAME_NAME=pl_overlay \
		GAME_ELF=$(PL_OVERLAY_FIXTURE)/guest.prx \
		GAME_PSP_HEADER=$(PL_OVERLAY_FIXTURE)/guest.psp \
		GAME_BASE=$(PL_OVERLAY_BASE) \
		GAME_ENTRY=0x08804010 \
		GAME_EXTRA_ELFS="$(foreach overlay,a b c,$(PL_OVERLAY_FIXTURE)/overlay_$(overlay).prx@runtime)" \
		TITLE_MANIFEST=$(PL_OVERLAY_FIXTURE)/title.json \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-overlay \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-overlay --build-dir $(PLATFORM_LADDER_DIR)/ladder-overlay
	"$(PYTHON)" $(PLATFORM_LADDER_GENERATOR) run --workload ladder-overlay --build-dir $(PLATFORM_LADDER_DIR)/ladder-overlay

platform-ladder-clean:
	$(MAKE) BUILD_DIR=$(PLATFORM_LADDER_DIR) clean

profile-zero-e2e:
	"$(PYTHON)" -m unittest tools.test_profile_zero_e2e -v

# The generator the codegen rule runs. Overridable so the cosim mutation
# campaign can mutate the GENERATOR as well as the interpreter -- a
# differential proven against one side only is half proven. It MUST be
# defined above the codegen profile hash and the codegen rule: both expand
# it immediately, so a later definition would silently expand to empty.
CODEGEN_TOOL ?= tools/codegen.py

# Resolve the build-profile inputs before hashing so one Python process can reuse
# compiler identities and the shared ABI-header digest for all three profiles.
# Keep these definitions before the profile stamp targets below; their values and
# order are part of the existing profile identities.
# ge.c: software comparison rasterizer with PPSSPP-derived behavior. -O2 for speed.
GE_CFLAGS ?= -O2 -fno-math-errno -Wall -Wextra -Isrc/rt -DSR_SDL3VK
# Optional sparse front-end attribution is absent from normal GE builds. ge.c is also a
# portable-core source, so the define reaches both compilations or neither.
GE_FRONTEND_PROFILE ?= 0
ifeq ($(GE_FRONTEND_PROFILE),1)
override GE_CFLAGS += -DSR_GE_FRONTEND_PROFILE
override PORTABLE_CORE_CFLAGS += -DSR_GE_FRONTEND_PROFILE
endif
override GE_CFLAGS += -DSR_FLIGHT_RECORDER_LINKED
# A trace build compiles the runtime with the trace writer too. This must precede the
# runtime profile hash so switching TRACE rebuilds the runtime objects.
TRACE ?= 0
ifeq ($(TRACE),1)
override CFLAGS += -DSR_INSTRUCTION_TRACE
endif
# Optimization and memory-saving flags for massive machine-generated files.
# -O0: the default for ad-hoc and selftest builds. A package build passes the measured level
#   (tools/nk_core/package_cache.py PACKAGE_RECOMP_OPT, #1730).
# -fno-strict-aliasing: as the runtime. Generated code passes integer register arrays to the
#   VFPU helpers as float pointers, which -O2's type-based alias analysis must not exploit.
# -fno-var-tracking: Saves significant memory on huge functions.
# -ftrack-macro-expansion=0: Reduces memory overhead for macro-heavy code. GCC only: Clang
#   refuses the option, so it is passed only when $(CC) is not Clang (#317). GCC builds are unchanged.
RECOMP_OPT ?= -O0
RECOMP_CC_IS_CLANG := $(findstring clang version,$(shell "$(CC)" --version 2>&1))
RECOMP_GCC_ONLY_FLAGS := $(if $(RECOMP_CC_IS_CLANG),,-ftrack-macro-expansion=0)
RECOMP_FLAGS ?= $(RECOMP_OPT) -w -fno-strict-aliasing -fno-var-tracking $(RECOMP_GCC_ONLY_FLAGS) $(NAN_TRAP_CFLAG)
override RECOMP_FLAGS += $(STACK_CENSUS_CFLAG)
override RECOMP_FLAGS += -DSR_FLIGHT_RECORDER_LINKED
ifeq ($(TRACE),1)
override RECOMP_FLAGS += -DSR_INSTRUCTION_TRACE
endif
ifeq ($(PERF_AOT_INSTRUCTIONS),1)
override RECOMP_FLAGS += -DPERF_AOT_INSTRUCTIONS=1
endif
export NK_RUNTIME_PROFILE_ENTRIES := CFLAGS=$(CFLAGS)$(NEWLINE)GE_CFLAGS=$(GE_CFLAGS)$(NEWLINE)TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)$(NEWLINE)SDL3_PROVIDER=$(SDL3_PROVIDER)$(NEWLINE)SDL3_VERSION=$(SDL3_VERSION)$(NEWLINE)SDL3_DIR=$(SDL3_DIR)$(NEWLINE)PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)

# profile_hash(ENTRIES_VAR, COMPILER, ENTRIES_FILE): the parse-time hash of a build
# profile whose entries are the newline-separated value of the exported ENTRIES_VAR.
# Recipes read that variable from their environment (--entries-env), but GNU Make
# before 4.4 -- the Linux runners' 4.3 among them -- does not hand exported variables
# to $(shell ...), which silently hashed an empty profile there. So the parse-time
# hash reads the same text from ENTRIES_FILE, written with $(file ...): no command
# line, no quoting, any length. The file sits beside the profile stamps in
# $(BUILD_DIR), which the parse-time mkdir creates; a parse that creates nothing (a
# clean preview or an info-only parse) keeps the environment form.
ifdef NK_BUILD_DIR_READY
profile_hash = $(file >$(3),$($(1)))$(shell "$(PYTHON)" $(BUILD_PROFILE_TOOL) hash --compiler "$(2)" --entries-file "$(3)" --file "$(CPU_STATE_ABI_HEADER)")
else
profile_hash = $(shell "$(PYTHON)" $(BUILD_PROFILE_TOOL) hash --compiler "$(2)" --entries-env $(1) --file "$(CPU_STATE_ABI_HEADER)")
endif

# The codegen profile's entries never travel on a command line: EXTRA_ELF_ARGS names
# every guest module, and a title with a hundred modules under a long private path
# overruns the command line a shell will accept. The entries and their order are the
# ones the --entry form used, so the hash is unchanged.
export NK_CODEGEN_PROFILE_ENTRIES := GAME_NAME=$(GAME_NAME)$(NEWLINE)GAME_BASE=$(GAME_BASE)$(NEWLINE)CODEGEN_PROFILE_ARG=$(CODEGEN_PROFILE_ARG)$(NEWLINE)EXTRA_ELF_ARGS=$(EXTRA_ELF_ARGS)$(NEWLINE)EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)$(NEWLINE)FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)$(NEWLINE)CODEGEN_USER_ARGS=$(CODEGEN_USER_ARGS)$(NEWLINE)CODEGEN_TOOL=$(CODEGEN_TOOL)$(CHUNK_TARGET_PROFILE_LINE)$(SYMBOL_MAP_PROFILE_LINE)$(NAME_IMPORTS_PROFILE_LINE)$(ENHANCEMENT_PACKAGE_PROFILE_LINE)
ifdef NK_BUILD_DIR_READY
$(file >$(BUILD_DIR)/.codegen-profile-entries,$(NK_CODEGEN_PROFILE_ENTRIES))
$(file >$(BUILD_DIR)/.runtime-profile-entries,$(NK_RUNTIME_PROFILE_ENTRIES))
_PROFILE_HASHES := $(shell "$(PYTHON)" $(BUILD_PROFILE_TOOL) hash-profiles --codegen-compiler "$(PYTHON)" --codegen-entries-file "$(BUILD_DIR)/.codegen-profile-entries" --runtime-compiler "$(CC)" --runtime-entries-file "$(BUILD_DIR)/.runtime-profile-entries" --recomp-compiler "$(CC)" --recomp-entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --recomp-entry "TRACE=$(TRACE)" --recomp-entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" $(RECOMP_LLVM_HASH_ARG) --file "$(CPU_STATE_ABI_HEADER)")
CODEGEN_PROFILE_HASH := $(word 1,$(_PROFILE_HASHES))
RUNTIME_PROFILE_HASH := $(word 2,$(_PROFILE_HASHES))
RECOMP_PROFILE_HASH := $(word 3,$(_PROFILE_HASHES))
else
CODEGEN_PROFILE_HASH := $(call profile_hash,NK_CODEGEN_PROFILE_ENTRIES,$(PYTHON),$(BUILD_DIR)/.codegen-profile-entries)
endif
CODEGEN_PROFILE_STAMP := $(BUILD_DIR)/.codegen-profile-$(CODEGEN_PROFILE_HASH)

$(CODEGEN_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	"$(PYTHON)" $(BUILD_PROFILE_TOOL) record --output "$(CODEGEN_PROFILE_MANIFEST)" --section codegen --compiler "$(PYTHON)" --entries-env NK_CODEGEN_PROFILE_ENTRIES --file "$(CPU_STATE_ABI_HEADER)" --stamp "$@" --stale-glob ".codegen-profile-*" --invalidate-glob "$(BUILD_DIR)/$(GAME_NAME)_recomp*.o" --invalidate-glob "$(BUILD_DIR)/nk_lib_*.o"
