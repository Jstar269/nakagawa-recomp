# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Makefile for rebuilding / running a recompiled PSP game
#
# Usage:
#   mingw32-make GAME_NAME=mygame GAME_ELF=eboot.elf GAME_BASE=0x08804000 GAME_ENTRY=0x08804128
#

# Override these on the command line for your game (see README).
# NOTE: HST (flat-PRX image) requires GAME_BASE=0 GAME_ENTRY=0. The defaults below
# are for a generic rebased ELF. Using the wrong base → cascading label errors at link.
# Prefer nk_manager.ps1 (sets the correct values automatically).
GAME_NAME  ?= mygame
GAME_ELF   ?= eboot.elf
GAME_BASE  ?= 0x08804000
GAME_ENTRY ?= 0x08804000

# Goals that only print information. When every requested goal is one of these,
# parse-time work with side effects (profile stamps and their invalidation, build
# directories, Python bytecode) is skipped, so `make help` never touches the tree.
NK_INFO_ONLY_GOALS := help
NK_INFO_ONLY := $(if $(MAKECMDGOALS),$(if $(filter-out $(NK_INFO_ONLY_GOALS),$(MAKECMDGOALS)),,1),)


# ---------------------------------------------------------------------------
# GENERIC TITLE CONTRACT (title-neutral, host-portable):
#   GAME_NAME, GAME_ELF, GAME_BASE, GAME_ENTRY, GAME_EXTRA_ELFS, GAME_PSP_HEADER,
#   TITLE_EXTRA_SPANS (extra executable span, at most one), BUILD_DIR,
#   FUNCS_PER_CHUNK, CODEGEN_PROFILE_ARG, etc., are all derived from a validated
#   title manifest via tools/title_codegen_plan.py or TITLE_MANIFEST. Default
#   values below are for a generic rebased ELF; Make does not infer title data.
# ---------------------------------------------------------------------------
TITLE_EXTRA_SPANS ?=
export TITLE_EXTRA_SPANS

CODEGEN_PROFILE_ARG ?=
GAME_EXTRA_ELFS ?=
GAME_PSP_HEADER ?=
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
# GENERIC: effective span derives ONLY from TITLE_EXTRA_SPANS.
EFFECTIVE_EXTRA_SPANS := $(strip $(TITLE_EXTRA_SPANS))
EXTRA_SPAN_ARG  = $(if $(strip $(EFFECTIVE_EXTRA_SPANS)),--extra-span=$(strip $(EFFECTIVE_EXTRA_SPANS)),)

# GNU Make defines a built-in CC=cc with origin "default". A normal `CC ?= gcc`
# therefore never takes effect. Treat only that built-in/undefined state as unset,
# select the supported PATH-resolved UCRT64 GCC, and preserve environment or
# command-line overrides such as `CC=clang`.
ifneq ($(filter default undefined,$(origin CC)),)
CC := gcc
endif
PYTHON     ?= python
# Direct Make callers may provide VULKAN_SDK explicitly (or export it).
# If unset, discover dynamically via tools/vulkan_sdk.py.
ifeq ($(VULKAN_SDK),)
VULKAN_SDK := $(shell $(PYTHON) -c "import sys; sys.path.insert(0, 'tools'); from vulkan_sdk import discover_vulkan_sdk, VulkanSdkError; (lambda: exec('try:\n print(discover_vulkan_sdk().as_posix())\nexcept VulkanSdkError:\n pass'))()")
endif
# PowerShell commonly exports this with backslashes while nk_manager passes the
# same directory with slashes. Canonicalize before hashing CFLAGS so direct Make
# and manager builds do not churn otherwise identical runtime profiles.
VULKAN_SDK := $(subst \,/,$(VULKAN_SDK))
# glslc from the Vulkan SDK is used ONLY by the opt-in `shaders` target below.
GLSLC ?= glslc

# Vulkan include and library search paths. On Windows these come from the resolved SDK
# (see the VULKAN_SDK block above). On Linux there is no SDK directory by default: the
# loader is a system library (libvulkan.so) and the headers live under /usr/include, so the
# standard search paths already find them and no -I/-L is emitted. Forcing -I/Include -I/include
# on Linux reorders GCC's own header search and breaks #include_next, so the empty SDK is
# handled explicitly here rather than by leaving a dangling -I$(VULKAN_SDK)/Include.
#
# An explicit VULKAN_SDK override on Linux is still honoured: if the caller names a real
# directory, -L<root>/lib is added and the SDK's own headers are picked up. The default
# (no override, no discovery) is the system loader.
ifeq ($(OS),Windows_NT)
VULKAN_INC_FLAGS := -I$(VULKAN_SDK)/Include -I$(VULKAN_SDK)/include
VULKAN_LIB_FLAGS := -L$(VULKAN_SDK)/Lib -L$(VULKAN_SDK)/lib
else
VULKAN_INC_FLAGS := $(if $(strip $(VULKAN_SDK)),-I$(VULKAN_SDK)/include,)
VULKAN_LIB_FLAGS := $(if $(strip $(VULKAN_SDK)),-L$(VULKAN_SDK)/lib,) -lvulkan
endif

# SDL3 dependency discovery and isolation (issue #331).
# An explicit SDL3_DIR overrides discovery; otherwise the supported provider is
# found (MSYS2 UCRT64 on Windows, pkg-config/system elsewhere). One Python pass
# writes every SDL3 variable to a fragment, so a parse costs one interpreter
# start rather than one per variable. Info-only goals skip discovery entirely.
SDL3_DIR ?=
SDL3_MAKE_FRAGMENT := build/.sdl3-discovery.mk
ifeq ($(strip $(filter clean distclean clean-preview,$(MAKECMDGOALS))$(NK_INFO_ONLY)),)
_SDL3_DISCOVERY := $(shell $(PYTHON) -W ignore -c "import sys; sys.path.insert(0, 'tools'); from nk_doctor_checks import write_sdl3_make_fragment; write_sdl3_make_fragment(r'$(SDL3_MAKE_FRAGMENT)', r'$(subst \,/,$(SDL3_DIR))', r'$(CC)')" 2>&1)
ifneq ($(strip $(_SDL3_DISCOVERY)),)
$(error SDL3 discovery failed: $(_SDL3_DISCOVERY))
endif
include $(SDL3_MAKE_FRAGMENT)
else
SDL3_PROVIDER := none
SDL3_VERSION := none
endif

# Native runtime code is host-side C and can be tested independently.
# Direct Make remains conservative -O0/-O0. Validated private title adapters may
# request measured profile-specific values; explicit overrides remain supported.
RUNTIME_OPT ?= -O0
PERF_AOT_INSTRUCTIONS ?= 0
CFLAGS     ?= $(RUNTIME_OPT) -fno-strict-aliasing -Isrc/rt -Isrc/core $(SDL3_INC_FLAGS) $(VULKAN_INC_FLAGS) -DSR_SDL3VK -D_CRT_SECURE_NO_WARNINGS -Wall -Wextra
override CFLAGS += -DSR_FLIGHT_RECORDER_LINKED -DPERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)
# The extracted HST archive tree has a title-specific extracted-data census
# (HST: 56,672 files). The generic build has no census (0 = disabled); a
# title-configured build carries the expectation via runtime_bindings
# (expected_data_file_count) validated from the title manifest. See
# tools/title_manifest.py, tools/title_runtime_config.py and docs/PORTING.md C-2.
LDFLAGS ?= $(SDL3_LDFLAGS) $(if $(VULKAN_SDK),-L$(VULKAN_SDK)/Lib -L$(VULKAN_SDK)/lib,)
# Optional per-package linker map. Kept separate from LDFLAGS so package builds
# can request it without replacing the SDK/library search paths.
LINK_MAP ?=
LINK_MAP_VALUE = -Wl,-Map,$(LINK_MAP)
LINK_MAP_ARG = $(if $(strip $(LINK_MAP)),$(LINK_MAP_VALUE),)
# DirectInput (-ldinput8 -ldxguid) removed: gui.c controller input is now handled entirely by
# the SDL3 gamepad subsystem (src/rt/gpu_sdl3vk). -lole32 stays (Media Foundation, h264_mf.c);
# -lwinmm stays (sched.c timeBeginPeriod); -lgdi32 stays (GDI fallback presenter).
# The Windows-only group is split out so a non-Windows link drops the members that have no
# Linux equivalent. h264_mf.c (mfplat/ole32) and gui.c (gdi32) are themselves #ifdef _WIN32,
# so on Linux they contribute no undefined symbols; -lwinmm is dead weight on every platform
# (no timeBeginPeriod/timeEndPeriod call exists in the tree) but is kept on Windows for
# byte-identical output.
# The Vulkan import library name differs by platform: vulkan-1.lib (Windows SDK) vs libvulkan.so
# (Linux loader). The two are NOT interchangeable, so the name is chosen per platform rather
# than unified to one.
ifeq ($(OS),Windows_NT)
VULKAN_LIB_NAME := -lvulkan-1
WIN_ONLY_LIBS   := -lmfplat -lgdi32 -lole32 -lwinmm
else
VULKAN_LIB_NAME := -lvulkan
WIN_ONLY_LIBS   :=
endif
LIBS       ?= -lSDL3 $(VULKAN_LIB_NAME) $(WIN_ONLY_LIBS)

BUILD_DIR  ?= build/$(GAME_NAME)
# The runtime's diagnostic exit artifacts (crash dump, exit flag) belong to the build
# that produced them, not to a fixed title. Without this the runtime wrote them to a
# literal build/hst/, so every non-HST build either scribbled into another title's
# output directory or silently dropped the dump when that directory did not exist.
# Deferred on purpose: a recursive $(MAKE) BUILD_DIR=... override retargets it too.
# `override` on purpose as well: a plain += is discarded when a caller passes CFLAGS on the
# command line, and hle.c would then fall back to a literal build/hst/ and silently restore
# the cross-title overwrite this define exists to prevent. Failing quietly there is worse
# than ignoring a custom flag, so this one define is not caller-overridable.
override CFLAGS += -DSR_BUILD_DIR=\"$(BUILD_DIR)\"
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

# Public, source-owned display/presentation smoke. Same two-phase `all` path as
# the production smoke, but the guest fills the framebuffer and flips it once a
# frame, so the gate covers sceDisplaySetFrameBuf -> vblank -> present rather
# than a compute sentinel. This is the only public-scope title the native player
# can launch, which is why it also carries a manifest under assets/titles.
# The build directory and artifact stem are deliberately the TITLE ID, not a
# prettier name: src/core/nk_launch.c resolves a launch by probing
# build/<title_id>/<title_id>.exe and build/<title_id>/<title_id>_image.bin.
# Naming the build anything else is why no other fixture in this tree can be
# launched from the native player. Keep these three strings equal to the "id"
# field of assets/titles/display-smoke.json.
DISPLAY_SMOKE_NAME      := display-smoke-v1
DISPLAY_SMOKE_DIR       := build/$(DISPLAY_SMOKE_NAME)
DISPLAY_SMOKE_FIXTURE   := $(DISPLAY_SMOKE_DIR)/fixture
DISPLAY_SMOKE_GENERATOR := fixtures/display_smoke/generate.py
DISPLAY_SMOKE_PRX       := $(DISPLAY_SMOKE_FIXTURE)/guest.prx
DISPLAY_SMOKE_PSP       := $(DISPLAY_SMOKE_FIXTURE)/guest.psp
DISPLAY_SMOKE_BASE      := 0x08810000
# The gate runs a short pattern; the launchable/demo build runs long enough to
# watch. Both are the same guest recipe with a different frame count.
DISPLAY_SMOKE_FRAMES      := 60
DISPLAY_SMOKE_DEMO_FRAMES := 1800

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

# SR_NAN_TRAP: build-time NaN/Inf origin diagnostic (issue #69). Off by default
# and zero cost when off. `make NAN_TRAP=1` turns on BOTH halves at once:
#   --nan-trap makes tools/codegen.py follow every generated FPU/VFPU result
#                 write with an SR_NAN_TRAP_* check, and
#   -DSR_NAN_TRAP  makes those checks live in the generated chunks and in the
#                 runtime/interpreter (src/rt/recomp.h, src/rt/debug.c,
#                 src/rt/guest_interp.c, src/rt/vfpu_interp.c).
# Splitting them is possible but is a footgun: codegen alone emits calls to a
# macro that then expands to ((void)0), so the build looks correct and reports
# nothing. Both halves are carried into the codegen and recompiler profile
# hashes, so flipping the option regenerates rather than reusing stale objects.
# At run time SR_NAN_TRAP_LIMIT (default 20) bounds how many reports print.
NAN_TRAP ?= 0
NAN_TRAP_CFLAG :=
ifeq ($(NAN_TRAP),1)
NAN_TRAP_CFLAG := -DSR_NAN_TRAP
CODEGEN_USER_ARGS += --nan-trap
override CFLAGS += -DSR_NAN_TRAP
endif

