# Makefile for rebuilding / running a recompiled PSP game
#
# Usage:
#   mingw32-make GAME_NAME=mygame GAME_ELF=eboot.elf GAME_BASE=0x08804000 GAME_ENTRY=0x08804128
#

# Override these on the command line for your game (see README).
# NOTE: HST (flat-PRX image) requires GAME_BASE=0 GAME_ENTRY=0. The defaults below
# are for a generic rebased ELF. Using the wrong base → cascading label errors at link.
# Prefer hst_manager.ps1 (sets the correct values automatically).
GAME_NAME  ?= mygame
GAME_ELF   ?= eboot.elf
GAME_BASE  ?= 0x08804000
GAME_ENTRY ?= 0x08804000


ifeq ($(GAME_NAME),hst)
CODEGEN_PROFILE_ARG := --profile=hst
GAME_EXTRA_ELFS ?= place_game_here/EXTRACTED/decrypted/libfont.prx@0x32200000 \
                   place_game_here/EXTRACTED/decrypted/scePsmf_library.prx@0x32280000 \
                   place_game_here/EXTRACTED/decrypted/scePsmfP_library.prx@0x322f8868
GAME_PSP_HEADER ?= place_game_here/EXTRACTED/PSP_GAME/SYSDIR/EBOOT.BIN
# The analyzer applies no title-specific span of its own: an extra executable span
# outside the section table is title configuration, so the HST span is bound here
# explicitly for direct-Make builds. hst_manager.ps1 -TitleManifest supplies the same
# value from the validated manifest plan; a command-line/environment value overrides
# this default. Keep in sync with assets/titles/hst-ucus98701.json (the source of truth).
HST_EXTRA_SPANS ?= 0x00303194,0x00306e24
RUNTIME_OPT ?= -O2
RECOMP_OPT  ?= -O1
endif

CODEGEN_PROFILE_ARG ?=
GAME_EXTRA_ELFS ?=
GAME_PSP_HEADER ?=
HST_EXTRA_SPANS ?=
EXTRA_ELF_ARGS  = $(foreach elf,$(GAME_EXTRA_ELFS),--extra-elf=$(elf))
PSP_HEADER_ARG  = $(if $(strip $(GAME_PSP_HEADER)),--psp-header=$(GAME_PSP_HEADER),)
# Environment forms: the switch carries no pathname bytes at all.
PSP_HEADER_ENV_ARG = $(if $(strip $(GAME_PSP_HEADER)),--env-psp-header,)
EXTRA_ELF_ENV_ARG  = $(if $(strip $(GAME_EXTRA_ELFS)),--env-extra-elfs,)

# ---------------------------------------------------------------------------
# Guest-input pathname transport.
#
# Guest input pathnames (GAME_ELF, GAME_PSP_HEADER, GAME_EXTRA_ELFS) are operator
# configuration, and a legal filename may contain characters a command interpreter
# treats as syntax: cmd.exe splits on `&`, escapes with `^`, and expands `%VAR%`;
# sh expands `$VAR`, backticks and quotes. Interpolating such a pathname into a
# recipe therefore BOTH truncates the path the tool actually receives AND hands the
# interpreter command tokens taken from pathname data. These values are consequently
# transported to the tools through the process environment and read there with
# --env-elf / --env-psp-header / --env-extra-elfs; no recipe below interpolates the
# raw value into command text. See tools/build_profile.py.
#
# Two corruptions happen inside GNU Make itself, upstream of this transport, and are
# NOT repaired here (tools/test_build_truth.py::GuestInputTransportTests pins them):
#   * `$` in a command-line value is expanded by Make: GAME_ELF=a$b.elf arrives as
#     "a.elf". Escape it as `$$` on the command line.
#   * mingw32 Make carries its command line through the ANSI code page, so a pathname
#     outside that code page arrives transliterated (CJK becomes "?").
# Both leave a nonexistent path, on which the tools fail closed rather than opening a
# different file.
export GAME_ELF
export GAME_PSP_HEADER

# Make separates list elements with spaces, which is itself a legal filename character,
# so the list form is transported newline-separated under its own name. GAME_EXTRA_ELFS
# keeps its space-separated Make value so EXTRA_ELF_ARGS -- an input to the codegen
# profile hash -- is byte-identical to before and no build cache is invalidated.
EMPTY :=
SPACE := $(EMPTY) $(EMPTY)
define NEWLINE


endef
GAME_EXTRA_ELFS_ENV := $(subst $(SPACE),$(NEWLINE),$(strip $(GAME_EXTRA_ELFS)))
export GAME_EXTRA_ELFS_ENV

# Make cannot carry these pathnames in a prerequisite list: prerequisites are
# whitespace-delimited and glob-expanded, so a path containing a space, or one
# containing `[`/`]`/`*`/`?`, cannot be named there faithfully. Filtering such paths
# out of the prerequisite list would silently DROP the dependency and let a stale
# image or translation be reused after the ELF changed. Instead the identity of the
# inputs is recorded in a stamp whose own path contains no operator bytes, and the
# pipeline targets depend on the stamp. tools/build_profile.py rewrites it only when an
# input's size, mtime or content hash changes, and fails closed when a declared input
# is unset, empty, missing, or a directory.
GAME_INPUT_STAMP = $(BUILD_DIR)/.game-inputs

# A guest input is tracked when the operator actually declared one, or when the
# Makefile default happens to exist. A public lane that supplies its own generated
# artifacts and never names a GAME_ELF (the synthetic VFPU fuzz lane, and any caller
# hand-providing $(GAME_NAME)_recomp.c) has no guest input to be stale against, and
# must not be forced to invent one -- the same reasoning as VFPU_FUZZ_PREGENERATED
# below. When the operator DID declare one, the edge is always present, so deleting
# it fails the build rather than silently reusing stale output.
#
# The $(wildcard) here tests only the Makefile's own fixed default, never operator
# metacharacter data, and its failure mode is benign: an untracked input still makes
# the consuming tool fail closed at the point it tries to read the file.
GAME_INPUT_TRACKED := $(if $(filter command line environment,$(origin GAME_ELF)),1,$(if $(wildcard $(GAME_ELF)),1,))
GAME_INPUT_PREREQ = $(if $(GAME_INPUT_TRACKED),$(GAME_INPUT_STAMP),)
# Passed as an explicit argument rather than a recipe environment prefix: `VAR=x cmd`
# needs a POSIX shell, and Make on Windows falls back to cmd.exe when sh is not on
# PATH. The span therefore reaches only the primary-image analysis (codegen, VFPU
# fuzz); rebased extra guest modules never receive it.
EXTRA_SPAN_ARG  = $(if $(strip $(HST_EXTRA_SPANS)),--extra-span=$(strip $(HST_EXTRA_SPANS)),)

# GNU Make defines a built-in CC=cc with origin "default". A normal `CC ?= gcc`
# therefore never takes effect. Treat only that built-in/undefined state as unset,
# select the supported PATH-resolved UCRT64 GCC, and preserve environment or
# command-line overrides such as `CC=clang`.
ifneq ($(filter default undefined,$(origin CC)),)
CC := gcc
endif
PYTHON     ?= python
# The manager resolves and validates the current SDK before invoking Make.
# Direct Make callers must provide VULKAN_SDK explicitly (or export it).
VULKAN_SDK ?=
# PowerShell commonly exports this with backslashes while hst_manager passes the
# same directory with slashes. Canonicalize before hashing CFLAGS so direct Make
# and manager builds do not churn otherwise identical runtime profiles.
VULKAN_SDK := $(subst \,/,$(VULKAN_SDK))
# glslc from the Vulkan SDK is used ONLY by the opt-in `shaders` target below.
GLSLC ?= glslc
# Native runtime code is host-side C and can be tested independently.
# HST now has measured -O2 runtime / -O1 generated defaults. Generic/unqualified
# titles remain conservative -O0/-O0. Explicit overrides remain supported.
# Generated -O2 is NOT being adopted; -O1's measured build cost is higher but
# acceptable for HST.
RUNTIME_OPT ?= -O0
CFLAGS     ?= $(RUNTIME_OPT) -fno-strict-aliasing -Isrc/rt -I$(VULKAN_SDK)/Include -I$(VULKAN_SDK)/include -DSR_SDL3VK -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra
# The extracted HST archive tree is a concrete 56,672-file input contract.  The
# runtime rejects a different count so a truncated walk cannot become evidence.
ifeq ($(GAME_NAME),hst)
CFLAGS += -DSR_DATA_EXPECTED_COUNT=56672
endif
LDFLAGS ?= -L$(VULKAN_SDK)/Lib -L$(VULKAN_SDK)/lib
# DirectInput (-ldinput8 -ldxguid) removed: gui.c controller input is now handled entirely by
# the SDL3 gamepad subsystem (src/rt/gpu_sdl3vk). -lole32 stays (Media Foundation, h264_mf.c);
# -lwinmm stays (sched.c timeBeginPeriod); -lgdi32 stays (GDI fallback presenter).
LIBS       ?= -lSDL3 -lvulkan-1 -lmfplat -lgdi32 -lole32 -lwinmm

BUILD_DIR  ?= build/$(GAME_NAME)
FUNCS_PER_CHUNK ?= 2000
# Optional deterministic size-aware chunking: greedy contiguous fill toward a
# per-chunk emitted-byte budget (function order preserved, FUNCS_PER_CHUNK still
# caps chunk size). Empty keeps the legacy count-based partition byte-for-byte.
CHUNK_TARGET_BYTES ?=
ifeq ($(strip $(CHUNK_TARGET_BYTES)),)
CHUNK_BYTES_ARG :=
CHUNK_TARGET_ENTRY :=
else
CHUNK_BYTES_ARG := --target-chunk-bytes=$(CHUNK_TARGET_BYTES)
CHUNK_TARGET_ENTRY := --entry "CHUNK_TARGET_BYTES=$(CHUNK_TARGET_BYTES)"
endif