# A filtered public candidate uses the project-authored PGF reader and the
# fail-closed PGD/amctrl backend. Full private checkouts default to their local
# backends; candidate trees use only sources admitted to the public profile.
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
PGF_BACKEND_SRC := src/rt/pgf_public.c
PGD_BACKEND_SRC := src/rt/pgd_unavailable.c
ISO_BACKEND_SRC := src/rt/iso_public.c
AUDIO_BACKEND_SRC := src/rt/audio_unavailable.c
ASSET_COPY_ARGS := -ExcludeOptionalFonts
# override for the same reason as SR_BUILD_DIR above, and because it is now required:
# once any append to CFLAGS uses override, GNU make ignores later ordinary assignments to
# it. Losing this define is the worse failure of the two -- a PUBLIC_SAFE build would stop
# declaring itself, and the production-smoke evidence check fails on exactly that.
override CFLAGS += -DSR_PUBLIC_SAFE
endif

include mk/build_common.mk

BUILD_PROFILE_TOOL := tools/build_profile.py
CPU_STATE_ABI_HEADER := src/rt/recomp.h

# Runtime title configuration. The compiled runtime carries no title identity of its
# own: tools/title_runtime_config.py turns a *validated* title manifest's optional
# runtime_bindings block into a build-local header that only src/rt/title_config.c
# includes. With TITLE_MANIFEST empty the generator emits the generic configuration in
# which every optional binding is disabled -- so `make runtime-objects` needs no game
# input at all, and no default build inherits any title's addresses.
#
# HST binds its real values through the local title manifest
# (assets/titles/hst-ucus98701.json, supplied by nk_manager.ps1 -TitleManifest or by
# TITLE_MANIFEST= on a direct Make command line). That file is intentionally never
# checked in and is publication-excluded, with a .gitignore accident guard.
# They are deliberately not encoded here.
TITLE_MANIFEST ?=
TITLE_CONFIG_TOOL := tools/title_runtime_config.py
TITLE_CONFIG_DIR ?= $(BUILD_DIR)
TITLE_CONFIG_HEADER := $(TITLE_CONFIG_DIR)/sr_title_config.h
TITLE_CONFIG_ARG := $(if $(strip $(TITLE_MANIFEST)),--manifest $(strip $(TITLE_MANIFEST)),)
# Identity of the effective configuration. Bound into RUNTIME_PROFILE_HASH below so a
# changed title binding invalidates stale runtime objects instead of relinking silently.
ifdef NK_INFO_ONLY
TITLE_CONFIG_DIGEST := info-only
else
TITLE_CONFIG_DIGEST := $(shell $(PYTHON) $(TITLE_CONFIG_TOOL) $(TITLE_CONFIG_ARG) --print-digest)
endif
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
	$(error GAME_NAME=hst needs a validated title manifest: pass TITLE_MANIFEST=<path> or use nk_manager.ps1 -TitleManifest. Direct Make does not infer HST defaults; generic builds need no manifest when they use a different GAME_NAME.)
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
# Use Python for fully portable directory creation across Windows cmd.exe, MSYS2,
# PowerShell, and POSIX environments. A clean preview must not create the state
# it is about to plan over, so it skips this parse-time mkdir entirely.
ifeq ($(strip $(filter clean-preview,$(MAKECMDGOALS))),)
ifndef NK_INFO_ONLY
_MKDIRS := $(shell $(PYTHON) -c "import os, sys; [os.makedirs(d, exist_ok=True) for d in sys.argv[1:]]" "$(BUILD_DIR)" "$(BUILD_DIR)/portable-core" $(ATRAC3P_OBJ_DIRS))
endif
endif

ifeq ($(OS),Windows_NT)
PLAYER_PLATFORM_SRC := src/core/nk_platform_win32.c
PLAYER_EXTRA_LIBS   := -lshell32
PLAYER_PLAT_SOURCES := $(PLAYER_PLATFORM_SRC)
# The player link consumes the Vulkan import library, so like CFLAGS/LDFLAGS it
# must derive from the shared VULKAN_SDK resolution above (explicit override,
# environment, then tools/vulkan_sdk.py discovery) rather than naming one
# machine's SDK install. The player-vulkan-check order-only prerequisite below
# fails closed with the one variable to set when discovery found nothing,
# instead of a hardcoded fallback or a confusing compiler error.
#
# On Windows the SDK's -L path is already in LDFLAGS and the import library is
# in LIBS, so PLAYER_VULKAN_LIB stays empty and the link line is byte-identical
# to the pre-Linux-edit form. On Linux there is no SDK directory, so the Vulkan
# group is carried here instead.
# PLAYER_VULKAN_INC is spelled out literally on Windows so the Makefile-wiring
# test can assert it derives from $(VULKAN_SDK) rather than an intermediate
# variable; on Linux it is empty by default and only set when the caller names
# an explicit VULKAN_SDK.
PLAYER_VULKAN_INC   := -I$(VULKAN_SDK)/Include -I$(VULKAN_SDK)/include
PLAYER_VULKAN_LIB   :=
EXE_EXT             := .exe
else
PLAYER_PLATFORM_SRC := src/core/nk_platform_posix.c
PLAYER_EXTRA_LIBS   :=
PLAYER_PLAT_SOURCES := $(PLAYER_PLATFORM_SRC)
PLAYER_VULKAN_INC   := $(VULKAN_INC_FLAGS)
PLAYER_VULKAN_LIB   :=
EXE_EXT             :=
endif

RT_GE_O    := $(BUILD_DIR)/ge.o
RT_SRCS    := src/rt/recomp.c \
              src/rt/flight_recorder.c \
              src/rt/cpu_lle.c \
              src/rt/domain_mode.c \
              src/rt/nested_frames.c \
              src/rt/stale_code.c \
              src/rt/guest_interp.c \
              src/rt/title_config.c \
              src/rt/vfpu_tables.c \
              src/rt/archive_vfs.c \
              src/rt/debug.c \
              src/rt/watchpoints_file.c \
              src/rt/guest_printf.c \
              src/rt/perf.c \
              src/rt/fbcap_policy.c \
              src/rt/ge_capture.c \
              src/rt/vfpu_interp.c \
              src/rt/hle.c \
              src/core/nk_xb.c \
              src/rt/hle_power.c \
              src/rt/prx_loader.c \
              src/rt/sched.c \
              src/rt/sr_coro.c \
              $(ISO_BACKEND_SRC) \
              $(PGD_BACKEND_SRC) \
              src/rt/mpeg.c \
              src/rt/psmf_producer.c \
              $(PGF_BACKEND_SRC) \
              src/rt/gui.c \
              $(AUDIO_BACKEND_SRC) \
              src/rt/h264_mf.c \
              src/rt/h264_null.c \
              src/rt/savedata.c \
              src/rt/osk_win.c \
              src/rt/driver.c \
              src/rt/gpu_sdl3vk/sdl3vk.c \
              src/rt/gpu_sdl3vk/ge_gpu.c \
              src/core/nk_input_profile.c \
              src/core/nk_json.c \
              $(PLAYER_PLATFORM_SRC)

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
SDL3VK_SRCS := src/rt/gpu_sdl3vk/sdl3vk.c src/rt/fbcap_policy.c \
               src/core/nk_input_profile.c src/core/nk_json.c $(PLAYER_PLATFORM_SRC)

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
                      src/rt/flight_recorder.c \
                      src/rt/cpu_lle.c \
                      src/rt/domain_mode.c \
                      src/rt/nested_frames.c \
                      src/rt/stale_code.c \
                      src/rt/guest_interp.c \
                      src/rt/title_config.c \
                      src/rt/vfpu_tables.c \
                      src/rt/archive_vfs.c \
                      src/rt/debug.c \
                      src/rt/watchpoints_file.c \
                      src/rt/guest_printf.c \
                      src/rt/perf.c \
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
PORTABLE_CORE_CFLAGS ?= -D_GNU_SOURCE -std=c11 -O0 -fno-strict-aliasing -Isrc/rt -Isrc/core -Wall -Wextra -Werror=format
override PORTABLE_CORE_CFLAGS += -DSR_FLIGHT_RECORDER_LINKED

# Reproducible build identity for the flight recorder's build block
# (src/rt/flight_recorder.c, assets/flight_recorder_schema.json). The old
# __DATE__/__TIME__ stamp was the one project-owned difference between two
# builds of the same package from identical inputs; it is gone. The identity is
# transported the way this Makefile already passes build identity into the
# compile: a -D define (compare -DSR_BUILD_DIR above).
#   * SOURCE_DATE_EPOCH, when the build sets it, is honoured as-is (the
#     reproducible-builds.org convention) and recorded as build.source_date_epoch.
#   * The source commit is recorded as build.build_id, resolved exactly like
#     PSP_ORACLE_SOURCE_COMMIT below (git rev-parse HEAD). A source drop without
#     git and without SOURCE_DATE_EPOCH records no identity and tools/flight_diff.py
#     refuses such a bundle fail closed; it never falls back to a clock.
# The runtime profile hash is deliberately NOT the identity here: it hashes
# CFLAGS, which carries -DSR_BUILD_DIR and the SDK paths, so it varies with the
# output root and would reintroduce per-root variation into every binary. The
# identity defines are kept out of the global CFLAGS (and so out of
# RUNTIME_PROFILE_HASH): only flight_recorder.o embeds them, so a new commit
# rebuilds that one object instead of every runtime object (see
# FLIGHT_IDENTITY_STAMP below).
ifndef NK_INFO_ONLY
SR_SOURCE_COMMIT ?= $(shell git rev-parse HEAD)
SR_SOURCE_COMMIT := $(strip $(SR_SOURCE_COMMIT))
ifneq ($(strip $(SR_SOURCE_COMMIT)),)
FLIGHT_IDENTITY_DEFS += -DSR_BUILD_ID=\"$(SR_SOURCE_COMMIT)\"
endif
ifneq ($(strip $(SOURCE_DATE_EPOCH)),)
# Transported through the environment (see the guest-input transport above) so
# no operator value reaches a command interpreter as syntax. Fail closed on a
# non-decimal value: flight_recorder.c emits it as a JSON number.
export SOURCE_DATE_EPOCH
SR_SOURCE_DATE_EPOCH := $(shell $(PYTHON) -c "import os; v = os.environ.get('SOURCE_DATE_EPOCH', ''); print(v if v.isdigit() else '')")
ifeq ($(strip $(SR_SOURCE_DATE_EPOCH)),)
$(error SOURCE_DATE_EPOCH must be a decimal Unix timestamp (reproducible-builds.org), got "$(SOURCE_DATE_EPOCH)")
endif
FLIGHT_IDENTITY_DEFS += -DSR_SOURCE_DATE_EPOCH=$(SR_SOURCE_DATE_EPOCH)
endif
endif

# Public targets are listed once so `make help` and phony-target behaviour cannot
# drift apart.  FORCE is intentionally separate: it is an implementation detail,
# not an entry-point a contributor should discover by accident.
PUBLIC_TARGETS := \
	help \
	check \
	contrib-check \
	test \
	native-core-tests \
	player-ui-tests \
	player-ui-regressions \
	fuzz-parsers \
	readiness \
	provenance-refresh \
	all \
	pipeline \
	compile \
	compiler-info \
	runtime-objects \
	portable-core-objects \
	atrac3p-objects \
	player \
	public-safe-verify \
	production-smoke \
	production-smoke-staged \
	production-smoke-clean \
	production-smoke-gap \
	perf-benchmark \
	showcase \
	showcase-smoke \
	display-smoke \
	display-smoke-run \
	display-smoke-gui \
	display-smoke-player \
	display-smoke-clean \
	production-smoke-gap-clean \
	platform-ladder \
	platform-ladder-zero \
	platform-ladder-reloc \
	platform-ladder-gap \
	platform-ladder-sched \
	platform-ladder-fpu \
	platform-ladder-fs \
	platform-ladder-fs-negative \
	platform-ladder-title2 \
	platform-ladder-title2-negative \
	platform-ladder-clean \
	cosim-selftest \
	cosim-selftest-run \
	cosim-selftest-clean \
	cosim-mutants \
	clean \
	clean-fixtures \
	tidy \
	distclean \
	clean-all \
	clean-preview \
	verify \
	selftest \
	strbuf-selftest \
	sched-selftest \
	sched-selftest-one \
	heap-selftest \
	profiler-selftest \
	coro-selftest \
	hle-thread-selftest \
	hle-thread-selftest-build \
	hle-title-selftest \
	hle-title-selftest-one \
	dispatch-selftest \
	stale-code-selftest \
	cpu-lle-selftest \
	domain-mode-selftest \
	dispatch-isolation-selftest \
	dispatch-isolation-selftest-one \
	asset-index-selftest \
	fp-convert-selftest \
	vfpu-tables-selftest \
	watchpoints-file-selftest \
	vfpu-interp-selftest \
	atrac3p-selftest \
	atrac3p-bridge-selftest \
	psmf-producer-selftest \
	psmf-media-selftest \
	audio-selftest \
	atrac3p-title-accept \
	gpu-coherence-selftest \
	gpu-snapsync-selftest \
	ge-replay \
	run \
	run_elf \
	vfpu_fuzz \
	vfpu_fuzz_build \
	shaders \
	shader-verify \
	shader-repro-verify \
	psp-oracle \
	psp-oracle-nakagawa \
	psp-oracle-vfpu \
	psp-oracle-vfpu-build \
	psp-oracle-nakagawa-smoke \
	psp-oracle-nakagawa-smoke-build \
	psp-oracle-nakagawa-smoke-generate \
	gpu-capture-selftest

INTERNAL_TARGETS := FORCE player-vulkan-check player-state-test-bin input-settings-test-bin package-builder-test-bin sdl3-check vfpu_fuzz_validate_synthetic
.PHONY: $(PUBLIC_TARGETS) $(INTERNAL_TARGETS)

HELP_DESCRIPTION_help := list every public Make target and its purpose
HELP_DESCRIPTION_check := run public-safe docs, policy, audit, native, and fast checks
HELP_DESCRIPTION_contrib-check := run only the local gates that apply to your changed files
HELP_DESCRIPTION_test := run the complete Python tooling test suite
HELP_DESCRIPTION_native-core-tests := build and run host-side native core tests
HELP_DESCRIPTION_fuzz-parsers := run bounded native parser mutation fuzzing
HELP_DESCRIPTION_readiness := run the strict pre-PR gate with external authority
HELP_DESCRIPTION_provenance-refresh := refresh controls through hosted generation and stage them
HELP_DESCRIPTION_all := generate and compile the current title runtime
HELP_DESCRIPTION_pipeline := generate image, imports, and recomputed source artifacts
HELP_DESCRIPTION_compile := compile and link the generated runtime
HELP_DESCRIPTION_compiler-info := print the effective compiler and build settings
HELP_DESCRIPTION_runtime-objects := build runtime and decoder objects
HELP_DESCRIPTION_portable-core-objects := build host-neutral runtime objects
HELP_DESCRIPTION_atrac3p-objects := build ATRAC3+ decoder objects
HELP_DESCRIPTION_player := build the native player
HELP_DESCRIPTION_player-ui-tests := build and run native player UI tests (needs SDL3)
HELP_DESCRIPTION_player-ui-regressions := run scripted native player UI event and recovery flows (needs SDL3)
HELP_DESCRIPTION_public-safe-verify := build public-safe host-neutral core objects
HELP_DESCRIPTION_production-smoke := run the public production-composition smoke test
HELP_DESCRIPTION_production-smoke-staged := run the production smoke from a staging directory outside the build tree
HELP_DESCRIPTION_production-smoke-clean := remove production smoke artifacts
HELP_DESCRIPTION_production-smoke-gap := run the public AOT-gap dispatch smoke test
HELP_DESCRIPTION_perf-benchmark := run the public source-owned SR_PERF benchmark matrix and overhead check
HELP_DESCRIPTION_showcase := build packaged source-owned PSP showcase demos (requires PSPDEV)
HELP_DESCRIPTION_showcase-smoke := run bundled demos headlessly with telemetry checks
HELP_DESCRIPTION_display-smoke := build the display smoke fixture
HELP_DESCRIPTION_display-smoke-run := run the display smoke fixture
HELP_DESCRIPTION_display-smoke-gui := run the display smoke with its GUI
HELP_DESCRIPTION_display-smoke-player := build the player and run display smoke
HELP_DESCRIPTION_display-smoke-clean := remove display smoke artifacts
HELP_DESCRIPTION_production-smoke-gap-clean := remove AOT-gap smoke artifacts
HELP_DESCRIPTION_platform-ladder := run the complete public platform ladder
HELP_DESCRIPTION_platform-ladder-zero := run the zero-base platform fixture
HELP_DESCRIPTION_platform-ladder-reloc := run the relocation platform fixture
HELP_DESCRIPTION_platform-ladder-gap := run the interpreter-gap platform fixture
HELP_DESCRIPTION_platform-ladder-sched := run the scheduler platform fixture
HELP_DESCRIPTION_platform-ladder-fpu := run the FPU platform fixture
HELP_DESCRIPTION_platform-ladder-fs := run the filesystem platform fixture
HELP_DESCRIPTION_platform-ladder-fs-negative := run the negative filesystem fixture
HELP_DESCRIPTION_platform-ladder-title2 := run the second-title platform fixture
HELP_DESCRIPTION_platform-ladder-title2-negative := run the negative second-title fixture
HELP_DESCRIPTION_platform-ladder-clean := remove platform-ladder artifacts
HELP_DESCRIPTION_cosim-selftest := run the source-owned AOT/interpreter cosimulation
HELP_DESCRIPTION_cosim-selftest-run := build and run the cosimulation harness
HELP_DESCRIPTION_cosim-selftest-clean := remove cosimulation artifacts
HELP_DESCRIPTION_cosim-mutants := run the cosimulation negative corpus
HELP_DESCRIPTION_clean := remove current-title build outputs
HELP_DESCRIPTION_clean-fixtures := remove smoke, cosimulation, and oracle artifacts
HELP_DESCRIPTION_tidy := remove intermediates while preserving linked binaries
HELP_DESCRIPTION_distclean := remove intermediates and ephemeral build logs
HELP_DESCRIPTION_clean-all := remove all build and fixture outputs
HELP_DESCRIPTION_clean-preview := preview allowlisted workspace clean (CONFIRM=1 to apply)
HELP_DESCRIPTION_verify := compare generated output against external oracle data
HELP_DESCRIPTION_selftest := run the runtime selftest
HELP_DESCRIPTION_strbuf-selftest := run the checked-formatting selftest
HELP_DESCRIPTION_sched-selftest := run the scheduler selftest suite
HELP_DESCRIPTION_sched-selftest-one := run one scheduler selftest build
HELP_DESCRIPTION_heap-selftest := run the heap and allocator selftest
HELP_DESCRIPTION_profiler-selftest := run the profiler selftest
HELP_DESCRIPTION_coro-selftest := run the coroutine selftest
HELP_DESCRIPTION_hle-thread-selftest := build and run the HLE thread selftest
HELP_DESCRIPTION_hle-thread-selftest-build := build the HLE thread selftest only
HELP_DESCRIPTION_hle-title-selftest := run title-configured HLE selftests
HELP_DESCRIPTION_hle-title-selftest-one := run one title-configured HLE selftest
HELP_DESCRIPTION_dispatch-selftest := run the production dispatch selftest
HELP_DESCRIPTION_stale-code-selftest := run the stale translated-code detector selftest
HELP_DESCRIPTION_cpu-lle-selftest := run the LLE COP0/exception interpreter selftest
HELP_DESCRIPTION_domain-mode-selftest := run the LLE domain-mode and import-seam selftest
HELP_DESCRIPTION_dispatch-isolation-selftest := run dispatch isolation selftests
HELP_DESCRIPTION_dispatch-isolation-selftest-one := run one dispatch isolation selftest
HELP_DESCRIPTION_asset-index-selftest := run the asset-index selftest
HELP_DESCRIPTION_fp-convert-selftest := run the FPU conversion selftest
HELP_DESCRIPTION_vfpu-tables-selftest := run the VFPU table-loader selftest
HELP_DESCRIPTION_watchpoints-file-selftest := run the watchpoints-file selftest
HELP_DESCRIPTION_vfpu-interp-selftest := run the VFPU interpreter selftest
HELP_DESCRIPTION_atrac3p-selftest := run the ATRAC3+ decoder selftest
HELP_DESCRIPTION_atrac3p-bridge-selftest := run the ATRAC3+ HLE bridge selftest
HELP_DESCRIPTION_psmf-producer-selftest := run the source-owned bounded PSMF producer selftest
HELP_DESCRIPTION_psmf-media-selftest := run the source-owned PSMF-to-decoder media selftest
HELP_DESCRIPTION_audio-selftest := run the SDL3 host audio output selftest
HELP_DESCRIPTION_atrac3p-title-accept := run the optional ATRAC3+ title acceptance route
HELP_DESCRIPTION_gpu-coherence-selftest := run the GPU coherence selftest
HELP_DESCRIPTION_gpu-snapsync-selftest := run the GPU snapshot-sync selftest
HELP_DESCRIPTION_ge-replay := run the graphics-engine replay selftest
HELP_DESCRIPTION_run := build and launch the current title runtime
HELP_DESCRIPTION_run_elf := build the reference interpreter runner
HELP_DESCRIPTION_vfpu_fuzz := build and run the VFPU differential fuzzer
HELP_DESCRIPTION_vfpu_fuzz_build := build the VFPU differential fuzzer only
HELP_DESCRIPTION_shaders := regenerate embedded shader artifacts
HELP_DESCRIPTION_shader-verify := verify embedded shader hashes
HELP_DESCRIPTION_shader-repro-verify := recompile and compare shader bytes
HELP_DESCRIPTION_psp-oracle := run the scalar Nakagawa PSP oracle stream
HELP_DESCRIPTION_psp-oracle-nakagawa := run the scalar host and PSP comparison helper
HELP_DESCRIPTION_psp-oracle-vfpu := emit the host VFPU oracle stream
HELP_DESCRIPTION_psp-oracle-vfpu-build := build the host VFPU oracle
HELP_DESCRIPTION_psp-oracle-nakagawa-smoke := run the generated-code PSP oracle smoke
HELP_DESCRIPTION_psp-oracle-nakagawa-smoke-build := build the generated-code PSP oracle smoke
HELP_DESCRIPTION_psp-oracle-nakagawa-smoke-generate := generate the PSP oracle smoke artifacts
HELP_DESCRIPTION_gpu-capture-selftest := run the GPU capture selftest

.SECONDARY:

help:
	$(info Nakagawa Recomp Make targets)
	$(info Usage: mingw32-make [VARIABLE=value ...] TARGET)
	$(info )
	$(foreach target,$(PUBLIC_TARGETS),$(info   $(target) - $(HELP_DESCRIPTION_$(target))))
	@:

# Stable diagnostic surface for CI and local setup checks. This target performs no
# compilation and makes GNU Make's selected compiler and assignment origin explicit.
compiler-info:
	@echo CC=$(CC)
	@echo CC_ORIGIN=$(origin CC)
	@echo RUNTIME_OPT=$(RUNTIME_OPT)
	@echo CFLAGS=$(CFLAGS)
	@echo RECOMP_OPT=$(RECOMP_OPT)
	@echo RECOMP_FLAGS=$(RECOMP_FLAGS)
	@echo PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)
	@echo FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)
	@echo CHUNK_TARGET_BYTES=$(CHUNK_TARGET_BYTES)
	@echo PUBLIC_SAFE=$(PUBLIC_SAFE)
	@echo SDL3_DIR=$(SDL3_DIR)
	@echo SDL3_PROVIDER=$(SDL3_PROVIDER)
	@echo SDL3_VERSION=$(SDL3_VERSION)

# One command for the whole pre-pull-request checklist in docs/CI.md.
#
# The checklist has been correct and documented for a while and is still
# routinely half-run: the usual outcome is that publish_audit is run, reported
# as "gates green", and provenance_attest_verify -- the gate that actually
# blocks the merge -- is never run at all. They answer different questions.
# publish_audit compares the candidate against its own checked-in ledger, so the
# candidate supplies both sides and an unapproved blob cannot be detected.
# provenance_attest_verify compares it against the external private authority,
# which is the only thing that emits BLOB_UNAPPROVED. A tree can be
# "publication audit: OK" and "verdict: FAIL" at the same commit.
#
# Ordered cheapest-first so a policy or ledger mistake surfaces in seconds
# rather than after the suite. Every step is fatal; make stops at the first one.
#
#   NK_TRUSTED_LEDGER=/path/to/IMPLEMENTATION_PROVENANCE.json make readiness
#
# NK_TRUSTED_LEDGER is the detailed development ledger from the private history
# repository. It is deliberately required rather than defaulted: skipping the
# attestation step when the path is absent is exactly the silent pass this
# target exists to prevent, so an unset value is BLOCKED, not "not applicable".
READINESS_BASE ?= $(shell git merge-base origin/main HEAD)
READINESS_WORKDIR ?= $(CURDIR)/../.nk-readiness-verify