# Production-HLE PSP oracle stream. The target reuses hle_thread_selftest.exe, so the
# binary_sha256 in its record is the hash of the executable that actually emits stdout.
PSP_ORACLE_CASE          ?= callback-notify-check
PSP_ORACLE_SOURCE_COMMIT ?= $(shell git rev-parse HEAD)
PSP_ORACLE_MODEL         ?= unknown
PSP_ORACLE_FIRMWARE      ?= unknown
PSP_ORACLE_OUTPUT        ?= $(BUILD_DIR)/psp_oracle_nakagawa.txt
PSP_ORACLE_SMOKE_ELF     ?= fixtures/psp_oracle/build/nakagawa_psp_oracle.elf
PSP_ORACLE_SMOKE_DIR     ?= $(BUILD_DIR)/psp_oracle_smoke
PSP_ORACLE_SMOKE_EXE     ?= $(BUILD_DIR)/hle_thread_selftest_smoke.exe
PSP_ORACLE_SMOKE_OUTPUT  ?= $(BUILD_DIR)/psp_oracle_smoke_nakagawa.txt
PSP_ORACLE_SMOKE_STAMP   := $(PSP_ORACLE_SMOKE_DIR)/.generated
PSP_ORACLE_SMOKE_HEADER  := $(PSP_ORACLE_SMOKE_DIR)/smoke_recomp_funcs.h
PSP_ORACLE_SMOKE_CHUNK   := $(PSP_ORACLE_SMOKE_DIR)/smoke_recomp_0.c
PSP_ORACLE_SMOKE_ADAPTER := $(PSP_ORACLE_SMOKE_DIR)/smoke_entry.c

# Public, source-owned end-to-end production smoke. The recipe emits its binary
# inputs only under the ignored build tree, then deliberately re-enters the normal
# two-phase `all` path so the test cannot substitute a reduced link harness.
PRODUCTION_SMOKE_DIR       := build/production-smoke
PRODUCTION_SMOKE_FIXTURE   := $(PRODUCTION_SMOKE_DIR)/fixture
PRODUCTION_SMOKE_GENERATOR := fixtures/production_smoke/generate.py
PRODUCTION_SMOKE_PRX       := $(PRODUCTION_SMOKE_FIXTURE)/guest.prx
PRODUCTION_SMOKE_PSP       := $(PRODUCTION_SMOKE_FIXTURE)/guest.psp
PRODUCTION_SMOKE_MAP       := $(PRODUCTION_SMOKE_DIR)/production_smoke.map

# The AOT-gap mode of the same fixture: identical guest addresses, but the
# helper is omitted from native emission (--omit-aot) so region A reaches it
# through the ordinary production dispatch() seam.
PRODUCTION_SMOKE_GAP_DIR       := build/production-smoke-gap
PRODUCTION_SMOKE_GAP_FIXTURE   := $(PRODUCTION_SMOKE_GAP_DIR)/fixture
PRODUCTION_SMOKE_GAP_MAP       := $(PRODUCTION_SMOKE_GAP_DIR)/production_smoke_gap.map
PRODUCTION_SMOKE_GAP_CODEGEN_ARGS := --omit-aot=0x08804028

# Caller-supplied extra codegen arguments (build-time codegen choices such as
# the smoke's --omit-aot). Empty by default; carried into the codegen profile
# hash so changing it regenerates instead of reusing stale output.
CODEGEN_USER_ARGS ?=

# A filtered public candidate omits the lineage-sensitive PGF backend and the
# PGD/amctrl implementation. Full private checkouts default to both backends;
# candidate trees default to fail-closed project-authored unavailable backends.
PUBLIC_SAFE ?= $(if $(and $(wildcard src/rt/pgf.c),$(wildcard src/rt/pgd.c)),0,1)
ifneq ($(PUBLIC_SAFE),0)
ifneq ($(PUBLIC_SAFE),1)
$(error PUBLIC_SAFE must be 0 or 1)
endif
endif
ifeq ($(PUBLIC_SAFE),0)
ifeq ($(and $(wildcard src/rt/pgf.c),$(wildcard src/rt/pgd.c)),)
$(error PUBLIC_SAFE=0 requires the private PGF and PGD backends)
endif
PGF_BACKEND_SRC := src/rt/pgf.c
PGD_BACKEND_SRC := src/rt/pgd.c
ISO_BACKEND_SRC := src/rt/iso.c
AUDIO_BACKEND_SRC := src/rt/audio.c
ASSET_COPY_ARGS :=
else
PGF_BACKEND_SRC := src/rt/pgf_unavailable.c
PGD_BACKEND_SRC := src/rt/pgd_unavailable.c
ISO_BACKEND_SRC := src/rt/iso_unavailable.c
AUDIO_BACKEND_SRC := src/rt/audio_unavailable.c
ASSET_COPY_ARGS := -ExcludeOptionalFonts
CFLAGS += -DSR_PUBLIC_SAFE
endif

include mk/build_common.mk

BUILD_PROFILE_TOOL := tools/build_profile.py

# Runtime title configuration. The compiled runtime carries no title identity of its
# own: tools/title_runtime_config.py turns a *validated* title manifest's optional
# runtime_bindings block into a build-local header that only src/rt/title_config.c
# includes. With TITLE_MANIFEST empty the generator emits the generic configuration in
# which every optional binding is disabled -- so `make runtime-objects` needs no game
# input at all, and no default build inherits any title's addresses.
#
# HST binds its real values through the local, Git-ignored title manifest
# (assets/titles/hst-ucus98701.json, supplied by hst_manager.ps1 -TitleManifest or by
# TITLE_MANIFEST= on a direct Make command line). They are deliberately not encoded here.
TITLE_MANIFEST ?=
TITLE_CONFIG_TOOL := tools/title_runtime_config.py
TITLE_CONFIG_DIR ?= $(BUILD_DIR)
TITLE_CONFIG_HEADER := $(TITLE_CONFIG_DIR)/sr_title_config.h
TITLE_CONFIG_ARG := $(if $(strip $(TITLE_MANIFEST)),--manifest $(strip $(TITLE_MANIFEST)),)
# Identity of the effective configuration. Bound into RUNTIME_PROFILE_HASH below so a
# changed title binding invalidates stale runtime objects instead of relinking silently.
TITLE_CONFIG_DIGEST := $(shell $(PYTHON) $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --print-digest)
# An unreadable or invalid manifest prints nothing. Refusing here keeps a rejected title
# configuration from becoming an empty profile entry that hashes like some other build.
ifeq ($(strip $(TITLE_CONFIG_DIGEST)),)
$(error title runtime configuration could not be resolved; run "$(PYTHON) $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --print-digest" for the reason)
endif

# A build that explicitly identifies itself as HST must say where HST's title
# configuration comes from. Without it every optional binding is disabled and the build
# would quietly produce an HST executable with no fallback entry, no worker/launcher role
# and no VBLANK counters -- a broken runtime that looks like a successful build. This is
# a build-time refusal, not a title default: generic builds are untouched, and nothing
# here makes `runtime-objects` require a retail or title input.
HST_TITLE_MANIFEST := assets/titles/hst-ucus98701.json
ifeq ($(GAME_NAME),hst)
ifeq ($(strip $(TITLE_MANIFEST)),)
TITLE_CONFIG_HST_UNBOUND := 1
endif
endif

# Content-addressed identity of the EFFECTIVE title configuration, and the stamp that
# carries it. This is what the generated header depends on, because none of the header's
# natural prerequisites can express "the configuration changed":
#
#   - The manifest FILE is not a stable prerequisite. Dropping TITLE_MANIFEST removes it
#     from the prerequisite list entirely, so a bound -> unbound transition presents Make
#     with a target that is newer than everything left, the recipe does not run, and the
#     refusal below -- a recipe line -- never fires. The build then compiles a fresh
#     title_config.o against the PREVIOUS title's header while RUNTIME_PROFILE_HASH
#     records the generic digest.
#   - mtime cannot express it in the other direction either: a manifest older than an
#     existing generic header leaves that header "up to date", so the profile records the
#     title digest while the compiled configuration binds nothing.
#
# TITLE_CONFIG_DIGEST already covers the source id and every binding, so it subsumes the
# manifest file's content; the HST-unbound state is appended because it shares the generic
# digest yet must never reuse a header generated for some other configuration.
#
# The manifest file is deliberately NOT also a prerequisite. The digest is derived from
# its content at parse time, so the file adds nothing the identity does not already carry
# -- while touching it without changing it would make this recipe run on every build (the
# generator writes only on change, so the header's mtime would never catch up).
#
# The stamp DELETES the header rather than relying on being newer than it, and is included
# below with the other profile stamps so Make restarts and sees that deletion before it
# judges freshness. A changed identity and a regenerated header can land inside one
# filesystem timestamp tick, and "newer" cannot decide that; "absent" always can.
TITLE_CONFIG_IDENTITY := $(TITLE_CONFIG_DIGEST)$(if $(TITLE_CONFIG_HST_UNBOUND),-hst-unbound,)
TITLE_CONFIG_STAMP := $(TITLE_CONFIG_DIR)/.title-config-$(TITLE_CONFIG_IDENTITY)
$(TITLE_CONFIG_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) stamp --output "$@" --stale-glob ".title-config-*" --value "$(TITLE_CONFIG_IDENTITY)" --invalidate "$(TITLE_CONFIG_HEADER)"