readiness:
ifndef NK_TRUSTED_LEDGER
	@echo "readiness: BLOCKED -- NK_TRUSTED_LEDGER is unset."
	@echo "  It must name the detailed development ledger"
	@echo "  (docs/provenance/IMPLEMENTATION_PROVENANCE.json in the private"
	@echo "  history repository). Without it the attestation gate cannot run,"
	@echo "  and an unrun gate is BLOCKED evidence, never a pass."
	@exit 1
endif
	@echo "== readiness: base $(READINESS_BASE)"
	$(PYTHON) tools/policy_sync.py
	$(PYTHON) tools/lint_docs.py
	$(PYTHON) tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
	$(PYTHON) tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
	$(PYTHON) tools/provenance_attest_verify.py --repo . --candidate $(shell git rev-parse HEAD) --base $(READINESS_BASE) --require-immutable-revisions --trusted-ledger "$(NK_TRUSTED_LEDGER)" --workdir "$(READINESS_WORKDIR)"
	git diff --check $(READINESS_BASE)..HEAD
	@echo "== readiness: control-file completeness"
	$(PYTHON) tools/policy_sync.py --regen-export
	git diff --quiet -- PUBLIC_EXPORT.json assets/public_provenance_ledger.json assets/public_source_profile.json || { echo "readiness: FAIL -- regenerating the control files changed them, so the commit does not carry the evidence for its own contents. Stage and commit assets/public_provenance_ledger.json, assets/public_source_profile.json and PUBLIC_EXPORT.json."; exit 1; }
	@echo "== readiness: OK (the suite is separate and still yours to run)"

# One-command local refresh using the hosted attestation's control generator.
# Stage intended candidate changes first; this target stages only the generated
# profile (when requested) and the two generated controls. The external
# authority remains mandatory. Set PROVENANCE_REFRESH_APPLY_POLICY=1 only when
# the maintainer has already decided that newly seen routine paths belong on
# the public surface.
PROVENANCE_REFRESH_APPLY_POLICY ?= 0
PROVENANCE_REFRESH_POLICY_ARG = $(if $(filter 1 true yes,$(PROVENANCE_REFRESH_APPLY_POLICY)),--apply-policy,)

provenance-refresh:
ifndef NK_TRUSTED_LEDGER
	@echo "provenance-refresh: TRUSTED_INPUT_MISSING -- NK_TRUSTED_LEDGER is unset."
	@echo "  Set it to the external IMPLEMENTATION_PROVENANCE.json and retry."
	@echo "  Stage intended candidate changes before running this target."
	@exit 1
else
	$(PYTHON) tools/provenance_refresh.py --trusted-ledger "$(NK_TRUSTED_LEDGER)" $(PROVENANCE_REFRESH_POLICY_ARG)
endif

# The contributor fast path: one command, only the gates that apply to the
# files this branch changed against origin/main, finished in minutes.
#
# It is a subset of `check`, never a substitute: `check` and `readiness` remain
# the authoritative gates, and CI still runs the hosted matrix. What it removes
# is the reason a contributor gives up -- being told to run the whole native and
# tooling suite to find out whether a one-line documentation change is well
# formed.
#
# Path routing is tools/ci_paths.py, the same classifier the hosted workflow
# uses, so the local selection cannot drift from the hosted one. The gates it
# runs are the ones a contributor can actually clear: Ruff, the Python test
# modules matching the changed tools/, a strict C compile of the changed C, and
# markdownlint plus the documentation-freshness lint for changed Markdown.
#
# The publication audit's provenance self-consistency leg compares the working
# tree with the checked-in ledger, which only a maintainer holding the private
# trusted ledger can regenerate. Those findings are printed as MAINTAINER-SIDE
# and do not fail this target; every other publication finding does. Nothing is
# suppressed -- both lists are always shown.
#
#   mingw32-make contrib-check              # Windows
#   make contrib-check                      # Linux
#   CONTRIB_BASE=origin/main contrib-check  # explicit base
#
# Reports PASS, FAIL, SKIP (tool not installed) or NOT_RUN (surface untouched)
# per gate. A skipped gate is never reported as a pass.
CONTRIB_BASE ?= origin/main

# The sub-gates run in the same interpreter as this script by default. Set
# CONTRIB_PYTHON in the environment when the repository's default python is not
# the one holding ruff and the project dependencies -- which is the normal case
# on Windows, where MSYS2's python precedes the CPython the project uses. It is
# an environment variable rather than a make variable because an interpreter
# path usually contains a space, and the rest of this Makefile expands
# $(PYTHON) unquoted.
contrib-check:
	CONTRIB_PYTHON="$${CONTRIB_PYTHON:-$(PYTHON)}" $(PYTHON) tools/contrib_check.py --base "$(CONTRIB_BASE)"

public-safe-verify:
	$(MAKE) PUBLIC_SAFE=1 portable-core-objects

# -----------------------------------------------------------------------------
# Public Verification Entry Points (Issue #188 Finding 3 O-05)
# -----------------------------------------------------------------------------
test:
	$(PYTHON) -m unittest discover -s tools -p "test_*.py" -v

check:
	@echo "== [1/5] Documentation freshness lint =="
	$(PYTHON) tools/lint_docs.py
	@echo "== [2/5] Canonical publication policy coverage =="
	$(PYTHON) -m unittest tools/test_publication_policy_gate.py
	@echo "== [3/5] Publication safety audits (index & worktree) =="
	$(PYTHON) tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
	$(PYTHON) tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
	@echo "== [4/5] Native host core tests =="
	$(MAKE) native-core-tests
	@echo "== [5/5] Fast critical test subset =="
	$(PYTHON) -m unittest \
		tools/test_title_manifest.py \
		tools/test_title_catalog.py \
		tools/test_build_system_parity.py \
		tools/test_sync_drift_check.py \
		tools/test_ci_paths.py \
		tools/test_ci_required.py
	@echo "== All public verification checks PASSED =="

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
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PRODUCTION_SMOKE_DIR) \
		FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 \
		LDFLAGS="$(LDFLAGS) -Wl,-Map,$(PRODUCTION_SMOKE_MAP)"
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) verify --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) run --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot

# Run the same smoke with the executable staged into a fresh directory outside
# the build tree (#294): a launcher/staging path bug must fail here, not only in
# the build-tree spelling.
production-smoke-staged: production-smoke
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) run-staged --build-dir $(PRODUCTION_SMOKE_DIR) --mode aot

production-smoke-clean:
	$(MAKE) BUILD_DIR=$(PRODUCTION_SMOKE_DIR) clean

showcase:
	$(PYTHON) fixtures/showcase/showcase.py build

showcase-smoke: showcase
	$(PYTHON) fixtures/showcase/showcase.py smoke

# display-smoke builds the guest and asserts the presented framebuffer word
# headlessly. display-smoke-gui is the same image in the SDL3/Vulkan window and
# is deliberately NOT part of any aggregate gate: it needs a display.
display-smoke: DISPLAY_SMOKE_BUILD_FRAMES ?= $(DISPLAY_SMOKE_FRAMES)
display-smoke:
	$(PYTHON) $(DISPLAY_SMOKE_GENERATOR) generate --out-dir $(DISPLAY_SMOKE_FIXTURE) \
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
	$(PYTHON) $(DISPLAY_SMOKE_GENERATOR) verify --build-dir $(DISPLAY_SMOKE_DIR)

display-smoke-run: display-smoke
	$(PYTHON) $(DISPLAY_SMOKE_GENERATOR) run --build-dir $(DISPLAY_SMOKE_DIR)

display-smoke-gui:
	$(MAKE) display-smoke DISPLAY_SMOKE_BUILD_FRAMES=$(DISPLAY_SMOKE_DEMO_FRAMES)
	$(PYTHON) $(DISPLAY_SMOKE_GENERATOR) run --build-dir $(DISPLAY_SMOKE_DIR) --gui

# Drive the actual native-player PLAY NOW path against the launchable fixture.
# The child opens a real window, so this is a developer/display gate rather than
# part of the headless CI aggregate. The fixture generator records the child's
# machine-readable window/first-frame evidence.
display-smoke-player: player display-smoke-run
	$(PYTHON) $(DISPLAY_SMOKE_GENERATOR) run-player --build-dir $(DISPLAY_SMOKE_DIR)

display-smoke-clean:
	$(MAKE) BUILD_DIR=$(DISPLAY_SMOKE_DIR) clean

# AOT-gap mode of the same fixture: the helper is omitted from native emission
# (build-time codegen choice), so region A reaches it through the typed production
# dispatch_call() seam. Generated registration owns the executable span; the
# interpreter executes the helper and hands off to registered AOT region B, whose
# final value is asserted by the production driver.
production-smoke-gap:
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) generate --out-dir $(PRODUCTION_SMOKE_GAP_FIXTURE) --mode aot-gap
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
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) verify --build-dir $(PRODUCTION_SMOKE_GAP_DIR) --mode aot-gap
	$(PYTHON) $(PRODUCTION_SMOKE_GENERATOR) run --build-dir $(PRODUCTION_SMOKE_GAP_DIR) --mode aot-gap

production-smoke-gap-clean:
	$(MAKE) BUILD_DIR=$(PRODUCTION_SMOKE_GAP_DIR) clean

perf-benchmark:
	$(PYTHON) tools/run_perf_benchmarks.py --make "$(MAKE)" --python "$(PYTHON)" --output "$(BUILD_DIR)/perf-benchmark" --overhead

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
PLATFORM_LADDER_DIR       := build/platform-ladder
PLATFORM_LADDER_GENERATOR := fixtures/platform_ladder/generate.py

PL_ZERO_BASE   := 0x08940000
PL_RELOC_BASE  := 0x088C0000
PL_SCHED_BASE  := 0x08900000
PL_FPU_BASE    := 0x08980000
PL_FS_BASE     := 0x089C0000
PL_TITLE2_BASE := 0x08A40000
PL_TITLE2_NEGATIVE_BASE := 0x08A80000

platform-ladder: platform-ladder-zero platform-ladder-reloc platform-ladder-gap platform-ladder-sched platform-ladder-fpu platform-ladder-fs platform-ladder-fs-negative platform-ladder-title2 platform-ladder-title2-negative

platform-ladder-zero:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-zero --out-dir $(PLATFORM_LADDER_DIR)/ladder-zero/fixture
	$(MAKE) all \
		GAME_NAME=pl_zero \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-zero/fixture/guest.prx \
		GAME_BASE=$(PL_ZERO_BASE) \
		GAME_ENTRY=0x08940040 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-zero \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-zero --build-dir $(PLATFORM_LADDER_DIR)/ladder-zero
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-zero --build-dir $(PLATFORM_LADDER_DIR)/ladder-zero

platform-ladder-reloc:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-reloc --out-dir $(PLATFORM_LADDER_DIR)/ladder-reloc/fixture
	$(MAKE) all \
		GAME_NAME=pl_reloc \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-reloc/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-reloc/fixture/guest.psp \
		GAME_BASE=$(PL_RELOC_BASE) \
		GAME_ENTRY=0x088C0020 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-reloc \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-reloc --build-dir $(PLATFORM_LADDER_DIR)/ladder-reloc
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-reloc --build-dir $(PLATFORM_LADDER_DIR)/ladder-reloc

# AOT-gap mode of ladder-reloc's tier chain: the interior callee is omitted
# from native emission so the call reaches the ordinary production dispatch()
# interpreter seam. Expected result is byte-identical to the intended guest.
platform-ladder-gap:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-gap --out-dir $(PLATFORM_LADDER_DIR)/ladder-gap/fixture
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
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-gap --build-dir $(PLATFORM_LADDER_DIR)/ladder-gap
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-gap --build-dir $(PLATFORM_LADDER_DIR)/ladder-gap

platform-ladder-sched:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-sched --out-dir $(PLATFORM_LADDER_DIR)/ladder-sched/fixture
	$(MAKE) all \
		GAME_NAME=pl_sched \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-sched/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-sched/fixture/guest.psp \
		GAME_BASE=$(PL_SCHED_BASE) \
		GAME_ENTRY=0x08900010 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-sched \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-sched --build-dir $(PLATFORM_LADDER_DIR)/ladder-sched
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-sched --build-dir $(PLATFORM_LADDER_DIR)/ladder-sched

platform-ladder-fpu:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-fpu --out-dir $(PLATFORM_LADDER_DIR)/ladder-fpu/fixture
	$(MAKE) all \
		GAME_NAME=pl_fpu \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-fpu/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-fpu/fixture/guest.psp \
		GAME_BASE=$(PL_FPU_BASE) \
		GAME_ENTRY=0x08980008 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-fpu \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-fpu --build-dir $(PLATFORM_LADDER_DIR)/ladder-fpu
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-fpu --build-dir $(PLATFORM_LADDER_DIR)/ladder-fpu