$(TITLE_CONFIG_HEADER): $(TITLE_CONFIG_TOOL) tools/title_manifest.py $(TITLE_CONFIG_STAMP)
ifeq ($(TITLE_CONFIG_HST_UNBOUND),1)
	$(error GAME_NAME=hst needs a title configuration: pass TITLE_MANIFEST=$(HST_TITLE_MANIFEST) (the local, Git-ignored HST manifest) or build through hst_manager.ps1 -TitleManifest. Building without one would disable every title binding and produce a non-functional HST runtime. Generic builds need no manifest: use a different GAME_NAME.)
endif
	$(PYTHON) $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --output $@

# Title-neutral configuration for the game-input-free selftests. Those targets assert
# generic PSP behavior and install their own role fixtures, so binding them to a title
# would make their result depend on which title the tree happens to be building.
GENERIC_TITLE_CONFIG_DIR := $(BUILD_DIR)/title-config/generic
GENERIC_TITLE_CONFIG_HEADER := $(GENERIC_TITLE_CONFIG_DIR)/sr_title_config.h

$(GENERIC_TITLE_CONFIG_HEADER): $(TITLE_CONFIG_TOOL) tools/title_manifest.py
	$(PYTHON) $(TITLE_CONFIG_TOOL) --output $@

RUNTIME_PROFILE_MANIFEST := $(BUILD_DIR)/runtime_profile.json
RECOMP_PROFILE_MANIFEST := $(BUILD_DIR)/recomp_profile.json
CODEGEN_PROFILE_MANIFEST := $(BUILD_DIR)/codegen_profile.json

# Verification-gate inputs. These are PPSSPP-captured golden traces + (for the microtest
# gate) a PSP-compiled test module. They are external assets not committed to the repo, so
# `make verify` is meant to be run in CI with them supplied on the command line, e.g.:
#   make verify GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 \
#     CODEGEN_ORACLE=oracle/eboot.trace \
#     MICROTEST_MODULE=build/hst/microtest.elf MICROTEST_ORACLE=oracle/microtest.trace
# When an input is absent the corresponding gate reports BLOCKED with a real (non-zero) signal.
CODEGEN_ORACLE   ?=
MICROTEST_MODULE ?=
MICROTEST_ORACLE ?=
RUN_ELF_EXE      ?= $(BUILD_DIR)/run_elf.exe
VERIFY_WORKDIR   ?= $(BUILD_DIR)/verify

CXX        ?= g++

# ATRAC3+ decoder import sources (PR-A). Shared by the runtime link (PR-B
# h_AtracDecodeData integration) and the standalone selftest targets.
ATRAC3P_SRCS := src/rt/atrac3p/atrac3p_api.c \
	src/rt/atrac3p/libavcodec/atrac.c \
	src/rt/atrac3p/libavcodec/atrac3plus.c \
	src/rt/atrac3p/libavcodec/atrac3plusdec.c \
	src/rt/atrac3p/libavcodec/atrac3plusdsp.c \
	src/rt/atrac3p/libavcodec/bitstream.c \
	src/rt/atrac3p/libavcodec/fft_float.c \
	src/rt/atrac3p/libavcodec/fft_init_table.c \
	src/rt/atrac3p/libavcodec/mdct_float.c \
	src/rt/atrac3p/libavcodec/sinewin.c \
	src/rt/atrac3p/libavutil/float_dsp.c \
	src/rt/atrac3p/libavutil/intmath.c \
	src/rt/atrac3p/libavutil/log2_tab.c \
	src/rt/atrac3p/libavutil/mem.c \
	src/rt/atrac3p/libavutil/reverse.c

# PR-B: real ATRAC3+ decode in the HLE (h_AtracDecodeData). The decoder
# bridge (src/rt/atrac3p_bridge.c) and the imported FFmpeg n4.4 decoder TUs
# join the runtime link; the include paths are relative to src/rt/atrac3p/.
# Objects are prefixed so nested import paths cannot collide with flat
# runtime object names.
ATRAC3P_OBJS := $(patsubst src/rt/atrac3p/%.c,$(BUILD_DIR)/atrac3p_%.o,$(ATRAC3P_SRCS))
ATRAC3P_OBJ_DIRS := $(sort $(patsubst %/,%,$(dir $(ATRAC3P_OBJS))))

# Ensure the build directory and nested object directories exist up front so no
# per-recipe mkdir is needed.
# (mkdir -p under MSYS2 GNU make intermittently fails with "No such file or directory"
#  when invoked from a non-MSYS2 PowerShell host — explicit creation here avoids that;
#  and a per-recipe `mkdir -p` fails under cmd.exe when sh is absent from PATH.)
ifeq ($(OS),Windows_NT)
ifdef MSYSTEM
# MSYS2 shell (CI's msys2 {0} step): cmd may be absent from PATH, so the cmd
# branch below silently no-ops and the first compile fails with "can't create
# <obj>: No such file or directory". sh-style mkdir is reliable here.
$(shell mkdir -p "$(BUILD_DIR)")
$(shell mkdir -p "$(BUILD_DIR)/portable-core")
$(foreach d,$(ATRAC3P_OBJ_DIRS),$(shell mkdir -p "$(d)"))
else
$(shell cmd /c if not exist "$(BUILD_DIR)" mkdir "$(subst /,\,$(BUILD_DIR))" 1>nul 2>nul)
$(shell cmd /c if not exist "$(BUILD_DIR)\portable-core" mkdir "$(subst /,\,$(BUILD_DIR))\portable-core" 1>nul 2>nul)
$(foreach d,$(ATRAC3P_OBJ_DIRS),$(shell cmd /c if not exist "$(subst /,\,$(d))" mkdir "$(subst /,\,$(d))" 1>nul 2>nul))
endif
else
$(shell mkdir -p "$(BUILD_DIR)")
$(shell mkdir -p "$(BUILD_DIR)/portable-core")
$(foreach d,$(ATRAC3P_OBJ_DIRS),$(shell mkdir -p "$(d)"))
endif

RT_GE_O    := $(BUILD_DIR)/ge.o
RT_SRCS    := src/rt/recomp.c \
              src/rt/title_config.c \
              src/rt/vfpu_tables.c \
              src/rt/debug.c \
              src/rt/watchpoints_file.c \
              src/rt/guest_printf.c \
              src/rt/perf.c \
              src/rt/fbcap_policy.c \
              src/rt/ge_capture.c \
              src/rt/vfpu_interp.c \
              src/rt/hle.c \
              src/rt/sched.c \
              src/rt/sr_coro.c \
              $(ISO_BACKEND_SRC) \
              $(PGD_BACKEND_SRC) \
              src/rt/mpeg.c \
              $(PGF_BACKEND_SRC) \
              src/rt/gui.c \
              $(AUDIO_BACKEND_SRC) \
              src/rt/h264_mf.c \
              src/rt/h264_null.c \
              src/rt/savedata.c \
              src/rt/osk_win.c \
              src/rt/driver.c \
              src/rt/gpu_sdl3vk/sdl3vk.c \
              src/rt/gpu_sdl3vk/ge_gpu.c

RT_OBJS    := $(addprefix $(BUILD_DIR)/,$(notdir $(RT_SRCS:.c=.o)))

# Everything needed to COMPILE src/rt/hle.c, in one place. hle.c reaches into
# the PR-B ATRAC3+ decode bridge, so every target that compiles it needs these
# include paths -- not just the $(BUILD_DIR)/hle.o rule. Keeping them in a
# variable is the point: hand-copied flag lists are how hle-thread-selftest
# silently stopped building when #315 landed, which in turn hid #326 breaking
# the same target a second time.
HLE_INCLUDES := -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec -Isrc/rt/atrac3p/libavutil

# Everything needed to LINK src/rt/gpu_sdl3vk/sdl3vk.c, in one place. sdl3vk.c
# calls the framebuffer-capture policy (sr_fbcap_owner / sr_fbcap_path /
# sr_fbcap_exit_status, src/rt/fbcap_policy.c) from sdl3vk_capture_selftest,
# so fbcap_policy.c is a hard link dependency of the backend, not an extra of
# gpu-capture-selftest. --gc-sections cannot rescue an ad-hoc recipe that omits
# it: ld resolves undefined symbols before it discards unreachable sections.
# Same lesson as HLE_INCLUDES above -- when #57 added the policy it updated
# RT_SRCS and gpu-capture-selftest but not the other recipes that compile
# sdl3vk.c directly, silently breaking gpu-coherence-selftest and ge-replay.
# tools/test_build_truth.py enforces that every user of sdl3vk.c supplies it.
SDL3VK_SRCS := src/rt/gpu_sdl3vk/sdl3vk.c src/rt/fbcap_policy.c

$(BUILD_DIR)/atrac3p_%.o: src/rt/atrac3p/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/atrac3p_bridge.o: src/rt/atrac3p_bridge.c src/rt/atrac3p_bridge.h src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(DEPFLAGS) -c $< -o $@

# Host-neutral translation units that can be compiled on Linux without SDL3, Vulkan, game
# inputs, or generated code. This is deliberately an object-only portability gate, not a
# claim that the complete Linux runtime links or runs yet.
PORTABLE_CORE_DIR := $(BUILD_DIR)/portable-core
PORTABLE_CORE_SRCS := src/rt/recomp.c \
                      src/rt/title_config.c \
                      src/rt/vfpu_tables.c \
                      src/rt/debug.c \
                      src/rt/watchpoints_file.c \
                      src/rt/guest_printf.c \
                      src/rt/vfpu_interp.c \
                      $(ISO_BACKEND_SRC) \
                      $(PGD_BACKEND_SRC) \
                      src/rt/mpeg.c \
                      $(PGF_BACKEND_SRC) \
                      src/rt/savedata.c \
                      src/rt/ge.c \
                      src/rt/h264_null.c \
                      src/rt/sr_coro.c