platform-ladder-fs:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-fs --out-dir $(PLATFORM_LADDER_DIR)/ladder-fs/fixture
	$(MAKE) all \
		GAME_NAME=pl_fs \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-fs/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-fs/fixture/guest.psp \
		GAME_BASE=$(PL_FS_BASE) \
		GAME_ENTRY=0x089C0018 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-fs \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-fs --build-dir $(PLATFORM_LADDER_DIR)/ladder-fs
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-fs --build-dir $(PLATFORM_LADDER_DIR)/ladder-fs

# Negative control: same executable and guest, but the payload file is absent.
# sceIoOpen must fail visibly and the guest must store the failure sentinel.
platform-ladder-fs-negative:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-fs --build-dir $(PLATFORM_LADDER_DIR)/ladder-fs --negative

platform-ladder-title2:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-title2 --out-dir $(PLATFORM_LADDER_DIR)/ladder-title2/fixture
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
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-title2 --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-title2 --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2

# Negative control: a real mapped import stub dispatches an absent NID through
# sr_syscall() and the production unknown-HLE scheduler boundary must exit 7.
platform-ladder-title2-negative:
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) generate --workload ladder-title2-negative --out-dir $(PLATFORM_LADDER_DIR)/ladder-title2-negative/fixture
	$(MAKE) all \
		GAME_NAME=pl_title2_negative \
		GAME_ELF=$(PLATFORM_LADDER_DIR)/ladder-title2-negative/fixture/guest.prx \
		GAME_PSP_HEADER=$(PLATFORM_LADDER_DIR)/ladder-title2-negative/fixture/guest.psp \
		GAME_BASE=$(PL_TITLE2_NEGATIVE_BASE) \
		GAME_ENTRY=0x08A80020 \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(PLATFORM_LADDER_DIR)/ladder-title2-negative \
		FUNCS_PER_CHUNK=2 PUBLIC_SAFE=1
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) verify --workload ladder-title2-negative --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2-negative
	$(PYTHON) $(PLATFORM_LADDER_GENERATOR) run --workload ladder-title2-negative --build-dir $(PLATFORM_LADDER_DIR)/ladder-title2-negative --negative

platform-ladder-clean:
	$(MAKE) BUILD_DIR=$(PLATFORM_LADDER_DIR) clean

# The generator the codegen rule runs. Overridable so the cosim mutation
# campaign can mutate the GENERATOR as well as the interpreter -- a
# differential proven against one side only is half proven. It MUST be
# defined above the codegen profile hash and the codegen rule: both expand
# it immediately, so a later definition would silently expand to empty.
CODEGEN_TOOL ?= tools/codegen.py

CODEGEN_PROFILE_HASH := $(shell $(PYTHON) $(BUILD_PROFILE_TOOL) hash --compiler "$(PYTHON)" --entry "GAME_NAME=$(GAME_NAME)" --entry "GAME_BASE=$(GAME_BASE)" --entry "CODEGEN_PROFILE_ARG=$(CODEGEN_PROFILE_ARG)" --entry "EXTRA_ELF_ARGS=$(EXTRA_ELF_ARGS)" --entry "EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)" --entry "FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)" --entry "CODEGEN_USER_ARGS=$(CODEGEN_USER_ARGS)" --entry "CODEGEN_TOOL=$(CODEGEN_TOOL)" --file "$(CPU_STATE_ABI_HEADER)" $(CHUNK_TARGET_ENTRY))
CODEGEN_PROFILE_STAMP := $(BUILD_DIR)/.codegen-profile-$(CODEGEN_PROFILE_HASH)

$(CODEGEN_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) record --output "$(CODEGEN_PROFILE_MANIFEST)" --section codegen --compiler "$(PYTHON)" --entry "GAME_NAME=$(GAME_NAME)" --entry "GAME_BASE=$(GAME_BASE)" --entry "CODEGEN_PROFILE_ARG=$(CODEGEN_PROFILE_ARG)" --entry "EXTRA_ELF_ARGS=$(EXTRA_ELF_ARGS)" --entry "EXTRA_SPAN_ARG=$(EXTRA_SPAN_ARG)" --entry "FUNCS_PER_CHUNK=$(FUNCS_PER_CHUNK)" --entry "CODEGEN_USER_ARGS=$(CODEGEN_USER_ARGS)" --entry "CODEGEN_TOOL=$(CODEGEN_TOOL)" --file "$(CPU_STATE_ABI_HEADER)" $(CHUNK_TARGET_ENTRY) --stamp "$@" --stale-glob ".codegen-profile-*" --invalidate-glob "$(BUILD_DIR)/$(GAME_NAME)_recomp*.o"

# Re-checked on every invocation that needs a guest input (hence FORCE), but rewritten
# only when an input's identity actually changed, so dependents do not rebuild spuriously.
# GAME_PSP_HEADER and GAME_EXTRA_ELFS_ENV are optional; GAME_ELF is not.
$(GAME_INPUT_STAMP): $(BUILD_PROFILE_TOOL) FORCE
	$(PYTHON) $(BUILD_PROFILE_TOOL) stamp-inputs --env GAME_ELF --optional-env GAME_PSP_HEADER --optional-env GAME_EXTRA_ELFS_ENV --list-env GAME_EXTRA_ELFS_ENV --out $@

FORCE:

pipeline: $(BUILD_DIR)/$(GAME_NAME)_image.bin $(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_imports.toml

$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py
	$(PYTHON) tools/prxload.py --env-elf $(GAME_BASE) $(PSP_HEADER_ENV_ARG) --out=$@

ifeq ($(NK_AOT_PREGENERATED),1)
$(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h:
	$(if $(wildcard $@),,$(error NK_AOT_PREGENERATED requires $@))
else
$(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h: $(GAME_INPUT_PREREQ) $(CODEGEN_TOOL) tools/codegen.py tools/analyze.py tools/entry_frame_balance.py tools/prxload.py tools/host_stubs.py tools/imports.py $(CODEGEN_PROFILE_STAMP)
	$(PYTHON) $(CODEGEN_TOOL) --env-elf $(BUILD_DIR)/$(GAME_NAME)_recomp.c --base=$(GAME_BASE) $(CODEGEN_PROFILE_ARG) $(EXTRA_SPAN_ARG) $(EXTRA_ELF_ENV_ARG) --funcs-per-chunk=$(FUNCS_PER_CHUNK) $(CHUNK_BYTES_ARG) $(CODEGEN_USER_ARGS)
endif

$(BUILD_DIR)/$(GAME_NAME)_imports.toml: $(GAME_INPUT_PREREQ) tools/imports.py tools/analyze.py tools/prxload.py
	$(PYTHON) tools/imports.py --env-elf $(GAME_BASE) --toml=$@

# ge.c: software comparison rasterizer with PPSSPP-derived behavior. -O2 for speed.
GE_CFLAGS ?= -O2 -fno-math-errno -Wall -Wextra -Isrc/rt -DSR_SDL3VK
RUNTIME_PROFILE_HASH := $(shell $(PYTHON) $(BUILD_PROFILE_TOOL) hash --compiler "$(CC)" --entry "CFLAGS=$(CFLAGS)" --entry "GE_CFLAGS=$(GE_CFLAGS)" --entry "TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)" --entry "SDL3_PROVIDER=$(SDL3_PROVIDER)" --entry "SDL3_VERSION=$(SDL3_VERSION)" --entry "SDL3_DIR=$(SDL3_DIR)" --entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" --file "$(CPU_STATE_ABI_HEADER)")
RUNTIME_PROFILE_STAMP := $(BUILD_DIR)/.runtime-profile-$(RUNTIME_PROFILE_HASH)
RUNTIME_INVALIDATE_ARGS := $(foreach obj,$(RT_GE_O) $(RT_OBJS),--invalidate "$(obj)")

$(RUNTIME_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) record --output "$(RUNTIME_PROFILE_MANIFEST)" --section runtime --compiler "$(CC)" --entry "CFLAGS=$(CFLAGS)" --entry "GE_CFLAGS=$(GE_CFLAGS)" --entry "TITLE_CONFIG_DIGEST=$(TITLE_CONFIG_DIGEST)" --entry "SDL3_PROVIDER=$(SDL3_PROVIDER)" --entry "SDL3_VERSION=$(SDL3_VERSION)" --entry "SDL3_DIR=$(SDL3_DIR)" --entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" --file "$(CPU_STATE_ABI_HEADER)" --stamp "$@" --stale-glob ".runtime-profile-*" $(RUNTIME_INVALIDATE_ARGS)

$(RT_GE_O): src/rt/ge.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(GE_CFLAGS) $(DEPFLAGS) -c src/rt/ge.c -o $@

# Optimization and memory-saving flags for massive machine-generated files.
# -O0: Conservative default for generic/unqualified titles to prevent compiler OOM / hangs.
# -O1: Measured and qualified default for HST.
# -fno-var-tracking: Saves significant memory on huge functions.
# -ftrack-macro-expansion=0: Reduces memory overhead for macro-heavy code.
RECOMP_OPT ?= -O0
RECOMP_FLAGS ?= $(RECOMP_OPT) -w -fno-var-tracking -ftrack-macro-expansion=0 $(NAN_TRAP_CFLAG)
TRACE ?= 0
ifeq ($(TRACE),1)
RECOMP_FLAGS += -DSR_INSTRUCTION_TRACE
endif
ifeq ($(PERF_AOT_INSTRUCTIONS),1)
RECOMP_FLAGS += -DPERF_AOT_INSTRUCTIONS=1
endif

# Make the object flavour explicit. Switching TRACE forces only the generated
# chunks to rebuild, avoiding a stale trace-enabled object in a release binary
# (or a trace-disabled object in an oracle run).
TRACE_STAMP := $(BUILD_DIR)/.recomp-trace-$(TRACE)
$(TRACE_STAMP):
	$(PYTHON) $(BUILD_PROFILE_TOOL) stamp --output "$@" --stale-glob ".recomp-trace-*" --value "$(TRACE)"

RECOMP_PROFILE_HASH := $(shell $(PYTHON) $(BUILD_PROFILE_TOOL) hash --compiler "$(CC)" --entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --entry "TRACE=$(TRACE)" --entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" --file "$(CPU_STATE_ABI_HEADER)")
RECOMP_PROFILE_STAMP := $(BUILD_DIR)/.recomp-profile-$(RECOMP_PROFILE_HASH)
$(RECOMP_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(PYTHON) $(BUILD_PROFILE_TOOL) record --output "$(RECOMP_PROFILE_MANIFEST)" --section generated --compiler "$(CC)" --entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --entry "TRACE=$(TRACE)" --entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" --file "$(CPU_STATE_ABI_HEADER)" --stamp "$@" --stale-glob ".recomp-profile-*" --invalidate-glob "$(BUILD_DIR)/$(GAME_NAME)_recomp*.o"

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

$(BUILD_DIR)/%.o: src/core/%.c $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The one translation unit that reads the build-local generated configuration. Every
# other runtime source consumes the generic accessors in src/rt/title_config.h, so the
# generated include path stops here rather than leaking into CFLAGS.
$(BUILD_DIR)/title_config.o: src/rt/title_config.c src/rt/title_config.h $(TITLE_CONFIG_HEADER) $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) -I$(TITLE_CONFIG_DIR) $(DEPFLAGS) -c $< -o $@

$(PORTABLE_CORE_DIR)/title_config.o: src/rt/title_config.c src/rt/title_config.h $(TITLE_CONFIG_HEADER)
	$(CC) $(PORTABLE_CORE_CFLAGS) -I$(TITLE_CONFIG_DIR) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/hle_power.o: src/rt/hle_power.c src/rt/hle_power.h

$(BUILD_DIR)/hle.o: src/rt/hle.c src/rt/asset_index.h src/rt/archive_vfs.h src/rt/pgf_api.h src/rt/atrac3p_bridge.h src/rt/gpu_sdl3vk/ge_gpu.h src/rt/hle_power.h
	$(CC) $(CFLAGS) $(HLE_INCLUDES) $(DEPFLAGS) -c $< -o $@
$(BUILD_DIR)/pgf.o: src/rt/pgf.c src/rt/pgf_api.h src/rt/pgf.h
$(BUILD_DIR)/pgf_public.o: src/rt/pgf_public.c src/rt/pgf_api.h src/rt/recomp.h src/rt/ge_shared.h

runtime-objects: shader-verify $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o

$(PORTABLE_CORE_DIR)/%.o: src/rt/%.c src/rt/recomp.h
	$(CC) $(PORTABLE_CORE_CFLAGS) $(DEPFLAGS) -c $< -o $@

portable-core-objects: $(PORTABLE_CORE_OBJS)

# Only the flight recorder embeds the reproducible build identity. A stamp named
# after the identity makes flight_recorder.o rebuild when the commit or
# SOURCE_DATE_EPOCH changes, without touching the other runtime objects.
FLIGHT_IDENTITY_STAMP := $(BUILD_DIR)/.flight-identity-$(or $(SR_SOURCE_COMMIT),none)-$(or $(SR_SOURCE_DATE_EPOCH),none)
$(FLIGHT_IDENTITY_STAMP):
	@$(PYTHON) -c "from pathlib import Path; p = Path(r'$@'); p.parent.mkdir(parents=True, exist_ok=True); [s.unlink() for s in p.parent.glob('.flight-identity-*') if s != p]; p.touch()"

# Explicit rules (not target-specific variables, which would also leak into the
# runtime-profile stamp prerequisite and record a different CFLAGS than it hashes).
$(BUILD_DIR)/flight_recorder.o: src/rt/flight_recorder.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP) $(FLIGHT_IDENTITY_STAMP)
	$(CC) $(CFLAGS) $(FLIGHT_IDENTITY_DEFS) $(DEPFLAGS) -c $< -o $@

$(PORTABLE_CORE_DIR)/flight_recorder.o: src/rt/flight_recorder.c src/rt/recomp.h $(FLIGHT_IDENTITY_STAMP)
	$(CC) $(PORTABLE_CORE_CFLAGS) $(FLIGHT_IDENTITY_DEFS) $(DEPFLAGS) -c $< -o $@

atrac3p-objects: $(ATRAC3P_OBJS)

# A clear player-target failure when no usable SDK resolved (see the Windows
# branch above). On Linux the Vulkan loader is a system library, so the check is
# an empty recipe and the player links -lvulkan directly.
.PHONY: player-vulkan-check
ifeq ($(OS),Windows_NT)
player-vulkan-check:
	$(if $(strip $(VULKAN_SDK)),,$(error No usable Vulkan SDK found; set VULKAN_SDK to the SDK root (e.g. mingw32-make player VULKAN_SDK=C:/path/to/VulkanSDK/<version>) or install a current SDK))
else
player-vulkan-check: ;
endif

# A clear failure when SDL3 dependency is missing or invalid.
.PHONY: sdl3-check
sdl3-check:
	@$(PYTHON) -c "import sys; sys.exit(sys.argv[1] or None)" "$(SDL3_ERROR)"

PLAYER_EXE ?= build/nakagawa_player$(EXE_EXT)
PLAYER_UI_TEST_EXE ?= build/nakagawa_player_ui_test$(EXE_EXT)
PLAYER_CORE_SOURCES := src/core/nk_font.c src/core/nk_iso.c src/core/nk_library.c src/core/nk_launch.c src/core/nk_title_manifest.c src/core/nk_xb.c src/core/nk_input_profile.c src/core/nk_json.c src/core/generated/nk_title_catalog.c
PLAYER_CORE_SRCS := $(PLAYER_CORE_SOURCES) $(PLAYER_PLATFORM_SRC)
PLAYER_SRCS := src/player/main.c src/player/player_state.c src/player/input_settings.c src/player/iso_reader.c src/player/setup_staging.c src/player/ui_renderer.c src/player/package_builder.c $(PLAYER_CORE_SRCS)
PLAYER_INCLUDES := -Isrc/player -Isrc/core -Isrc/core/generated $(SDL3_INC_FLAGS) $(PLAYER_VULKAN_INC)

$(PLAYER_EXE): | player-vulkan-check sdl3-check

$(PLAYER_EXE): $(PLAYER_SRCS) src/player/player_state.h src/player/input_settings.h src/player/iso_reader.h src/player/setup_staging.h src/player/ui_renderer.h src/player/package_builder.h src/core/nk_types.h src/core/nk_font.h src/core/nk_iso.h src/core/nk_library.h src/core/nk_launch.h src/core/nk_title_manifest.h src/core/nk_xb.h src/core/nk_input_profile.h src/core/nk_json.h src/core/generated/nk_title_catalog.h
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) $(RUNTIME_OPT) -Wall -Wextra $(PLAYER_INCLUDES) $(LDFLAGS) $(PLAYER_VULKAN_LIB) $(PLAYER_SRCS) -lSDL3 $(PLAYER_EXTRA_LIBS) -o $@

player: $(PLAYER_EXE)

$(PLAYER_UI_TEST_EXE): | player-vulkan-check sdl3-check

$(PLAYER_UI_TEST_EXE): $(PLAYER_SRCS) src/player/player_state.h src/player/input_settings.h src/player/iso_reader.h src/player/setup_staging.h src/player/ui_renderer.h src/player/package_builder.h src/core/nk_types.h src/core/nk_font.h src/core/nk_iso.h src/core/nk_library.h src/core/nk_launch.h src/core/nk_title_manifest.h src/core/nk_xb.h src/core/nk_input_profile.h src/core/nk_json.h src/core/generated/nk_title_catalog.h
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) $(RUNTIME_OPT) -Wall -Wextra -DNK_PLAYER_UI_REGRESSION_TEST $(PLAYER_INCLUDES) $(LDFLAGS) $(PLAYER_VULKAN_LIB) $(PLAYER_SRCS) -lSDL3 $(PLAYER_EXTRA_LIBS) -o $@

.PHONY: player-ui-regressions
player-ui-regressions: $(PLAYER_UI_TEST_EXE)
	NAKAGAWA_PLAYER_UI_TEST_EXE="$(PLAYER_UI_TEST_EXE)" $(PYTHON) -m unittest discover -s tests/native -p "test_player_ui.py" -v

CHUNK_OBJS = $(patsubst %.c,%.o,$(wildcard $(BUILD_DIR)/$(GAME_NAME)_recomp_*.c))
DEP_FILES = $(patsubst %.o,%.d,$(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(PORTABLE_CORE_OBJS) $(CHUNK_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(BUILD_DIR)/vfpu_fuzz.o)

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
COSIM_DIR        := build/cosim
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

# The loader and codegen rules are named directly rather than through `pipeline`.
# This guest deliberately imports nothing, and tools/imports.py fails closed on an
# empty stub table -- correctly, for a title. Manufacturing an unused import stub
# just to satisfy that gate would put a fiction in the fixture; the import path is
# already covered end to end by fixtures/production_smoke.
cosim-selftest:
	$(PYTHON) $(COSIM_GENERATOR) generate --out-dir $(COSIM_FIXTURE)
	$(MAKE) $(COSIM_DIR)/cosim_image.bin $(COSIM_DIR)/cosim_recomp.c \
		GAME_NAME=cosim GAME_ELF=$(COSIM_FIXTURE)/guest.prx \
		GAME_BASE=$(COSIM_BASE_ADDR) GAME_ENTRY=$(COSIM_BASE_ADDR) \
		GAME_PSP_HEADER=$(COSIM_FIXTURE)/guest.psp \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(COSIM_DIR) FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 TRACE=1 \
		CODEGEN_TOOL=$(CODEGEN_TOOL)
	$(PYTHON) $(COSIM_GENERATOR) verify --build-dir $(COSIM_DIR)
	$(MAKE) cosim-selftest-run \
		GAME_NAME=cosim GAME_ELF=$(COSIM_FIXTURE)/guest.prx \
		GAME_BASE=$(COSIM_BASE_ADDR) GAME_ENTRY=$(COSIM_BASE_ADDR) \
		GAME_PSP_HEADER=$(COSIM_FIXTURE)/guest.psp \
		GAME_EXTRA_ELFS= TITLE_MANIFEST= \
		BUILD_DIR=$(COSIM_DIR) FUNCS_PER_CHUNK=1 PUBLIC_SAFE=1 TRACE=1 \
		COSIM_INTERP_SRC=$(COSIM_INTERP_SRC) \
		COSIM_FP_CONVERT_PRELUDE=$(COSIM_FP_CONVERT_PRELUDE) \
		COSIM_FPU_REFERENCE_SRC=$(COSIM_FPU_REFERENCE_SRC)

# Second phase: CHUNK_OBJS is derived with $(wildcard) at parse time, so the
# generated chunk sources must already exist before this target is parsed.
# No $(LIBS): the harness stubs the runtime's few host-library symbols, so
# the comparison links with neither SDL3 nor Vulkan. Keeping it that way is
# deliberate -- this gate should stay runnable anywhere the toolchain is,
# not inherit the graphics stack's environment requirements.
cosim-selftest-run: $(GENERIC_TITLE_CONFIG_HEADER) $(CHUNK_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o
	$(CC) $(CFLAGS) $(COSIM_FP_CONVERT_PRELUDE_FLAG) -DSR_INSTRUCTION_TRACE \
		-I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) -I$(COSIM_FIXTURE) \
		-o $(BUILD_DIR)/cosim_selftest.exe \
		$(COSIM_HARNESS) $(COSIM_FPU_REFERENCE_SRC) $(CHUNK_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(COSIM_INTERP_SRC) src/rt/flight_recorder.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/vfpu_tables.c src/rt/perf.c -lm
	$(BUILD_DIR)/cosim_selftest.exe $(BUILD_DIR)/$(GAME_NAME)_image.bin \
		$(COSIM_BASE_ADDR) $(COSIM_TRACES)

# Prove the comparator is load-bearing: each mutant is a semantic change to the
# interpreter, FPU helper or generator that must make the gate FAIL. A mutant
# that only breaks the build is not a kill and the driver rejects it.
cosim-mutants:
	$(PYTHON) fixtures/cosim/mutate.py --build-dir $(COSIM_DIR)

cosim-selftest-clean:
	$(MAKE) BUILD_DIR=$(COSIM_DIR) clean


# Treat profile stamps as generated included makefiles. GNU Make restarts after
# creating a missing flavour, so objects invalidated by that recipe are absent
# before target freshness is evaluated (avoiding timestamp-resolution races).
ifeq ($(strip $(filter clean-preview,$(MAKECMDGOALS))),)
ifeq ($(strip $(filter clean distclean,$(MAKECMDGOALS))$(NK_INFO_ONLY)),)
-include $(CODEGEN_PROFILE_STAMP) $(RUNTIME_PROFILE_STAMP) $(RECOMP_PROFILE_STAMP) $(TITLE_CONFIG_STAMP)
endif
endif
-include $(DEP_FILES)

compile: shader-verify $(CHUNK_OBJS) $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(BUILD_DIR)/$(GAME_NAME)_recomp.o | sdl3-check
	$(CC) $(CFLAGS) $(LDFLAGS) $(LINK_MAP_ARG) -Wl,--no-insert-timestamp -o $(BUILD_DIR)/$(GAME_NAME).exe \
		$(BUILD_DIR)/$(GAME_NAME)_recomp.o \
		$(CHUNK_OBJS) \
		$(RT_GE_O) \
		$(RT_OBJS) \
		$(ATRAC3P_OBJS) \
		$(BUILD_DIR)/atrac3p_bridge.o \
		$(LIBS)
	pwsh -NoProfile -ExecutionPolicy Bypass -File copy_build_assets.ps1 -BuildDir "$(BUILD_DIR)" -Sdl3DllPath "$(SDL3_DLL)" $(ASSET_COPY_ARGS)
	@$(PYTHON) -c "print('Build finished: $(BUILD_DIR)/$(GAME_NAME).exe')"

clean:
	$(PYTHON) -c "import shutil, sys; from pathlib import Path; p = Path(r'$(BUILD_DIR)'); [shutil.rmtree(p) if p.is_dir() else p.unlink()] if p.exists() else None"

clean-fixtures:
	$(PYTHON) -c "import shutil, sys; from pathlib import Path; [shutil.rmtree(Path(d)) if Path(d).is_dir() else Path(d).unlink() for d in ('$(PRODUCTION_SMOKE_DIR)', '$(PRODUCTION_SMOKE_GAP_DIR)', '$(COSIM_DIR)', 'build/nakagawa_psp_oracle', 'build/vfpu_oracle', 'build/portable-core', 'build/verify', 'build/link') if Path(d).exists()]"

tidy: distclean

distclean:
	@echo "Removing stale build artefacts (preserving .exe and .pdb for debugger)"
	$(PYTHON) -c "from pathlib import Path; r=Path(r'$(BUILD_DIR)'); [p.unlink(missing_ok=True) for g in ('**/*.o','**/*.d','.*-profile-*','*_profile.json') for p in r.glob(g)] if r.exists() else None"
	$(PYTHON) -c "from pathlib import Path; [Path(p).unlink(missing_ok=True) for p in ('logs/build_out_recomp.log', 'logs/build_err_recomp.log', 'logs/recomp_err.log', 'logs/obj_err.log', 'link_err.log', 'logs/stdout_run.log', 'logs/stderr_run.log')]"

clean-all: clean clean-fixtures
	$(PYTHON) -c "import shutil, sys; from pathlib import Path; b = Path('build'); [shutil.rmtree(p) if p.is_dir() else p.unlink() for p in b.iterdir()] if b.exists() else None"
	$(PYTHON) -c "from pathlib import Path; [Path(p).unlink(missing_ok=True) for p in ('logs/build_out_recomp.log', 'logs/build_err_recomp.log', 'logs/recomp_err.log', 'logs/obj_err.log', 'link_err.log', 'logs/stdout_run.log', 'logs/stderr_run.log')]"

# Preview-first workspace clean (issue #368): plans allowlisted output roots and
# prints the exact file set. Deletion requires CONFIRM=1 (which passes --yes);
# OLDER_THAN=<days> bounds the plan by file age. Existing clean targets are untouched.
clean-preview:
	$(PYTHON) tools/nk_clean.py $(if $(filter 1,$(CONFIRM)),--yes,) $(if $(strip $(OLDER_THAN)),--older-than $(OLDER_THAN),)

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
	@$(foreach cfg,$(SCHED_SELFTEST_CONFIGS),$(MAKE) --no-print-directory sched-selftest-one SCHED_SELFTEST_CONFIG=$(cfg)$(NEWLINE)	)

# One configuration of the matrix. SCHED_SELFTEST_CONFIG names the flavour; the title
# configuration is generated fresh into build/<game>/title-config/<flavour>/.
SCHED_SELFTEST_CONFIG ?= generic
SCHED_SELFTEST_DIR := $(BUILD_DIR)/title-config/$(SCHED_SELFTEST_CONFIG)
SCHED_SELFTEST_MANIFEST := $(SCHED_SELFTEST_MANIFEST_$(SCHED_SELFTEST_CONFIG))
SCHED_SELFTEST_CONFIG_ARG := $(if $(strip $(SCHED_SELFTEST_MANIFEST)),--manifest $(strip $(SCHED_SELFTEST_MANIFEST)),)

sched-selftest-one: $(TITLE_CONFIG_TOOL) tools/title_manifest.py src/rt/nested_frames.c src/rt/nested_frames.h
	$(PYTHON) $(TITLE_CONFIG_TOOL) $(SCHED_SELFTEST_CONFIG_ARG) --output $(SCHED_SELFTEST_DIR)/sr_title_config.h
	$(CC) $(CFLAGS) -I$(SCHED_SELFTEST_DIR) -DSR_SCHED_LIVENESS_TEST $(LDFLAGS) -o $(BUILD_DIR)/sched_selftest_$(SCHED_SELFTEST_CONFIG).exe \
		src/rt/sched_selftest.c src/rt/nested_frames.c src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/title_config.c $(LIBS)
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
		src/rt/heap_selftest.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/vfpu_tables.c src/rt/title_config.c src/rt/perf.c $(LIBS) -lm
	$(BUILD_DIR)/heap_selftest.exe

# profiler-selftest — production profiler hash-table regression suite. Exercises PC zero as a
# real key and a deliberately saturated 64-probe collision window without game inputs.
profiler-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -DSR_PROFILER_SELFTEST \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables $(LDFLAGS) \
		-Wl,--gc-sections -o $(BUILD_DIR)/profiler_selftest.exe \
		src/rt/profiler_selftest.c src/rt/recomp.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/perf.c $(LIBS) -lm
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
# watchpoints.json runtime artifact (issue #188): the canonical JSON
# fixture round-trips into the expected native watchpoint set, plus fail-closed
# cases (wrong version/format, malformed JSON, out-of-range/reversed/oversized
# strbuf-selftest — unit and adversarial tests for sr_buf_append / strbuf.h.
# Exercises bounded formatting, hostile cursors, negative returns, canary bounds,
# and golden trace formatting invariants.
strbuf-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/strbuf_selftest.exe src/rt/strbuf_selftest.c
	$(BUILD_DIR)/strbuf_selftest.exe

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
		src/rt/atrac3p_bridge_selftest.c src/rt/atrac3p_bridge.c src/rt/perf.c \
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
# overlap scan, plus the issue #40 codegen-vs-interpreter differential for
# vhdp/vmscl overlap encodings. The white-box TU includes the real
# recomp.c/vfpu_tables.c/vfpu_interp.c (heap_selftest pattern); only
# scheduler/driver plumbing is stubbed. No game inputs or private data required.
vfpu-interp-selftest: $(GENERIC_TITLE_CONFIG_HEADER) $(BUILD_DIR)/vfpu_overlap_diff_cases.h
	$(CC) $(CFLAGS) -DSR_FLIGHT_RECORDER_STANDALONE -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_interp_selftest.exe \
		src/rt/vfpu_interp_selftest.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/perf.c $(LIBS) -lm
	$(BUILD_DIR)/vfpu_interp_selftest.exe

$(BUILD_DIR)/vfpu_overlap_diff_cases.h: tools/vfpu_overlap_diff_gen.py tools/codegen.py
	$(PYTHON) tools/vfpu_overlap_diff_gen.py $@

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
HLE_SELFTEST_DEFINES := -DSR_HLE_THREAD_SELFTEST -DSR_CORO_LIFECYCLE_TEST -DSR_SCHED_LIVENESS_TEST
# hle.c includes atrac3p_bridge.h and calls into the PR-B decode bridge, so any
# target that compiles it needs the same include paths and bridge/decoder
# sources the $(BUILD_DIR)/hle.o rule and `compile` already use. Without the
# -I flags this target does not even reach the linker: avcodec.h fails on
# libavutil/attributes.h.

PSMF_FUZZ_ITERS ?= 0
MEDIA_FUZZ_ITERS ?= 0

psmf-producer-selftest:
	$(CC) $(CFLAGS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -o $(BUILD_DIR)/psmf_producer_selftest.exe src/rt/psmf_producer.c src/rt/psmf_producer_selftest.c
	$(BUILD_DIR)/psmf_producer_selftest.exe $(if $(PSMF_FUZZ_ITERS),--fuzz-iters $(PSMF_FUZZ_ITERS),)

# psmf-media-selftest -- the synthetic PSMF fixture through the real H.264 backend and into a
# guest display buffer.  The fixture is generated by the selftest itself (no retail media, no
# checked-in bitstream), so this target needs nothing but a compiler and, on Windows, the
# Media Foundation libraries the runtime already links.  Off Windows h264_null.c supplies the
# seam, the decode half reports SKIP, and the demux half still runs.
ifeq ($(OS),Windows_NT)
PSMF_MEDIA_LIBS := -lmfplat -lole32
else
PSMF_MEDIA_LIBS :=
endif

psmf-media-selftest:
	$(CC) $(CFLAGS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -ffunction-sections -fdata-sections \
		-o $(BUILD_DIR)/psmf_media_selftest.exe \
		src/rt/psmf_producer.c src/rt/psmf_media_selftest.c src/rt/h264_mf.c src/rt/h264_null.c src/rt/perf.c \
		$(PSMF_MEDIA_LIBS) -Wl,--gc-sections
	$(BUILD_DIR)/psmf_media_selftest.exe $(if $(MEDIA_FUZZ_ITERS),--fuzz-iters $(MEDIA_FUZZ_ITERS),)

audio-selftest:
	$(CC) $(CFLAGS) -Isrc/rt -DSR_AUDIO_SELFTEST \
		src/rt/audio_unavailable.c src/rt/perf.c -lSDL3 -lm \
		-o $(BUILD_DIR)/audio_selftest$(EXE_EXT)
	$(BUILD_DIR)/audio_selftest$(EXE_EXT)

hle-thread-selftest-build: $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER) src/rt/nested_frames.c src/rt/nested_frames.h src/rt/stale_code.c src/rt/stale_code.h
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -DSR_HLE_THREAD_SELFTEST -DSR_CORO_LIFECYCLE_TEST -DSR_SCHED_LIVENESS_TEST \
		$(HLE_INCLUDES) \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wno-unused-function \
		$(LDFLAGS) -Wl,--gc-sections -Wl,--no-insert-timestamp -o $(BUILD_DIR)/hle_thread_selftest.exe \
		src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/archive_vfs.c src/core/nk_xb.c $(PLAYER_PLAT_SOURCES) src/rt/hle_power.c src/rt/prx_loader.c src/rt/flight_recorder.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/sr_coro.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c $(LIBS)

hle-thread-selftest: hle-thread-selftest-build
	$(BUILD_DIR)/hle_thread_selftest.exe

# hle-title-selftest — run the same production-HLE executable against the generic
# profile and both public fixture profiles. The optional configured profile is
# invoked by tools/test_hle_title_config_behavior.py with a temporary synthetic
# manifest; no title-specific fixture is committed merely to make this gate pass.
HLE_TITLE_SELFTEST_CONFIGS := generic fixture-a fixture-b
HLE_TITLE_SELFTEST_MANIFEST_generic :=
HLE_TITLE_SELFTEST_MANIFEST_fixture-a := assets/titles/pspdev-phase5.json
HLE_TITLE_SELFTEST_MANIFEST_fixture-b := assets/titles/synthetic.json
HLE_TITLE_CONFIG ?= generic
HLE_TITLE_MANIFEST ?= $(HLE_TITLE_SELFTEST_MANIFEST_$(HLE_TITLE_CONFIG))
HLE_TITLE_SELFTEST_DIR := $(BUILD_DIR)/title-config/hle-$(HLE_TITLE_CONFIG)
HLE_TITLE_SELFTEST_HEADER := $(HLE_TITLE_SELFTEST_DIR)/sr_title_config.h
HLE_TITLE_SELFTEST_CONFIG_ARG := $(if $(strip $(HLE_TITLE_MANIFEST)),--manifest $(strip $(HLE_TITLE_MANIFEST)),)
HLE_TITLE_SELFTEST_EXE := $(BUILD_DIR)/hle_title_production_selftest_$(HLE_TITLE_CONFIG).exe

hle-title-selftest:
	$(MAKE) --no-print-directory hle-title-selftest-one HLE_TITLE_CONFIG=generic HLE_TITLE_MANIFEST=
	$(MAKE) --no-print-directory hle-title-selftest-one HLE_TITLE_CONFIG=fixture-a HLE_TITLE_MANIFEST=assets/titles/pspdev-phase5.json
	$(MAKE) --no-print-directory hle-title-selftest-one HLE_TITLE_CONFIG=fixture-b HLE_TITLE_MANIFEST=assets/titles/synthetic.json

hle-title-selftest-one: $(RT_GE_O) $(TITLE_CONFIG_TOOL) tools/title_manifest.py src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/archive_vfs.c src/core/nk_xb.c src/rt/hle_power.c src/rt/prx_loader.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC)
	$(PYTHON) $(TITLE_CONFIG_TOOL) $(HLE_TITLE_SELFTEST_CONFIG_ARG) --output $(HLE_TITLE_SELFTEST_HEADER)
	$(CC) $(CFLAGS) -I$(HLE_TITLE_SELFTEST_DIR) $(HLE_SELFTEST_DEFINES) $(HLE_INCLUDES) \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wno-unused-function \
		$(LDFLAGS) -Wl,--gc-sections -Wl,--no-insert-timestamp -o $(HLE_TITLE_SELFTEST_EXE) \
		src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/archive_vfs.c src/core/nk_xb.c $(PLAYER_PLAT_SOURCES) src/rt/hle_power.c src/rt/prx_loader.c src/rt/flight_recorder.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/sr_coro.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c $(LIBS)
	$(HLE_TITLE_SELFTEST_EXE) --title-config

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
	$(PYTHON) tools/psp_oracle/build_nakagawa_smoke.py --elf "$(PSP_ORACLE_SMOKE_ELF)" --out-dir "$(PSP_ORACLE_SMOKE_DIR)"
	$(PYTHON) -c "from pathlib import Path; Path(r'$(PSP_ORACLE_SMOKE_STAMP)').write_text('generated\n', encoding='ascii')"

$(PSP_ORACLE_SMOKE_HEADER) $(PSP_ORACLE_SMOKE_CHUNK) $(PSP_ORACLE_SMOKE_ADAPTER): $(PSP_ORACLE_SMOKE_STAMP)

$(PSP_ORACLE_SMOKE_EXE): $(PSP_ORACLE_SMOKE_STAMP) $(PSP_ORACLE_SMOKE_HEADER) $(PSP_ORACLE_SMOKE_CHUNK) $(PSP_ORACLE_SMOKE_ADAPTER) src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/archive_vfs.c src/core/nk_xb.c src/rt/hle_power.c src/rt/prx_loader.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/sr_coro.c $(PGD_BACKEND_SRC) $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(HLE_SELFTEST_DEFINES) $(HLE_INCLUDES) -DSR_PSP_ORACLE_SMOKE \
		-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables \
		-Wno-unused-function -w -I"$(PSP_ORACLE_SMOKE_DIR)" $(LDFLAGS) \
		-Wl,--gc-sections -Wl,--no-insert-timestamp -o "$(PSP_ORACLE_SMOKE_EXE)" \
		src/rt/hle_thread_selftest.c src/rt/hle.c src/rt/archive_vfs.c src/core/nk_xb.c $(PLAYER_PLAT_SOURCES) src/rt/hle_power.c src/rt/prx_loader.c src/rt/flight_recorder.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/sr_coro.c src/rt/title_config.c src/rt/psmf_producer.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c \
		"$(PSP_ORACLE_SMOKE_DIR)/smoke_entry.c" "$(PSP_ORACLE_SMOKE_DIR)/smoke_recomp_0.c" $(LIBS)

psp-oracle-nakagawa-smoke-build: $(PSP_ORACLE_SMOKE_EXE)

psp-oracle-nakagawa-smoke: psp-oracle-nakagawa-smoke-build
	$(PYTHON) tools/psp_oracle/run_nakagawa.py --executable "$(PSP_ORACLE_SMOKE_EXE)" --output "$(PSP_ORACLE_SMOKE_OUTPUT)" -- --psp-oracle --case sum-1-to-100 --artifact "$(PSP_ORACLE_SMOKE_EXE)" --source-commit "$(PSP_ORACLE_SOURCE_COMMIT)" --model "$(PSP_ORACLE_MODEL)" --firmware "$(PSP_ORACLE_FIRMWARE)"

# dispatch-selftest — host-neutral unit tests for the guest code-address table (issue #45).# No game inputs needed; compiles src/rt/dispatch_selftest.c against the real primitives in
# dispatch_table.h. Asserts that guest address 0 is a first-class key (register/look up,
# hash collisions involving 0 in both orders, L1 caching, re-registration), that a real
# function at address 0 executes while an unregistered lookup does not, and that occupancy
# is independent of the key. Exit code 0 = all invariants hold.
dispatch-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/dispatch_selftest.exe src/rt/dispatch_selftest.c
	$(BUILD_DIR)/dispatch_selftest.exe

# stale-code-selftest — host-neutral unit tests for the TD-27 opt-in stale
# translated-code detector (src/rt/stale_code.*). No game inputs needed; links
# only the standalone detector TU. Runs twice: gate off (an overwritten
# invalidate must stay silent) and SR_STALE_DETECT=1 (an overwritten
# translated block must fire loudly, pristine invalidates must stay silent).
# Exit code 0 = all invariants hold.
stale-code-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/stale_code_selftest.exe src/rt/stale_code_selftest.c src/rt/stale_code.c
	$(BUILD_DIR)/stale_code_selftest.exe
	$(PYTHON) -c "import os,subprocess,sys; e=dict(os.environ); e['SR_STALE_DETECT']='1'; p=os.path.abspath(r'$(BUILD_DIR)/stale_code_selftest.exe'); sys.exit(subprocess.run([p], env=e).returncode)"

# cpu-lle-selftest — host-neutral unit tests for the LLE Phase 1 COP0,
# exception, and eret helpers (spec 3.2/3.3) plus the interpreter lane
# (spec 3.5). No game inputs needed; #includes recomp.c/cpu_lle.c for direct
# access to the exact helpers the generated code calls and links the real
# guest_interp.c, proving helper and interpreter behavior from one binary.
# Asserts exception EPC/Cause/EXL/BD (including delay-slot faults), eret
# return + EXL clear, user-mode traps, RI on unsupported encodings, Status /
# Cause write masking, vector selection/validation, and the LLE gate that
# keeps default-lane syscall/break fail-closed. Exit code 0 = all hold.
cpu-lle-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -DSR_INSTRUCTION_TRACE -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/cpu_lle_selftest.exe \
		src/rt/cpu_lle_selftest.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/vfpu_tables.c src/rt/title_config.c src/rt/perf.c -lm
	$(BUILD_DIR)/cpu_lle_selftest.exe

# domain-mode-selftest — host-neutral unit tests for the LLE Phase 1
# per-domain HLE/LLE table and the sr_import_call() seam (spec section 4).
# Same white-box shape as cpu-lle-selftest: #includes recomp.c/cpu_lle.c/
# domain_mode.c for direct access to the exact seam the generated stubs call
# and links the real guest_interp.c, proving the LLE guest-export lane runs
# through the production linked-call boundary. No game inputs needed. Asserts
# HLE-everywhere defaults, set/get/reset/lock, the library table, NID/export
# registries (conflicts fail closed), HLE passthrough, COSIM accounting, LLE
# hit/miss/dispatch-reject, and fallback hit/miss/reject. Exit code 0 = all.
domain-mode-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/domain_mode_selftest.exe \
		src/rt/domain_mode_selftest.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/stale_code.c src/rt/vfpu_tables.c src/rt/title_config.c src/rt/perf.c -lm
	$(BUILD_DIR)/domain_mode_selftest.exe

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
	@$(foreach cfg,$(DISPATCH_ISO_CONFIGS),$(MAKE) --no-print-directory dispatch-isolation-selftest-one DISPATCH_ISO_CONFIG=$(cfg)$(NEWLINE)	)

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
		src/rt/dispatch_isolation_selftest.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/vfpu_tables.c src/rt/perf.c \
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
VFPU_FUZZ_TITLE_OBJ :=
VFPU_FUZZ_CHUNK_OBJS :=
VFPU_FUZZ_VALIDATE := vfpu_fuzz_validate_synthetic
# CI/public mode: the caller generated a synthetic cases header explicitly.  Do not
# introduce a fake GAME_ELF dependency or regenerate from proprietary/private input.
$(VFPU_FUZZ_H):
	$(PYTHON) tools/vfpu_fuzz_gen.py --require-synthetic "$@"
else
VFPU_FUZZ_TITLE_OBJ := $(BUILD_DIR)/$(GAME_NAME)_recomp.o
VFPU_FUZZ_CHUNK_OBJS := $(CHUNK_OBJS)
VFPU_FUZZ_VALIDATE :=
$(VFPU_FUZZ_H): $(GAME_INPUT_PREREQ) tools/vfpu_fuzz_gen.py tools/analyze.py tools/codegen.py
	$(PYTHON) tools/vfpu_fuzz_gen.py --env-elf $(VFPU_FUZZ_H) --base=$(GAME_BASE) $(EXTRA_SPAN_ARG)
endif

vfpu_fuzz_validate_synthetic:
	$(PYTHON) tools/vfpu_fuzz_gen.py --require-synthetic "$(VFPU_FUZZ_H)"

$(BUILD_DIR)/vfpu_fuzz.o: src/rt/vfpu_fuzz.c $(VFPU_FUZZ_H) src/rt/recomp.h
	$(CC) -O0 -fno-strict-aliasing -Isrc/rt -I$(BUILD_DIR) -DSR_SDL3VK $(DEPFLAGS) -c src/rt/vfpu_fuzz.c -o $@

# Two-phase build (same rationale as `all`): `pipeline` must generate the chunk
# .c files before this target is parsed, or CHUNK_OBJS ($(wildcard)) resolves empty
# and the chunk objects are never compiled. Run codegen first, then build/link in a
# second make pass so the chunk objects are discovered.
ifeq ($(VFPU_FUZZ_PREGENERATED),1)
vfpu_fuzz:
	$(MAKE) VFPU_FUZZ_PREGENERATED=1 vfpu_fuzz_build
else
vfpu_fuzz:
	$(MAKE) pipeline
	$(MAKE) vfpu_fuzz_build
endif

# RT_OBJS carries hle.o, which calls into the PR-B ATRAC3+ decode bridge, so
# every target that links RT_OBJS must also link the bridge and the imported
# decoder TUs -- exactly as `compile` and `runtime-objects` do. Omitting them
# here is an undefined-reference link failure, not a smaller binary.
vfpu_fuzz_build: $(VFPU_FUZZ_H) $(VFPU_FUZZ_VALIDATE) $(BUILD_DIR)/vfpu_fuzz.o $(VFPU_FUZZ_CHUNK_OBJS) $(RT_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(VFPU_FUZZ_TITLE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_fuzz.exe \
		$(BUILD_DIR)/vfpu_fuzz.o \
		$(VFPU_FUZZ_TITLE_OBJ) \
		$(VFPU_FUZZ_CHUNK_OBJS) \
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

# -----------------------------------------------------------------------------
# Native Product Core Tests
# -----------------------------------------------------------------------------
player-state-test-bin:
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/player \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/player/input_settings.c src/player/player_state.c src/player/iso_reader.c src/player/package_builder.c \
		tests/native/test_player_state.c -o build/test_player_state$(EXE_EXT)

input-settings-test-bin:
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/player \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/player/input_settings.c \
		tests/native/test_input_settings.c -o build/test_input_settings$(EXE_EXT)

package-builder-test-bin:
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/player \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/player/package_builder.c \
		tests/native/test_package_builder.c -o build/test_package_builder$(EXE_EXT)

# Player UI tests link SDL3 (software renderer, no window), so they run where the
# player itself builds rather than in the SDL-free native-core-tests set.
player-ui-tests:
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra $(PLAYER_INCLUDES) $(LDFLAGS) tests/native/test_ui_clip.c -lSDL3 \
		-o build/test_ui_clip$(EXE_EXT)
	./build/test_ui_clip$(EXE_EXT)

native-core-tests: cpu-lle-selftest domain-mode-selftest
	$(CC) -std=c99 -Wall -Wextra -Isrc/rt src/rt/pgf_public.c \
		tests/native/test_pgf_public.c -o build/test_pgf_public$(EXE_EXT)
	./build/test_pgf_public$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_core_catalog.c -o build/test_core_catalog$(EXE_EXT)
	./build/test_core_catalog$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_parsers_hostile.c -o build/test_parsers_hostile$(EXE_EXT)
	./build/test_parsers_hostile$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_manifest_parser.c -o build/test_manifest_parser$(EXE_EXT)
	./build/test_manifest_parser$(EXE_EXT) --check
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_launch_resolution.c -o build/test_launch_resolution$(EXE_EXT)
	./build/test_launch_resolution$(EXE_EXT)
	$(MAKE) --no-print-directory player-state-test-bin
	./build/test_player_state$(EXE_EXT)
	$(MAKE) --no-print-directory input-settings-test-bin
	./build/test_input_settings$(EXE_EXT)
	$(MAKE) --no-print-directory package-builder-test-bin
	./build/test_package_builder$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/rt -Isrc/player \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/player/setup_staging.c src/rt/archive_vfs.c \
		tests/native/test_xb_parser.c -o build/test_xb_parser$(EXE_EXT)
	./build/test_xb_parser$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/rt \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c \
		tests/native/test_fuzz_parsers.c -o build/test_fuzz_parsers$(EXE_EXT)
	./build/test_fuzz_parsers$(EXE_EXT) --iters 100
	$(MAKE) --no-print-directory psmf-producer-selftest PSMF_FUZZ_ITERS=100
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_input_profile.c -o build/test_input_profile$(EXE_EXT)
	./build/test_input_profile$(EXE_EXT)
ifeq ($(OS),Windows_NT)
	$(CC) -std=c99 -Wall -Wextra tests/native/argv_echo_helper.c -lshell32 -o build/argv_echo_helper$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_win32_process.c -o build/test_win32_process$(EXE_EXT)
	./build/test_win32_process$(EXE_EXT)
else
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) \
		tests/native/test_posix_process.c -o build/test_posix_process$(EXE_EXT)
	./build/test_posix_process$(EXE_EXT)
endif

FUZZ_ITERS ?= 5000
ifeq ($(OS),Windows_NT)
FUZZ_SAN_FLAGS :=
else
FUZZ_SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all
endif

fuzz-parsers:
	@$(PYTHON) -c "from pathlib import Path; Path('build').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra $(FUZZ_SAN_FLAGS) -Isrc/core -Isrc/core/generated -Isrc/rt \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c \
		tests/native/test_fuzz_parsers.c -o build/test_fuzz_parsers$(EXE_EXT)
	./build/test_fuzz_parsers$(EXE_EXT) --iters $(FUZZ_ITERS)
	$(MAKE) --no-print-directory psmf-producer-selftest \
		PSMF_FUZZ_ITERS=$(FUZZ_ITERS) FUZZ_SAN_FLAGS="$(FUZZ_SAN_FLAGS)"
	$(MAKE) --no-print-directory psmf-media-selftest \
		MEDIA_FUZZ_ITERS=$(FUZZ_ITERS) FUZZ_SAN_FLAGS="$(FUZZ_SAN_FLAGS)"