PORTABLE_CORE_OBJS := $(patsubst src/rt/%.c,$(PORTABLE_CORE_DIR)/%.o,$(PORTABLE_CORE_SRCS))
PORTABLE_CORE_CFLAGS ?= -D_GNU_SOURCE -std=c11 -O0 -fno-strict-aliasing -Isrc/rt -Wall -Wextra -Werror=format

.PHONY: FORCE all pipeline compile compiler-info runtime-objects sched-selftest-one portable-core-objects atrac3p-objects public-safe-verify production-smoke production-smoke-clean production-smoke-gap production-smoke-gap-clean clean distclean verify selftest sched-selftest heap-selftest profiler-selftest coro-selftest hle-thread-selftest hle-thread-selftest-build dispatch-selftest dispatch-isolation-selftest dispatch-isolation-selftest-one asset-index-selftest fp-convert-selftest vfpu-tables-selftest watchpoints-file-selftest vfpu-interp-selftest atrac3p-selftest atrac3p-bridge-selftest atrac3p-title-accept gpu-coherence-selftest gpu-snapsync-selftest ge-replay run run_elf vfpu_fuzz vfpu_fuzz_build shaders shader-verify shader-repro-verify psp-oracle-vfpu psp-oracle-vfpu-build psp-oracle-nakagawa-smoke psp-oracle-nakagawa-smoke-build psp-oracle-nakagawa-smoke-generate gpu-capture-selftest
.SECONDARY:

# Stable diagnostic surface for CI and local setup checks. This target performs no
# compilation and makes GNU Make's selected compiler and assignment origin explicit.
compiler-info:
	@echo CC=$(CC)
	@echo CC_ORIGIN=$(origin CC)
	@echo RUNTIME_OPT=$(RUNTIME_OPT)
	@echo CFLAGS=$(CFLAGS)
	@echo RECOMP_OPT=$(RECOMP_OPT)
	@echo RECOMP_FLAGS=$(RECOMP_FLAGS)
	@echo FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)
	@echo CHUNK_TARGET_BYTES=$(CHUNK_TARGET_BYTES)
	@echo PUBLIC_SAFE=$(PUBLIC_SAFE)

public-safe-verify:
	$(MAKE) PUBLIC_SAFE=1 portable-core-objects

# Two-phase build: `pipeline` (codegen) must finish and write the chunk .c files
# BEFORE `compile` is parsed, because CHUNK_OBJS is derived via $(wildcard) and is
# resolved at parse time. A single "all: pipeline compile" pass expands CHUNK_OBJS to
# empty (no .c exist yet) and never compiles the chunks on a clean build. Splitting into
# two make invocations makes a from-scratch build regenerate correctly.
all:
	$(MAKE) pipeline
	$(MAKE) compile

production-smoke:
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) generate --out-dir $(PRODUCTION_SMOKE_FIXTURE)
	$(MAKE) all \
		GAME_NAME=production_smoke \
		GAME_ELF=$(PRODUCTION_SMOKE_PRX) \
		GAME_BASE=0x08804000 \
		GAME_ENTRY=0x08804000 \
		GAME_PSP_HEADER=$(PRODUCTION_SMOKE_PSP) \
		GAME_EXTRA_ELFS= HST_EXTRA_SPANS= TITLE_MANIFEST= \
		BUILD_DIR=$(PRODUCTION_SMOKE_DIR) \
		FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 \
		LDFLAGS="$(LDFLAGS) -Wl,-Map,$(PRODUCTION_SMOKE_MAP)"
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) verify --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) run --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot

production-smoke-clean:
	$(MAKE) BUILD_DIR=$(PRODUCTION_SMOKE_DIR) clean

# AOT-gap mode of the same fixture: the helper is omitted from native emission
# (build-time codegen choice), so region A reaches it through the ordinary
# production dispatch() seam. Until a production interpreter fallback exists,
# that miss must terminate under SR_DISPATCH_FATAL=1, and the run stage asserts
# exactly that evidence.
production-smoke-gap:
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) generate --out-dir $(PRODUCTION_SMOKE_GAP_FIXTURE) --mode aot-gap
	$(MAKE) all \
		GAME_NAME=production_smoke_gap \
		GAME_ELF=$(PRODUCTION_SMOKE_GAP_FIXTURE)/guest.prx \
		GAME_BASE=0x08804000 \
		GAME_ENTRY=0x08804000 \
		GAME_PSP_HEADER=$(PRODUCTION_SMOKE_GAP_FIXTURE)/guest.psp \
		GAME_EXTRA_ELFS= HST_EXTRA_SPANS= TITLE_MANIFEST= \
		BUILD_DIR=$(PRODUCTION_SMOKE_GAP_DIR) \
		FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 \
		CODEGEN_USER_ARGS=$(PRODUCTION_SMOKE_GAP_CODEGEN_ARGS) \
		LDFLAGS="$(LDFLAGS) -Wl,-Map,$(PRODUCTION_SMOKE_GAP_MAP)"
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) verify --build-dir $(PRODUCTION_SMOKE_GAP_DIR) --mode aot-gap
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) run --build-dir $(PRODUCTION_SMOKE_GAP_DIR) --mode aot-gap

production-smoke-gap-clean:
	$(MAKE) BUILD_DIR=$(PRODUCTION_SMOKE_GAP_DIR) clean

CODEGEN_PROFILE_HASH := $(shell $(PYTHON) $(BUILD_PROFILE_TOOL) hash --compiler "$(PYTHON)" --entry "GAME_NAME=$(GAME_NAME)" --entry "GAME_BASE=$(GAME_BASE)" --entry "CODEGEN_PROFILE_ARG=$(CODEGEN_PROFILE_ARG)" --entry "EXTRA_ELF_ARGS=$(EXTRA_ELF_ARGS)" --entry "EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)" --entry "FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)" --entry "CODEGEN_USER_ARGS=$(CODEGEN_USER_ARGS)" $(CHUNK_TARGET_ENTRY))
CODEGEN_PROFILE_STAMP := $(BUILD_DIR)/.codegen-profile-$(CODEGEN_PROFILE_HASH)

$(CODEGEN_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) record --output "$(CODEGEN_PROFILE_MANIFEST)" --section codegen --compiler "$(PYTHON)" --entry "GAME_NAME=$(GAME_NAME)" --entry "GAME_BASE=$(GAME_BASE)" --entry "CODEGEN_PROFILE_ARG=$(CODEGEN_PROFILE_ARG)" --entry "EXTRA_ELF_ARGS=$(EXTRA_ELF_ARGS)" --entry "EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)" --entry "FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)" --entry "CODEGEN_USER_ARGS=$(CODEGEN_USER_ARGS)" $(CHUNK_TARGET_ENTRY) --stamp "$@" --stale-glob ".codegen-profile-*" --invalidate-glob "$(BUILD_DIR)/$(GAME_NAME)_recomp*.o"

# Re-checked on every invocation that needs a guest input (hence FORCE), but rewritten
# only when an input's identity actually changed, so dependents do not rebuild spuriously.
# GAME_PSP_HEADER and GAME_EXTRA_ELFS_ENV are optional; GAME_ELF is not.
$(GAME_INPUT_STAMP): $(BUILD_PROFILE_TOOL) FORCE
	$(PYTHON) $(BUILD_PROFILE_TOOL) stamp-inputs --env GAME_ELF --optional-env GAME_PSP_HEADER --optional-env GAME_EXTRA_ELFS_ENV --list-env GAME_EXTRA_ELFS_ENV --out $@

FORCE:

pipeline: $(BUILD_DIR)/$(GAME_NAME)_image.bin $(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_imports.toml

$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py
	$(PYTHON) tools/prxload.py --env-elf $(GAME_BASE) $(PSP_HEADER_ENV_ARG) --out=$@

$(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h: $(GAME_INPUT_PREREQ) tools/codegen.py tools/analyze.py tools/entry_frame_balance.py tools/prxload.py tools/host_stubs.py tools/imports.py $(CODEGEN_PROFILE_STAMP)
	$(PYTHON) tools/codegen.py --env-elf $(BUILD_DIR)/$(GAME_NAME)_recomp.c --base=$(GAME_BASE) $(CODEGEN_PROFILE_ARG) $(EXTRA_SPAN_ARG) $(EXTRA_ELF_ENV_ARG) --funcs-per-chunk=$(FUNCS_PER_CHUNK) $(CHUNK_BYTES_ARG) $(CODEGEN_USER_ARGS)

$(BUILD_DIR)/$(GAME_NAME)_imports.toml: $(GAME_INPUT_PREREQ) tools/imports.py tools/analyze.py tools/prxload.py
	$(PYTHON) tools/imports.py --env-elf $(GAME_BASE) --toml=$@

# ge.c: software comparison rasterizer with PPSSPP-derived behavior. -O2 for speed.
GE_CFLAGS ?= -O2 -fno-math-errno -Wall -Wextra -Isrc/rt -DSR_SDL3VK
RUNTIME_PROFILE_HASH := $(shell $(PYTHON) $(BUILD_PROFILE_TOOL) hash --compiler "$(CC)" --entry "CFLAGS=$(CFLAGS)" --entry "GE_CFLAGS=$(GE_CFLAGS)" --entry "TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)")
RUNTIME_PROFILE_STAMP := $(BUILD_DIR)/.runtime-profile-$(RUNTIME_PROFILE_HASH)
RUNTIME_INVALIDATE_ARGS := $(foreach obj,$(RT_GE_O) $(RT_OBJS),--invalidate "$(obj)")

$(RUNTIME_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) record --output "$(RUNTIME_PROFILE_MANIFEST)" --section runtime --compiler "$(CC)" --entry "CFLAGS=$(CFLAGS)" --entry "GE_CFLAGS=$(GE_CFLAGS)" --entry "TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)" --stamp "$@" --stale-glob ".runtime-profile-*" $(RUNTIME_INVALIDATE_ARGS)

$(RT_GE_O): src/rt/ge.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(GE_CFLAGS) $(DEPFLAGS) -c src/rt/ge.c -o $@

# Optimization and memory-saving flags for massive machine-generated files.
# -O0: Conservative default for generic/unqualified titles to prevent compiler OOM / hangs.
# -O1: Measured and qualified default for HST.
# -fno-var-tracking: Saves significant memory on huge functions.
# -ftrack-macro-expansion=0: Reduces memory overhead for macro-heavy code.
RECOMP_OPT ?= -O0
RECOMP_FLAGS ?= $(RECOMP_OPT) -w -fno-var-tracking -ftrack-macro-expansion=0
TRACE ?= 0
ifeq ($(TRACE),1)
RECOMP_FLAGS += -DSR_INSTRUCTION_TRACE
endif

# Make the object flavour explicit. Switching TRACE forces only the generated
# chunks to rebuild, avoiding a stale trace-enabled object in a release binary
# (or a trace-disabled object in an oracle run).
TRACE_STAMP := $(BUILD_DIR)/.recomp-trace-$(TRACE)
$(TRACE_STAMP):
	$(PYTHON) $(BUILD_PROFILE_TOOL) stamp --output "$@" --stale-glob ".recomp-trace-*" --value "$(TRACE)"

RECOMP_PROFILE_HASH := $(shell $(PYTHON) $(BUILD_PROFILE_TOOL) hash --compiler "$(CC)" --entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --entry "TRACE=$(TRACE)")
RECOMP_PROFILE_STAMP := $(BUILD_DIR)/.recomp-profile-$(RECOMP_PROFILE_HASH)
$(RECOMP_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) record --output "$(RECOMP_PROFILE_MANIFEST)" --section generated --compiler "$(CC)" --entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --entry "TRACE=$(TRACE)" --stamp "$@" --stale-glob ".recomp-profile-*" --invalidate-glob "$(BUILD_DIR)/$(GAME_NAME)_recomp*.o"

# Compile the chunked generated C code.

$(BUILD_DIR)/$(GAME_NAME)_recomp_%.o: $(BUILD_DIR)/$(GAME_NAME)_recomp_%.c src/rt/recomp.h $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	$(CC) $(RECOMP_FLAGS) -I$(BUILD_DIR) -Isrc/rt -DSR_SDL3VK $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/$(GAME_NAME)_recomp.o: $(BUILD_DIR)/$(GAME_NAME)_recomp.c src/rt/recomp.h $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	$(CC) $(RECOMP_FLAGS) -I$(BUILD_DIR) -Isrc/rt -DSR_SDL3VK $(DEPFLAGS) -c $< -o $@

# Compile runtime sources.
$(BUILD_DIR)/%.o: src/rt/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/rt/gpu_sdl3vk/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The one translation unit that reads the build-local generated configuration. Every
# other runtime source consumes the generic accessors in src/rt/title_config.h, so the
# generated include path stops here rather than leaking into CFLAGS.
$(BUILD_DIR)/title_config.o: src/rt/title_config.c src/rt/title_config.h $(TITLE_CONFIG_HEADER) $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) -I$(TITLE_CONFIG_DIR) $(DEPFLAGS) -c $< -o $@

$(PORTABLE_CORE_DIR)/title_config.o: src/rt/title_config.c src/rt/title_config.h $(TITLE_CONFIG_HEADER)
	$(CC) $(PORTABLE_CORE_CFLAGS) -I$(TITLE_CONFIG_DIR) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/hle.o: src/rt/hle.c src/rt/asset_index.h src/rt/pgf_api.h src/rt/atrac3p_bridge.h src/rt/gpu_sdl3vk/ge_gpu.h
	$(CC) $(CFLAGS) $(HLE_INCLUDES) $(DEPFLAGS) -c $< -o $@
$(BUILD_DIR)/pgf.o: src/rt/pgf.c src/rt/pgf_api.h src/rt/pgf.h
$(BUILD_DIR)/pgf_unavailable.o: src/rt/pgf_unavailable.c src/rt/pgf_api.h

runtime-objects: shader-verify $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o

$(PORTABLE_CORE_DIR)/%.o: src/rt/%.c src/rt/recomp.h
	$(CC) $(PORTABLE_CORE_CFLAGS) $(DEPFLAGS) -c $< -o $@

portable-core-objects: $(PORTABLE_CORE_OBJS)

atrac3p-objects: $(ATRAC3P_OBJS)

CHUNK_OBJS = $(patsubst %.c,%.o,$(wildcard $(BUILD_DIR)/$(GAME_NAME)_recomp_*.c))
DEP_FILES = $(patsubst %.o,%.d,$(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(PORTABLE_CORE_OBJS) $(CHUNK_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(BUILD_DIR)/vfpu_fuzz.o)

# Treat profile stamps as generated included makefiles. GNU Make restarts after
# creating a missing flavour, so objects invalidated by that recipe are absent
# before target freshness is evaluated (avoiding timestamp-resolution races).
ifeq ($(strip $(filter clean distclean,$(MAKECMDGOALS))),)
-include $(CODEGEN_PROFILE_STAMP) $(RUNTIME_PROFILE_STAMP) $(RECOMP_PROFILE_STAMP) $(TITLE_CONFIG_STAMP)
endif
-include $(DEP_FILES)

compile: shader-verify $(CHUNK_OBJS) $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CC) $(CFLAGS) $(LDFLAGS) -Wl,--no-insert-timestamp -o $(BUILD_DIR)/$(GAME_NAME).exe \
		$(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(CHUNK_OBJS) \
		$(RT_GE_O) \
		$(RT_OBJS) \
		$(ATRAC3P_OBJS) \
		$(BUILD_DIR)/atrac3p_bridge.o \
		$(LIBS)
	pwsh -NoProfile -ExecutionPolicy Bypass -File copy_build_assets.ps1 -BuildDir "$(BUILD_DIR)" $(ASSET_COPY_ARGS)
	@$(PYTHON) -c "print('Build finished: $(BUILD_DIR)/$(GAME_NAME).exe')"

clean:
	$(PYTHON) -c "import shutil; shutil.rmtree(r'$(BUILD_DIR)', ignore_errors=True)"

distclean:
	@echo "Removing stale build artefacts (preserving .exe and .pdb for debugger)"
	$(PYTHON) -c "from pathlib import Path; r=Path(r'$(BUILD_DIR)'); [p.unlink(missing_ok=True) for g in ('**/*.o','**/*.d','.*-profile-*','*_profile.json') for p in r.glob(g)]"
	-rm -f logs/build_out_recomp.log logs/build_err_recomp.log logs/recomp_err.log logs/obj_err.log logs/link_err.log

# sched-selftest — white-box scheduler/lifecycle unit tests (src/rt/sched_selftest.c).
# No game inputs needed; #includes sched.c for direct access to pick_next()/TCB state and
# links the real sr_coro backend. Asserts PSP strict-priority selection, equal-priority
# rotation, non-runnable exclusion, implicit thread exit on entry return, role-UID
# capture, stack-exhaustion create failure, and sr_coro self-switch/park guards.
# Exit code 0 = all invariants hold.
# Title-configuration matrix. The SAME scheduler source is built three times against
# three generated runtime title configurations, so a title binding that leaked back into
# generic scheduler code fails at least one of them:
#   generic  -- no title configuration; every optional binding disabled
#   fixture-a -- assets/titles/pspdev-phase5.json (source-owned synthetic addresses)
#   fixture-b -- assets/titles/synthetic.json (a different source-owned address set)
# Each generated header lands in its own directory so the three builds cannot share one.
SCHED_SELFTEST_CONFIGS := generic fixture-a fixture-b
SCHED_SELFTEST_MANIFEST_generic :=
SCHED_SELFTEST_MANIFEST_fixture-a := assets/titles/pspdev-phase5.json
SCHED_SELFTEST_MANIFEST_fixture-b := assets/titles/synthetic.json

sched-selftest:
	@$(foreach cfg,$(SCHED_SELFTEST_CONFIGS),$(MAKE) --no-print-directory sched-selftest-one SCHED_SELFTEST_CONFIG=$(cfg) &&) true

# One configuration of the matrix. SCHED_SELFTEST_CONFIG names the flavour; the title
# configuration is generated fresh into build/<game>/title-config/<flavour>/.
SCHED_SELFTEST_CONFIG ?= generic
SCHED_SELFTEST_DIR := $(BUILD_DIR)/title-config/$(SCHED_SELFTEST_CONFIG)
SCHED_SELFTEST_MANIFEST := $(SCHED_SELFTEST_MANIFEST_$(SCHED_SELFTEST_CONFIG))
SCHED_SELFTEST_CONFIG_ARG := $(if $(strip $(SCHED_SELFTEST_MANIFEST)),--manifest $(strip $(SCHED_SELFTEST_MANIFEST)),)

sched-selftest-one: $(TITLE_CONFIG_TOOL) tools/title_manifest.py
	$(PYTHON) $(TITLE_CONFIG_TOOL) $(SCHED_SELFTEST_CONFIG_ARG) --output $(SCHED_SELFTEST_DIR)/sr_title_config.h
	$(CC) $(CFLAGS) -I$(SCHED_SELFTEST_DIR) $(LDFLAGS) -o $(BUILD_DIR)/sched_selftest_$(SCHED_SELFTEST_CONFIG).exe \
		src/rt/sched_selftest.c src/rt/sr_coro.c src/rt/title_config.c $(LIBS)
	$(BUILD_DIR)/sched_selftest_$(SCHED_SELFTEST_CONFIG).exe

# heap-selftest — white-box unit tests for the guest heap allocator's boundary-tag
# coalescing (src/rt/heap_selftest.c). No game inputs needed; #includes recomp.c for
# direct access to the allocator statics and sr_heap_stats(), and drives the same
# sr_newlib_malloc/free entry points the generated code calls. Asserts forward and
# backward merges, the #122 regression (a large allocation must succeed after the
# arena is fragmented and released), refusal to merge across a live block even with
# a forged boundary tag, clean payload across merge seams, bounded free-list growth
# under randomized churn, and corruption/overflow guardrails. Exit code 0 = all
# invariants hold. vfpu_tables.c is linked because the inlined recomp.c now calls
# sr_vfpu_load() and reads the VFPU LUT pointers (issue #187); without it the
# standalone binary fails to link after the table-loader integration.
heap-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/heap_selftest.exe \
		src/rt/heap_selftest.c src/rt/vfpu_tables.c src/rt/title_config.c $(LIBS) -lm
	$(BUILD_DIR)/heap_selftest.exe

# profiler-selftest — production profiler hash-table regression suite. Exercises PC zero as a
# real key and a deliberately saturated 64-probe collision window without game inputs.
profiler-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -DSR_PROFILER_SELFTEST \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables $(LDFLAGS) \
		-Wl,--gc-sections -o $(BUILD_DIR)/profiler_selftest.exe \
		src/rt/profiler_selftest.c src/rt/recomp.c src/rt/title_config.c $(LIBS)
	$(BUILD_DIR)/profiler_selftest.exe

# vfpu-tables-selftest — fail-closed VFPU table loader regression suite (issue #187):
# SHA-256 known-answer vectors, value-domain validators against synthetic corrupt
# buffers, file-level loader tests against temporary table roots (truncated, extra
# data, wrong content of the same length, endian-swapped, missing files), repeated
# initialization and concurrent first use. No game inputs required; the success
# path validates against the committed assets/vfpu/ tables.
vfpu-tables-selftest:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_tables_selftest.exe \
		src/rt/vfpu_tables_selftest.c src/rt/vfpu_tables.c $(LIBS)
	$(BUILD_DIR)/vfpu_tables_selftest.exe

# watchpoints-file-selftest — bounded parser regression for the derived
# watchpoints.json runtime artifact (issue #188): the exact dashboard-writer
# fixture round-trips into the expected native watchpoint set, plus fail-closed
# cases (wrong version/format, malformed JSON, out-of-range/reversed/oversized
# spans, bad labels, duplicates, over-capacity lists, legacy bare-array form).
# No game inputs or private data required.
watchpoints-file-selftest:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/watchpoints_file_selftest.exe \
		src/rt/watchpoints_file_selftest.c src/rt/watchpoints_file.c $(LIBS)
	$(BUILD_DIR)/watchpoints_file_selftest.exe

# atrac3p-selftest — standalone ATRAC3+ decoder regression suite (PR-A,
# src/rt/atrac3p/). Public checks are source-owned (create validation, NULL/
# oversized/garbage rejection with the nb_samples=0 contract, a deterministic
# terminator-frame decode through the production entry point, determinism
# across instances/reset/flush, destroy(NULL)). The optional private fixture
# hook (ATRAC3P_FIXTURE=<dir> with stream.bin + meta.txt, see
# src/rt/atrac3p_selftest.c) decodes and SHA-256s the whole stream with the
# same hash primitive as the VFPU table loader; it is reported as SKIP when
# unset and must never be committed. No game inputs required.

atrac3p-selftest:
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(LDFLAGS) \
		-o $(BUILD_DIR)/atrac3p_selftest.exe \
		src/rt/atrac3p_selftest.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c -lm
	$(BUILD_DIR)/atrac3p_selftest.exe

# atrac3p-bridge-selftest — regression suite for the PSP ATRAC3+ HLE decode
# bridge (PR-B, src/rt/atrac3p_bridge.c). Source-owned tests: create
# validation, NULL/contract-violation rejection, the deterministic terminator
# and MONO transform-path canaries through the production bridge entry point,
# determinism across instances/reset/recreate, destroy(NULL). No game inputs,
# no fixtures required.
atrac3p-bridge-selftest:
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(LDFLAGS) \
		-o $(BUILD_DIR)/atrac3p_bridge_selftest.exe \
		src/rt/atrac3p_bridge_selftest.c src/rt/atrac3p_bridge.c \
		$(ATRAC3P_SRCS) src/rt/vfpu_tables.c -lm
	$(BUILD_DIR)/atrac3p_bridge_selftest.exe

# atrac3p-title-accept — PRIVATE title acceptance (PR-C, #286/#32). Decodes the
# lawful private title ATRAC3+ stream (canonically the extracted bgm_title.sgb)
# through the production PR-A decoder/PR-B bridge and asserts valid, nonzero,
# deterministic PCM. Prints aggregate statistics only; never writes or hashes
# retail content. SKIPs (exit 77) when the private input is absent — never a
# pass. NOT part of CI; the evidence is the user's private title route only.
ATRAC3P_TITLE ?= place_game_here/EXTRACTED/PSP_GAME/USRDIR/data/sound/bgm/bgm_title.sgb

atrac3p-title-accept:
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(LDFLAGS) \
		-o $(BUILD_DIR)/atrac3p_title_accept.exe \
		src/rt/atrac3p_title_accept.c src/rt/atrac3p_bridge.c \
		$(ATRAC3P_SRCS) src/rt/vfpu_tables.c -lm
	$(BUILD_DIR)/atrac3p_title_accept.exe "$(ATRAC3P_TITLE)"

# vfpu-interp-selftest — executable regression for issue #184: quad/vector VFPU
# memory ops are all-or-nothing (span preflight before any lane commit, no prefix
# consumption on rejection), plus the vcrs width guard and vrot active-lane
# overlap scan. The white-box TU includes the real recomp.c/vfpu_tables.c/
# vfpu_interp.c (heap_selftest pattern); only scheduler/driver plumbing is
# stubbed. No game inputs or private data required.
vfpu-interp-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_interp_selftest.exe \
		src/rt/vfpu_interp_selftest.c src/rt/title_config.c $(LIBS)
	$(BUILD_DIR)/vfpu_interp_selftest.exe

# Canonical Allegrex/VFPU float-to-word fixed-vector regression. Expected
# results are explicit source-owned constants and the same vectors run under
# every available host rounding mode. CI adds the UBSan float-cast-overflow gate.
fp-convert-selftest:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/fp_convert_selftest.exe \
		src/rt/fp_convert_selftest.c -lm
	$(BUILD_DIR)/fp_convert_selftest.exe

# Production-backend regression for the historical repeated-adoption RAM runaway.
# This intentionally does not link HLE or define SR_CORO_LIFECYCLE_TEST: it proves
# the ordinary coroutine implementation itself keeps one stable main identity.
coro-selftest:
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/coro_selftest.exe \
		src/rt/sr_coro_selftest.c src/rt/sr_coro.c $(LDFLAGS) \
		$(if $(filter Windows_NT,$(OS)),-lpsapi,)
	$(BUILD_DIR)/coro_selftest.exe

# hle-thread-selftest — executable production-HLE ThreadMan/IoFileMgr regression suite.
# Links real hle.c, includes real sched.c for a controlled synthetic scheduler
# world. ThreadMan handlers use sr_syscall's registered-NID path; the focused
# IoFileMgr fixture calls test-only wrappers around the same production handlers.
# No game input is required. The PGD implementation is linked for the production
# HLE descriptor teardown path; unrelated host subsystems are supplied by narrow stubs.
# hle-thread-selftest-build -- compile and link the bounded production-HLE selftest.
#
# The executable is split from its run because this test previously exhausted host RAM
# (a joiner parked with a loop that re-adopted the main coroutine every iteration). The
# corrected lifecycle path is now bounded from the inside: `16fbb0a` made main-coroutine
# adoption idempotent and `1d8d494` added lifecycle counters that fail fast if the runaway
# pattern returns. The instrumentation macro is defined by this target alone; no production
# target compiles it, and the test refuses to build without it. The normal target is a safe,
# bounded runnable gate (45603 checks in the current measured run, of which 82 come from the
# issue #88 interrupt-context conformance matrix in src/rt/intr_conformance.h); the --psp-oracle
# sub-mode below
# remains available when only one scalar production-HLE stream is needed.
HLE_SELFTEST_DEFINES := -DSR_HLE_THREAD_SELFTEST -DSR_CORO_LIFECYCLE_TEST
# hle.c includes atrac3p_bridge.h and calls into the PR-B decode bridge, so any
# target that compiles it needs the same include paths and bridge/decoder
# sources the $(BUILD_DIR)/hle.o rule and `compile` already use. Without the
# -I flags this target does not even reach the linker: avcodec.h fails on
# libavutil/attributes.h.
hle-thread-selftest-build: $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -DSR_HLE_THREAD_SELFTEST -DSR_CORO_LIFECYCLE_TEST \
		$(HLE_INCLUDES) \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wno-unused-function \
		$(LDFLAGS) -Wl,--gc-sections -Wl,--no-insert-timestamp -o $(BUILD_DIR)/hle_thread_selftest.exe \
		src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/sr_coro.c src/rt/title_config.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c $(LIBS)

hle-thread-selftest: hle-thread-selftest-build
	$(BUILD_DIR)/hle_thread_selftest.exe

# Emit one scalar production-HLE comparison stream. Keep this target separate from the normal
# selftest so a deliberate FAIL remains visible in the record without turning the build recipe
# into a second standalone oracle implementation.
psp-oracle-nakagawa: hle-thread-selftest-build
	$(PYTHON) tools/psp_oracle/run_nakagawa.py --executable "$(BUILD_DIR)/hle_thread_selftest.exe" --output "$(PSP_ORACLE_OUTPUT)" -- --psp-oracle --case "$(PSP_ORACLE_CASE)" --artifact "$(BUILD_DIR)/hle_thread_selftest.exe" --source-commit "$(PSP_ORACLE_SOURCE_COMMIT)" --model "$(PSP_ORACLE_MODEL)" --firmware "$(PSP_ORACLE_FIRMWARE)"

psp-oracle: psp-oracle-nakagawa

# Production generated-code smoke stream.  The PSP-side ELF must be built from
# fixtures/psp_oracle with CASE=smoke first; this target then runs the normal
# codegen on that exact ELF and links the generated guest body into the existing
# production-HLE selftest executable.  No host-side sum implementation is used.
psp-oracle-nakagawa-smoke-generate: $(PSP_ORACLE_SMOKE_STAMP)

$(PSP_ORACLE_SMOKE_STAMP): $(PSP_ORACLE_SMOKE_ELF) tools/psp_oracle/build_nakagawa_smoke.py tools/codegen.py tools/analyze.py
	@if not exist "$(PSP_ORACLE_SMOKE_ELF)" (echo Missing $(PSP_ORACLE_SMOKE_ELF) ^& echo Build it with the documented PSPDEV fixture command first. ^& exit /b 2)
	$(PYTHON) tools/psp_oracle/build_nakagawa_smoke.py --elf "$(PSP_ORACLE_SMOKE_ELF)" --out-dir "$(PSP_ORACLE_SMOKE_DIR)"
	$(PYTHON) -c "from pathlib import Path; Path(r'$(PSP_ORACLE_SMOKE_STAMP)').write_text('generated\n', encoding='ascii')"

$(PSP_ORACLE_SMOKE_HEADER) $(PSP_ORACLE_SMOKE_CHUNK) $(PSP_ORACLE_SMOKE_ADAPTER): $(PSP_ORACLE_SMOKE_STAMP)

$(PSP_ORACLE_SMOKE_EXE): $(PSP_ORACLE_SMOKE_STAMP) $(PSP_ORACLE_SMOKE_HEADER) $(PSP_ORACLE_SMOKE_CHUNK) $(PSP_ORACLE_SMOKE_ADAPTER) src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/sr_coro.c $(PGD_BACKEND_SRC) $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(HLE_SELFTEST_DEFINES) $(HLE_INCLUDES) -DSR_PSP_ORACLE_SMOKE \
		-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables \
		-Wno-unused-function -w -I"$(PSP_ORACLE_SMOKE_DIR)" $(LDFLAGS) \
		-Wl,--gc-sections -Wl,--no-insert-timestamp -o "$(PSP_ORACLE_SMOKE_EXE)" \
		src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/sr_coro.c src/rt/title_config.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c \
		"$(PSP_ORACLE_SMOKE_DIR)/smoke_entry.c" "$(PSP_ORACLE_SMOKE_DIR)/smoke_recomp_0.c" $(LIBS)

psp-oracle-nakagawa-smoke-build: $(PSP_ORACLE_SMOKE_EXE)

psp-oracle-nakagawa-smoke: psp-oracle-nakagawa-smoke-build
	$(PYTHON) tools/psp_oracle/run_nakagawa.py --executable "$(PSP_ORACLE_SMOKE_EXE)" --output "$(PSP_ORACLE_SMOKE_OUTPUT)" -- --psp-oracle --case sum-1-to-100 --artifact "$(PSP_ORACLE_SMOKE_EXE)" --source-commit "$(PSP_ORACLE_SOURCE_COMMIT)" --model "$(PSP_ORACLE_MODEL)" --firmware "$(PSP_ORACLE_FIRMWARE)"

# dispatch-selftest — host-neutral unit tests for the guest code-address table (issue #45).
# No game inputs needed; compiles src/rt/dispatch_selftest.c against the real primitives in
# dispatch_table.h. Asserts that guest address 0 is a first-class key (register/look up,
# hash collisions involving 0 in both orders, L1 caching, re-registration), that a real
# function at address 0 executes while an unregistered lookup does not, and that occupancy
# is independent of the key. Exit code 0 = all invariants hold.
dispatch-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/dispatch_selftest.exe src/rt/dispatch_selftest.c
	$(BUILD_DIR)/dispatch_selftest.exe

# dispatch-isolation-selftest — executable proof that the two TYPED dispatch bindings a
# title configuration owns (dispatch aliases, callback terminators) act only where that
# configuration names them. No game inputs needed; src/rt/dispatch_isolation_selftest.c
# #includes recomp.c and drives the real dispatch() entry point, asserting its observable
# effect on CpuState.
#
# Same three-configuration matrix as sched-selftest, and for the same reason: the SAME
# dispatch source is built against a generic, a fixture-A and a fixture-B configuration,
# so a binding that leaked back into generic dispatch fails at least one of the three.
# The two fixtures carry deliberately DISJOINT synthetic address families, so neither can
# satisfy the other's expectations by coincidence. Exit code 0 = all invariants hold.
DISPATCH_ISO_CONFIGS := generic fixture-a fixture-b
DISPATCH_ISO_MANIFEST_generic :=
DISPATCH_ISO_MANIFEST_fixture-a := assets/titles/pspdev-phase5.json
DISPATCH_ISO_MANIFEST_fixture-b := assets/titles/synthetic.json

dispatch-isolation-selftest:
	@$(foreach cfg,$(DISPATCH_ISO_CONFIGS),$(MAKE) --no-print-directory dispatch-isolation-selftest-one DISPATCH_ISO_CONFIG=$(cfg) &&) true

# One configuration of the matrix. The generated header lands in its own directory so the
# three builds cannot share one.
DISPATCH_ISO_CONFIG ?= generic
DISPATCH_ISO_DIR := $(BUILD_DIR)/title-config/$(DISPATCH_ISO_CONFIG)
DISPATCH_ISO_MANIFEST := $(DISPATCH_ISO_MANIFEST_$(DISPATCH_ISO_CONFIG))
DISPATCH_ISO_CONFIG_ARG := $(if $(strip $(DISPATCH_ISO_MANIFEST)),--manifest $(strip $(DISPATCH_ISO_MANIFEST)),)

dispatch-isolation-selftest-one: $(TITLE_CONFIG_TOOL) tools/title_manifest.py
	$(PYTHON) $(TITLE_CONFIG_TOOL) $(DISPATCH_ISO_CONFIG_ARG) --output $(DISPATCH_ISO_DIR)/sr_title_config.h
	$(CC) $(CFLAGS) -I$(DISPATCH_ISO_DIR) $(LDFLAGS) \
		-o $(BUILD_DIR)/dispatch_isolation_selftest_$(DISPATCH_ISO_CONFIG).exe \
		src/rt/dispatch_isolation_selftest.c src/rt/title_config.c src/rt/vfpu_tables.c \
		$(LIBS) -lm
	$(BUILD_DIR)/dispatch_isolation_selftest_$(DISPATCH_ISO_CONFIG).exe

# asset-index-selftest — host-neutral dynamic extracted-data index regression (issue #223).
# The production Windows HLE supplies the path enumeration and wide I/O; this target proves the
# shared ownership/growth/sort/lookup core with a synthetic long host path and no game input.
asset-index-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/asset_index_selftest.exe src/rt/asset_index_selftest.c
	$(BUILD_DIR)/asset_index_selftest.exe

# gpu-coherence-selftest — Vulkan-backed production-path regression for CPU writes that
# overlap persistent GPU targets. The harness owns synthetic guest memory only; target
# acquire, dirty notification, reacquire, and readback all execute ge_gpu.c's real path.
gpu-coherence-selftest: shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -DSR_GPU_COHERENCE_SELFTEST -ffunction-sections -fdata-sections \
		$(LDFLAGS) -Wl,--gc-sections -o $(BUILD_DIR)/gpu_coherence_selftest.exe \
		src/rt/gpu_coherence_selftest.c src/rt/ge_capture.c $(RT_GE_O) src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c $(LIBS)
	$(BUILD_DIR)/gpu_coherence_selftest.exe

# gpu-snapsync-selftest — production-path regression for the explicit guest-VRAM
# snapshot boundary. It proves ordinary presentation remains async, then verifies
# target-scoped synchronization closes the generation gap and rejects unsafe geometry.
gpu-snapsync-selftest: shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -DSR_GPU_COHERENCE_SELFTEST -DSR_GPU_SNAPSHOT_SYNC_SELFTEST \
		-ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections \
		-o $(BUILD_DIR)/gpu_snapsync_selftest.exe \
		src/rt/gpu_coherence_selftest.c src/rt/ge_capture.c $(RT_GE_O) src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c $(LIBS)
	$(BUILD_DIR)/gpu_snapsync_selftest.exe

# gpu-capture-selftest — deterministic present-source capture regression (issue #57): the
# production present path is armed and driven with synthetic pixels; the published P6 PPMs
# are byte-checked (header, channel order, row pitch, no trailing bytes). Exit 77 = SKIP
# when Vulkan or the validation layer is unavailable.
gpu-capture-selftest: shader-verify
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections \
		$(LDFLAGS) -Wl,--gc-sections -o $(BUILD_DIR)/gpu_capture_selftest.exe \
		src/rt/gpu_capture_selftest.c src/rt/perf.c \
		$(SDL3VK_SRCS) $(LIBS)
	$(BUILD_DIR)/gpu_capture_selftest.exe

# Standalone seconds-scale GE fixture replay. Fixtures are private game-derived inputs and
# stay ignored; this target builds only the generic reader/rasterizer/backend executable.
ge-replay: shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections \
		-o $(BUILD_DIR)/ge_replay.exe \
		src/rt/ge_replay.c src/rt/ge_capture.c $(RT_GE_O) src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c $(LIBS)

# selftest — compile and run the C++ reference interpreter unit tests.
# Requires g++ with C++17. Exit code 0 = all tests passed.
selftest:
	g++ -std=c++17 -O1 -Isrc/ref -Isrc/rt -o $(BUILD_DIR)/selftest.exe \
		src/ref/selftest.cpp src/ref/interp.cpp src/ref/run_elf.cpp \
		-DSR_SELFTEST_ONLY -fno-exceptions
	$(BUILD_DIR)/selftest.exe
	@echo "selftest passed"

# vfpu_fuzz — build and run the VFPU differential fuzzer (generated codegen vs sr_vfpu_interp).
# Generates the per-game cases header from the ELF, then links a standalone harness that drives
# every distinct VFPU compute word through both paths on randomized register states. Depends on
# the runtime objects (not the `compile` link of hst.exe) because the generated bodies call
# sr_vread/sr_vwrite/sr_vfpu_* which live in recomp.c. driver.o is excluded since it owns the
# real program's main(); ge.o (RT_GE_O) is linked in because recomp.o references it.
VFPU_FUZZ_H := $(BUILD_DIR)/vfpu_fuzz_cases.h
VFPU_FUZZ_PREGENERATED ?= 0
ifeq ($(VFPU_FUZZ_PREGENERATED),1)
# CI/public mode: the caller generated a synthetic cases header explicitly.  Do not
# introduce a fake GAME_ELF dependency or regenerate from proprietary/private input.
$(VFPU_FUZZ_H):
	@test -f "$@" || (echo "missing pre-generated VFPU fuzz header: $@" >&2; exit 1)
else
$(VFPU_FUZZ_H): $(GAME_INPUT_PREREQ) tools/vfpu_fuzz_gen.py tools/analyze.py tools/codegen.py
	$(PYTHON) tools/vfpu_fuzz_gen.py --env-elf $(VFPU_FUZZ_H) --base=$(GAME_BASE) $(EXTRA_SPAN_ARG)
endif

$(BUILD_DIR)/vfpu_fuzz.o: src/rt/vfpu_fuzz.c $(VFPU_FUZZ_H) src/rt/recomp.h
	$(CC) -O0 -fno-strict-aliasing -Isrc/rt -I$(BUILD_DIR) -DSR_SDL3VK $(DEPFLAGS) -c src/rt/vfpu_fuzz.c -o $@

# Two-phase build (same rationale as `all`): `pipeline` must generate the chunk
# .c files before this target is parsed, or CHUNK_OBJS ($(wildcard)) resolves empty
# and the chunk objects are never compiled. Run codegen first, then build/link in a
# second make pass so the chunk objects are discovered.
vfpu_fuzz:
	$(MAKE) pipeline
	$(MAKE) vfpu_fuzz_build

# RT_OBJS carries hle.o, which calls into the PR-B ATRAC3+ decode bridge, so
# every target that links RT_OBJS must also link the bridge and the imported
# decoder TUs -- exactly as `compile` and `runtime-objects` do. Omitting them
# here is an undefined-reference link failure, not a smaller binary.
vfpu_fuzz_build: $(VFPU_FUZZ_H) $(BUILD_DIR)/vfpu_fuzz.o $(CHUNK_OBJS) $(RT_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_fuzz.exe \
		$(BUILD_DIR)/vfpu_fuzz.o \
		$(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(CHUNK_OBJS) \
		$(RT_GE_O) \
		$(filter-out $(BUILD_DIR)/driver.o,$(RT_OBJS)) \
		$(ATRAC3P_OBJS) \
		$(BUILD_DIR)/atrac3p_bridge.o \
		$(LIBS)
	$(BUILD_DIR)/vfpu_fuzz.exe

# psp-oracle-vfpu -- Nakagawa side of the VFPU transcendental hardware oracle
# (Loop A of docs/HARDWARE_ORACLE.md).  Deliberately game-independent: it drives the
# production sr_vfpu_* implementations in recomp.c over the shared input vector in
# fixtures/vfpu_oracle/vfpu_oracle_cases.h and emits the same record shape the PSP
# probe emits.  No GAME_ELF and no generated chunks are required; assets/vfpu/ is the
# only data input (override the directory with PSP_VFPU_TABLES).
PSP_VFPU_ORACLE_EXE := $(BUILD_DIR)/vfpu_oracle_host.exe

$(PSP_VFPU_ORACLE_EXE): src/rt/vfpu_oracle_host.c fixtures/vfpu_oracle/vfpu_oracle_cases.h $(RT_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o
	$(CC) $(CFLAGS) -Isrc/rt -Ifixtures/vfpu_oracle $(LDFLAGS) -o $@ 		src/rt/vfpu_oracle_host.c 		$(RT_GE_O) 		$(filter-out $(BUILD_DIR)/driver.o,$(RT_OBJS)) 		$(ATRAC3P_OBJS) 		$(BUILD_DIR)/atrac3p_bridge.o 		$(LIBS)

psp-oracle-vfpu-build: $(PSP_VFPU_ORACLE_EXE)

# Emit the Nakagawa stream.  All four provenance flags are required by the harness;
# unmeasured provenance must fail the acceptance gate rather than default to a
# placeholder.  Output goes to an ignored path.
PSP_VFPU_ORACLE_OUT ?= $(BUILD_DIR)/vfpu_oracle/nakagawa.stdout.txt
PSP_VFPU_ORACLE_MODEL ?= unknown
PSP_VFPU_ORACLE_FIRMWARE ?= unknown
PSP_VFPU_ORACLE_COMMIT ?= $(shell git rev-parse HEAD)

psp-oracle-vfpu: $(PSP_VFPU_ORACLE_EXE)
	$(PYTHON) -c "from pathlib import Path; Path(r'$(BUILD_DIR)/vfpu_oracle').mkdir(parents=True, exist_ok=True)"
	$(PYTHON) tools/psp_oracle/run_nakagawa_vfpu.py --executable "$(PSP_VFPU_ORACLE_EXE)" --output "$(PSP_VFPU_ORACLE_OUT)" -- --model "$(PSP_VFPU_ORACLE_MODEL)" --firmware "$(PSP_VFPU_ORACLE_FIRMWARE)" --source-commit "$(PSP_VFPU_ORACLE_COMMIT)"

# verify — differential smoke gates (no recompiler build needed; runs Python analysis tools
# and the host reference interpreter). These are differential tests: each compares the
# recompiler/reference output against a PPSSPP-captured oracle trace. The oracle inputs are
# external and supplied via the *ORACLE / *MODULE variables above.
#
# Correct gate signatures (see tools/codegen_gate.py and tools/microtest_gate.py):
#   codegen_gate.py   <elf> <oracle.trace> <workdir>
#   microtest_gate.py <run_elf.exe> <module.elf> <oracle.trace> <workdir>
#
# Usage: make verify GAME_NAME=hst GAME_ELF=eboot.elf GAME_BASE=0 GAME_ENTRY=0 \
#          CODEGEN_ORACLE=oracle/eboot.trace \
#          MICROTEST_MODULE=build/hst/microtest.elf MICROTEST_ORACLE=oracle/microtest.trace
verify: run_elf
	$(PYTHON) tools/verify_gates.py --cc "$(CC)" --env-elf \
		--run-elf "$(RUN_ELF_EXE)" --workdir "$(VERIFY_WORKDIR)" \
		--codegen-oracle "$(CODEGEN_ORACLE)" --microtest-module "$(MICROTEST_MODULE)" \
		--microtest-oracle "$(MICROTEST_ORACLE)"

# run_elf — host reference-interpreter driver used by microtest_gate. Built WITHOUT
# -DSR_SELFTEST_ONLY so run_elf.cpp's main() (ELF loader + trace driver) is included.
run_elf:
	$(CXX) -std=c++17 -O1 -Isrc/ref -Isrc/rt -o $(RUN_ELF_EXE) \
		src/ref/run_elf.cpp src/ref/interp.cpp -fno-exceptions

run: all
	./$(BUILD_DIR)/$(GAME_NAME).exe --image $(BUILD_DIR)/$(GAME_NAME)_image.bin $(GAME_BASE) $(GAME_ENTRY) none none --gui

# shaders — deterministic SPIR-V regeneration for the SDL3/Vulkan GPU backend.
# Normal builds verify source/embedding/manifest hashes without requiring glslc. The
# stricter shader-repro-verify target recompiles with glslc and compares byte-for-byte.
SHADER_DIR     := src/rt/gpu_sdl3vk
VERT_SHADER    := $(SHADER_DIR)/shaders/psp.vert
FRAG_SHADER    := $(SHADER_DIR)/shaders/psp.frag
VERT_SPV       := $(SHADER_DIR)/psp_vert.spv
FRAG_SPV       := $(SHADER_DIR)/psp_frag.spv
VERT_INC       := $(SHADER_DIR)/psp_vert.inc
FRAG_INC       := $(SHADER_DIR)/psp_frag.inc

shaders:
	$(PYTHON) tools/shader_embed.py regenerate --glslc "$(GLSLC)"

shader-verify:
	$(PYTHON) tools/shader_embed.py verify

shader-repro-verify:
	$(PYTHON) tools/shader_embed.py verify --recompile --glslc "$(GLSLC)"
