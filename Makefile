# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
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
# The title link writes its object list with $(file ...), which needs GNU Make 4.0 or newer.
ifeq ($(filter 4.% 5.% 6.% 7.% 8.% 9.%,$(MAKE_VERSION)),)
$(error GNU Make 4.0 or newer is required (found $(MAKE_VERSION)); install a current GNU Make such as MSYS2 UCRT64 mingw32-make)
endif

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
# Declared values need no explicit export to reach recipes: a command-line value
# is passed through automatically, and an environment value passes with it. The
# `?=` default is different. Exporting it unconditionally put eboot.elf into every
# recipe's environment, so a make nested inside any repository recipe (the suite
# under `make test` or `make contrib-check`) saw origin GAME_ELF = environment,
# which GAME_INPUT_TRACKED below reads as "the operator declared an input", and
# the guest-input stamp was demanded for a lane that declares none. Export the
# default only when a file backs it -- exactly the wildcard branch of
# GAME_INPUT_TRACKED -- so a default that does not exist stays invisible to
# recipes and to every nested make, while a default that does exist is available
# to --env-elf the same way a declared value is.
ifeq ($(origin GAME_ELF),file)
ifneq ($(wildcard $(GAME_ELF)),)
export GAME_ELF
endif
else
export GAME_ELF
endif
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
# Quote the executable in recipes so explicit Windows paths with spaces work.
PYTHON     ?= python
ifeq ($(OS),Windows_NT)
POWERSHELL ?= powershell.exe
else
POWERSHELL ?= pwsh
endif
# glslc from the Vulkan SDK is used ONLY by the opt-in shaders target.
GLSLC ?= glslc

# BUILD AND LOG ROOTS. BUILD_ROOT is the shared build tree: the default per-title
# BUILD_DIR, the shared fixture trees clean-fixtures removes and the parse-time SDL3
# discovery cache all live beneath it, and clean-all empties it. LOG_DIR holds the
# ephemeral build and run logs distclean and clean-all remove; every other log there
# (oracle, evidence and private-run logs) is preserved. Overriding both points every
# lifecycle target, and every parse-time write those goals make, at another tree:
# that is how tools/test_build_truth.py exercises the destructive targets without
# touching a developer's real build/ and logs/.
BUILD_ROOT ?= build
LOG_DIR    ?= logs
# Every default output is created beneath BUILD_ROOT and clean-fixtures and clean-all
# delete beneath it, so an overridden root is refused, before the parse-time mkdir or
# any recipe runs, when it would reach the checkout itself: the repository root or one
# of its ancestors, or a directory inside the checkout other than its build/ tree
# (sources, place_game_here/, logs/, memstick/ ...). A root outside the checkout is the
# caller's explicit choice, exactly as BUILD_DIR is for `clean`. The default root needs
# no check, so an ordinary invocation starts no extra interpreter.
#
# This scope check runs BEFORE the space refusal below, so the more specific safety
# reason wins: in a checkout whose own path contains a space (which is supported), its
# parent directory is spaced too, and naming it must report "an ancestor of the
# repository root", not only that Make cannot spell it. The value travels to Python as
# one quoted argument, so a space in it cannot split it.
# The checkout-scope check, shared by BUILD_ROOT and an overridden BUILD_DIR. It prints
# `ok`, or the reason the tree reaches the checkout. The tree travels to Python as one
# quoted argument, so a space in it cannot split it. It is only ever run for a value the
# caller supplied (or the non-default root), so an ordinary invocation starts no extra
# interpreter.
_CHECKOUT_SCOPE_PY := import sys; from pathlib import Path; repo = Path.cwd().resolve(); root = Path(sys.argv[1]).resolve(); print('the repository root or one of its ancestors' if repo.is_relative_to(root) else 'inside the checkout but outside its build tree' if root.is_relative_to(repo) and not root.is_relative_to(repo / 'build') else 'ok')
ifneq ($(BUILD_ROOT),build)
_BUILD_ROOT_SCOPE := $(strip $(shell "$(PYTHON)" -c "$(_CHECKOUT_SCOPE_PY)" "$(BUILD_ROOT)"))
ifneq ($(_BUILD_ROOT_SCOPE),ok)
$(error BUILD_ROOT '$(BUILD_ROOT)' is $(or $(_BUILD_ROOT_SCOPE),uncheckable (the scope check printed nothing)); build outputs are created and clean-fixtures and clean-all delete beneath it, so it must be the checkout's build/ tree (the default), a directory beneath it, or a directory outside the checkout)
endif
endif
ifneq ($(subst $(SPACE),,$(BUILD_ROOT)),$(BUILD_ROOT))
$(error BUILD_ROOT '$(BUILD_ROOT)' contains a space, which GNU Make cannot represent in a target or prerequisite name. Use a BUILD_ROOT without spaces (the default is build))
endif
# How a recipe runs a native test binary built beneath BUILD_ROOT. The default keeps
# the `./build/...` spelling; an absolute or drive-letter root is run as given, since
# prefixing it with `./` names no file.
BUILD_ROOT_RUN := $(if $(filter /%,$(BUILD_ROOT))$(findstring :,$(BUILD_ROOT)),$(BUILD_ROOT),./$(BUILD_ROOT))

# SDL3 and Vulkan dependency discovery.
# Normal build parses resolve both in one Python startup. The fragment carries the
# same Make variables used by compiler/profile hashing; info-only and clean goals
# retain their existing lightweight SDL behavior.
SDL3_DIR ?=
SDL3_MAKE_FRAGMENT := $(BUILD_ROOT)/.sdl3-discovery.mk
ifeq ($(strip $(filter clean distclean clean-preview,$(MAKECMDGOALS))$(NK_INFO_ONLY)),)
ifeq ($(VULKAN_SDK),)
_BUILD_DEPENDENCY_DISCOVERY := $(shell "$(PYTHON)" -W ignore -c "import sys; sys.path.insert(0, 'tools'); from nk_doctor_checks import write_build_discovery_make_fragment; write_build_discovery_make_fragment(r'$(SDL3_MAKE_FRAGMENT)', r'$(subst \,/,$(SDL3_DIR))', r'$(CC)', r'$(subst \,/,$(VULKAN_SDK))', True)" 2>&1)
ifneq ($(strip $(_BUILD_DEPENDENCY_DISCOVERY)),)
$(error SDL3 discovery failed: $(_BUILD_DEPENDENCY_DISCOVERY))
endif
else
_SDL3_DISCOVERY := $(shell "$(PYTHON)" -W ignore -c "import sys; sys.path.insert(0, 'tools'); from nk_doctor_checks import write_sdl3_make_fragment; write_sdl3_make_fragment(r'$(SDL3_MAKE_FRAGMENT)', r'$(subst \,/,$(SDL3_DIR))', r'$(CC)')" 2>&1)
ifneq ($(strip $(_SDL3_DISCOVERY)),)
$(error SDL3 discovery failed: $(_SDL3_DISCOVERY))
endif
endif
VULKAN_SDK := $(subst \,/,$(VULKAN_SDK))
include $(SDL3_MAKE_FRAGMENT)

else
SDL3_PROVIDER := none
SDL3_VERSION := none
ifeq ($(VULKAN_SDK),)
VULKAN_SDK := $(shell "$(PYTHON)" -c "import sys; sys.path.insert(0, 'tools'); from vulkan_sdk import discover_vulkan_sdk, VulkanSdkError; (lambda: exec('try:\n print(discover_vulkan_sdk().as_posix())\nexcept VulkanSdkError:\n pass'))()")
endif
VULKAN_SDK := $(subst \,/,$(VULKAN_SDK))
endif

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
VULKAN_INC_FLAGS := -I"$(VULKAN_SDK)/Include" -I"$(VULKAN_SDK)/include"
VULKAN_LIB_FLAGS := -L"$(VULKAN_SDK)/Lib" -L"$(VULKAN_SDK)/lib"
else
VULKAN_INC_FLAGS := $(if $(strip $(VULKAN_SDK)),-I"$(VULKAN_SDK)/include",)
VULKAN_LIB_FLAGS := $(if $(strip $(VULKAN_SDK)),-L"$(VULKAN_SDK)/lib",) -lvulkan
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
LDFLAGS ?= $(SDL3_LDFLAGS) $(if $(VULKAN_SDK),-L"$(VULKAN_SDK)/Lib" -L"$(VULKAN_SDK)/lib",)
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
WIN_ONLY_LIBS   := -lmfplat -lgdi32 -lole32 -lshell32 -luuid -lwinmm
# -Wl,--no-insert-timestamp keeps the PE image byte-reproducible on the
# MinGW/COFF linker. GNU ld publishes no such option -- an ELF link is already
# timestamp-free -- so the flag is Windows-only and the Linux link line carries
# nothing in its place rather than a rejected option.
REPRODUCIBLE_LINK_FLAG := -Wl,--no-insert-timestamp
else
VULKAN_LIB_NAME := -lvulkan
WIN_ONLY_LIBS   :=
REPRODUCIBLE_LINK_FLAG :=
# MinGW resolves the maths routines from the C runtime it always links, so the
# Windows link line never names libm. A GNU/ELF link must name it: the runtime's
# transcendental maths (GE rasterizer, VFPU, ATRAC DSP) references libm
# symbols and ld refuses an implicit dependency ("DSO missing from command
# line"). Listed here, after the project libraries, exactly as -lm is on any
# other GNU link line.
POSIX_RUNTIME_LIBS := -lm
endif
LIBS       ?= -lSDL3 $(VULKAN_LIB_NAME) $(WIN_ONLY_LIBS) $(POSIX_RUNTIME_LIBS)

# Portable H.264 backend (src/rt/h264_ffmpeg.c): the pinned FFmpeg n4.4 H.264 decoder subset
# vendored under src/rt/h264_ffmpeg_sub/ (LGPL-2.1-or-later, generic C, statically linked, no
# system FFmpeg or dev headers). H264_FFMPEG=1 builds it and makes it the decoder where no
# platform decoder exists; it is the default off Windows, where Media Foundation (h264_mf.c)
# stays the backend. H264_FFMPEG=0 keeps h264_null.c failing closed. See NOTICE.md and
# docs/PLATFORM_PORTABILITY.md.
ifeq ($(OS),Windows_NT)
H264_FFMPEG ?= 0
else
H264_FFMPEG ?= 1
endif
H264_SUB_DIR := src/rt/h264_ffmpeg_sub
ifeq ($(H264_FFMPEG),1)
override CFLAGS += -DSR_H264_FFMPEG
H264_SUB_SRCS := src/rt/h264_ffmpeg_sub/nk_h264_api.c \
	src/rt/h264_ffmpeg_sub/libavcodec/allcodecs.c \
	src/rt/h264_ffmpeg_sub/libavcodec/atsc_a53.c \
	src/rt/h264_ffmpeg_sub/libavcodec/avcodec.c \
	src/rt/h264_ffmpeg_sub/libavcodec/avpacket.c \
	src/rt/h264_ffmpeg_sub/libavcodec/bitstream.c \
	src/rt/h264_ffmpeg_sub/libavcodec/bitstream_filters.c \
	src/rt/h264_ffmpeg_sub/libavcodec/bsf.c \
	src/rt/h264_ffmpeg_sub/libavcodec/cabac.c \
	src/rt/h264_ffmpeg_sub/libavcodec/codec_desc.c \
	src/rt/h264_ffmpeg_sub/libavcodec/codec_par.c \
	src/rt/h264_ffmpeg_sub/libavcodec/decode.c \
	src/rt/h264_ffmpeg_sub/libavcodec/encode.c \
	src/rt/h264_ffmpeg_sub/libavcodec/error_resilience.c \
	src/rt/h264_ffmpeg_sub/libavcodec/golomb.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h2645_parse.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_cabac.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_cavlc.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_direct.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_loopfilter.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_mb.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_parse.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_picture.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_ps.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_refs.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_sei.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264_slice.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264chroma.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264data.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264dec.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264dsp.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264idct.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264pred.c \
	src/rt/h264_ffmpeg_sub/libavcodec/h264qpel.c \
	src/rt/h264_ffmpeg_sub/libavcodec/mathtables.c \
	src/rt/h264_ffmpeg_sub/libavcodec/me_cmp.c \
	src/rt/h264_ffmpeg_sub/libavcodec/null_bsf.c \
	src/rt/h264_ffmpeg_sub/libavcodec/options.c \
	src/rt/h264_ffmpeg_sub/libavcodec/profiles.c \
	src/rt/h264_ffmpeg_sub/libavcodec/simple_idct.c \
	src/rt/h264_ffmpeg_sub/libavcodec/startcode.c \
	src/rt/h264_ffmpeg_sub/libavcodec/utils.c \
	src/rt/h264_ffmpeg_sub/libavcodec/videodsp.c \
	src/rt/h264_ffmpeg_sub/libavutil/avstring.c \
	src/rt/h264_ffmpeg_sub/libavutil/bprint.c \
	src/rt/h264_ffmpeg_sub/libavutil/buffer.c \
	src/rt/h264_ffmpeg_sub/libavutil/channel_layout.c \
	src/rt/h264_ffmpeg_sub/libavutil/dict.c \
	src/rt/h264_ffmpeg_sub/libavutil/display.c \
	src/rt/h264_ffmpeg_sub/libavutil/error.c \
	src/rt/h264_ffmpeg_sub/libavutil/eval.c \
	src/rt/h264_ffmpeg_sub/libavutil/fifo.c \
	src/rt/h264_ffmpeg_sub/libavutil/file_open.c \
	src/rt/h264_ffmpeg_sub/libavutil/frame.c \
	src/rt/h264_ffmpeg_sub/libavutil/hwcontext.c \
	src/rt/h264_ffmpeg_sub/libavutil/imgutils.c \
	src/rt/h264_ffmpeg_sub/libavutil/log.c \
	src/rt/h264_ffmpeg_sub/libavutil/log2_tab.c \
	src/rt/h264_ffmpeg_sub/libavutil/mathematics.c \
	src/rt/h264_ffmpeg_sub/libavutil/mem.c \
	src/rt/h264_ffmpeg_sub/libavutil/opt.c \
	src/rt/h264_ffmpeg_sub/libavutil/parseutils.c \
	src/rt/h264_ffmpeg_sub/libavutil/pixdesc.c \
	src/rt/h264_ffmpeg_sub/libavutil/random_seed.c \
	src/rt/h264_ffmpeg_sub/libavutil/rational.c \
	src/rt/h264_ffmpeg_sub/libavutil/reverse.c \
	src/rt/h264_ffmpeg_sub/libavutil/samplefmt.c \
	src/rt/h264_ffmpeg_sub/libavutil/sha.c \
	src/rt/h264_ffmpeg_sub/libavutil/stereo3d.c \
	src/rt/h264_ffmpeg_sub/libavutil/time.c \
	src/rt/h264_ffmpeg_sub/libavutil/timecode.c \
	src/rt/h264_ffmpeg_sub/libavutil/utils.c \
	src/rt/h264_ffmpeg_sub/libavutil/video_enc_params.c
else
H264_SUB_SRCS :=
endif

BUILD_DIR  ?= $(BUILD_ROOT)/$(GAME_NAME)
# An overridden BUILD_DIR gets the BUILD_ROOT scope refusal. `make clean` deletes it and
# every build writes its outputs into it, so `BUILD_DIR=.` or `BUILD_DIR=src` must stop
# the run while Make parses, before any recipe runs. The default derives from the
# BUILD_ROOT checked above, so it needs no second interpreter. Like BUILD_ROOT, a
# directory outside the checkout is the caller's explicit choice.
ifneq ($(origin BUILD_DIR),file)
_BUILD_DIR_SCOPE := $(strip $(shell "$(PYTHON)" -c "$(_CHECKOUT_SCOPE_PY)" "$(subst \,/,$(BUILD_DIR))"))
ifneq ($(_BUILD_DIR_SCOPE),ok)
$(error BUILD_DIR '$(BUILD_DIR)' is $(or $(_BUILD_DIR_SCOPE),uncheckable (the scope check printed nothing)); clean deletes it and every build writes its outputs into it, so it must be a directory beneath the checkout's build/ tree or a directory outside the checkout)
endif
endif
# clean and clean-all delete $(BUILD_DIR) wholesale (clean-all runs clean first), so a
# BUILD_DIR that is the whole build root would delete every title's build beneath it:
# `make clean BUILD_DIR=build` did exactly that. The root is BUILD_ROOT, and also the
# checkout's own build/ tree, which a scratch BUILD_ROOT leaves in place. The two paths
# are compared normalized (the value reaches Python with / for \, and _BUILD_DIR_ROOT_PY
# resolves it and folds case on Windows), so `build`, `build/`, `./build`, an absolute path
# and backslash spellings are all refused. Parse-time, like the scope check above. A
# per-title directory beneath the root still cleans, and clean-all still empties BUILD_ROOT.
_BUILD_DIR_ROOT_PY := import os, sys; from pathlib import Path; norm = lambda p: os.path.normcase(str(Path(p).resolve())); repo = Path.cwd().resolve(); print('root' if norm(sys.argv[1]) in (norm(sys.argv[2]), norm(str(repo / 'build'))) else 'ok')
ifneq ($(strip $(filter clean clean-all,$(MAKECMDGOALS))),)
_BUILD_DIR_ROOT := $(strip $(shell "$(PYTHON)" -c "$(_BUILD_DIR_ROOT_PY)" "$(subst \,/,$(BUILD_DIR))" "$(subst \,/,$(BUILD_ROOT))"))
ifneq ($(_BUILD_DIR_ROOT),ok)
$(error BUILD_DIR '$(BUILD_DIR)' is $(if $(filter root,$(_BUILD_DIR_ROOT)),the whole build root (BUILD_ROOT '$(BUILD_ROOT)' or the checkout's build/ tree),uncheckable (the root check printed nothing)); clean deletes BUILD_DIR wholesale, which would remove every title's build beneath it. Set BUILD_DIR to one title's directory beneath the root (the default is BUILD_ROOT/<game>), or leave BUILD_DIR unset. To empty the whole BUILD_ROOT on purpose, run clean-all without BUILD_DIR)
endif
endif
# Refuse a BUILD_DIR GNU Make cannot represent, before any target name is derived
# from it and before the parse-time mkdir below runs. Make splits a target or
# prerequisite name on whitespace, so a BUILD_DIR containing a space silently
# becomes several targets: `clean` then removes the FIRST fragment -- a directory
# the caller never named -- and the parse-time mkdir creates directories named
# after the remaining fragments in the repository root. Failing closed here names
# the boundary instead of corrupting the tree.
#
# The repository ROOT may contain spaces: BUILD_DIR defaults to the relative
# `$(BUILD_ROOT)/$(GAME_NAME)` (`build/<game>`), which carries none; every
# parse-time $(shell) call quotes "$(PYTHON)" so no command interpreter re-splits
# a spaced path; and the -I/-L search paths are quoted before they reach the
# compiler or the profile hasher, whose entries travel through the environment
# rather than argv (#667).
# tools/title_codegen_plan.py already picks a Make-safe build root
# (NK_BUILD_ROOT, then the 8.3 short name) for every route that accepts an
# operator-chosen output directory (#296).
ifneq ($(subst $(SPACE),,$(BUILD_DIR)),$(BUILD_DIR))
$(error BUILD_DIR '$(BUILD_DIR)' contains a space, which GNU Make cannot represent in a target or prerequisite name. Use a relative BUILD_DIR under the repository root (the default `build/<game>`), or set NK_BUILD_ROOT to a folder without spaces so tools/title_codegen_plan.py can pick a Make-safe build root (issue #296).)
endif
# No compile carries the build directory: the runtime writes its diagnostic exit artifacts
# (crash dump, exit flag) beside its own executable, found at run time, so one compiled
# object serves every build folder and the shared object cache can reuse it (#1306).
FUNCS_PER_CHUNK ?= 2000
# Optional deterministic size-aware chunking: greedy contiguous fill toward a
# per-chunk emitted-byte budget (function order preserved, FUNCS_PER_CHUNK still
# caps chunk size). Empty keeps the legacy count-based partition byte-for-byte.
CHUNK_TARGET_BYTES ?=
ifeq ($(strip $(CHUNK_TARGET_BYTES)),)
CHUNK_BYTES_ARG :=
CHUNK_TARGET_PROFILE_LINE :=
else
CHUNK_BYTES_ARG := --target-chunk-bytes=$(CHUNK_TARGET_BYTES)
CHUNK_TARGET_PROFILE_LINE := $(NEWLINE)CHUNK_TARGET_BYTES=$(CHUNK_TARGET_BYTES)
endif
# Optional guest symbol map (tools/codegen.py --symbol-map, docs/DEBUGGING.md): generated
# functions, crash reports and host profiles show the map's names. The map is a codegen
# prerequisite, so editing it regenerates; empty keeps the output and the profile unchanged.
SYMBOL_MAP ?=
ifeq ($(strip $(SYMBOL_MAP)),)
SYMBOL_MAP_ARG :=
SYMBOL_MAP_PROFILE_LINE :=
else
SYMBOL_MAP_ARG := --symbol-map=$(SYMBOL_MAP)
SYMBOL_MAP_PROFILE_LINE := $(NEWLINE)SYMBOL_MAP=$(SYMBOL_MAP)
endif
# NAME_IMPORTS=1 names each import stub after the function its NID imports (tools/codegen.py
# --name-imports), from the NID corpus src/rt/nid_names.h is generated from. The corpus is then a
# codegen prerequisite; the default (0) keeps the output and the profile unchanged.
NAME_IMPORTS ?= 0
ifeq ($(NAME_IMPORTS),1)
NAME_IMPORTS_ARG := --name-imports
NAME_IMPORTS_PREREQ := tools/nid_corpus.json tools/gen_nidnames.py
NAME_IMPORTS_PROFILE_LINE := $(NEWLINE)NAME_IMPORTS=1
else
NAME_IMPORTS_ARG :=
NAME_IMPORTS_PREREQ :=
NAME_IMPORTS_PROFILE_LINE :=
endif
# Optional declarative enhancement package (e.g. 60 Hz frame-rate rescale).
# Empty keeps authentic translation and profile stamps byte-identical.
ENHANCEMENT_PACKAGE ?=
ifeq ($(strip $(ENHANCEMENT_PACKAGE)),)
ENHANCEMENT_PACKAGE_ARG :=
ENHANCEMENT_PACKAGE_PROFILE_LINE :=
else
ENHANCEMENT_PACKAGE_ARG := --enhancement-package=$(ENHANCEMENT_PACKAGE)
ENHANCEMENT_PACKAGE_PROFILE_LINE := $(NEWLINE)ENHANCEMENT_PACKAGE=$(ENHANCEMENT_PACKAGE)
endif
# The title's extracted files, where a package's data_files are read and checked.
# Only a package build passes it; the package digest already names those files.
ENHANCEMENT_DATA_ROOT ?=
ifneq ($(and $(strip $(ENHANCEMENT_PACKAGE)),$(strip $(ENHANCEMENT_DATA_ROOT))),)
ENHANCEMENT_PACKAGE_ARG += --enhancement-data-root=$(ENHANCEMENT_DATA_ROOT)
endif

# ---------------------------------------------------------------------------
# BUILD IDENTITY: the source revision every commit-stamped artifact records --
# the flight recorder's build.build_id (#532) and both PSP oracle records.
#
# `GIT` names the git executable, for a host that keeps it off PATH, and the
# revision is resolved ONCE here. Every default below reads that one value, and
# a caller that supplies the identity itself (a hosted job passing
# SR_SOURCE_COMMIT=${{ github.sha }}) never reaches for git at all.
#
# The resolution runs through the interpreter this build already requires at
# parse time (see the VULKAN_SDK discovery above) and never through a bare
# `$(shell git ...)`: make spawns a missing program itself, so on a host without
# git the old default logged `process_begin: CreateProcess(NULL, git
# rev-parse HEAD, ...) failed` and then quietly built a binary with NO recorded
# identity. The Windows runtime compile gate runs in an MSYS2 shell with no git
# on PATH and hit exactly that. A host with no git now resolves an empty
# identity and a clean log, which is the documented state for a source drop:
# tools/flight_diff.py refuses an identity-less bundle fail closed, and it never
# falls back to a clock.
GIT ?= git
ifndef NK_INFO_ONLY
NK_GIT_REV_HEAD := $(strip $(shell "$(PYTHON)" -c "import shutil, subprocess, sys; exe = shutil.which(sys.argv[1]); print(subprocess.run([exe, 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip() if exe else '')" "$(GIT)"))
endif

# Production-HLE PSP oracle stream. The target reuses hle_thread_selftest.exe, so the
# binary_sha256 in its record is the hash of the executable that actually emits stdout.
PSP_ORACLE_CASE          ?= callback-notify-check
PSP_ORACLE_SOURCE_COMMIT ?= $(NK_GIT_REV_HEAD)
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
PRODUCTION_SMOKE_DIR       := $(BUILD_ROOT)/production-smoke
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
DISPLAY_SMOKE_DIR       := $(BUILD_ROOT)/$(DISPLAY_SMOKE_NAME)
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
# through the ordinary production dispatch() seam. The address must match
# HELPER in fixtures/production_smoke/generate.py (it moved to 0x08804068 when
# the sceImpose language round trip extended region A).
PRODUCTION_SMOKE_GAP_DIR       := $(BUILD_ROOT)/production-smoke-gap
PRODUCTION_SMOKE_GAP_FIXTURE   := $(PRODUCTION_SMOKE_GAP_DIR)/fixture
PRODUCTION_SMOKE_GAP_MAP       := $(PRODUCTION_SMOKE_GAP_DIR)/production_smoke_gap.map
PRODUCTION_SMOKE_GAP_CODEGEN_ARGS := --omit-aot=0x08804068

# Caller-supplied extra codegen arguments (build-time codegen choices such as
# the smoke's --omit-aot). Empty by default; carried into the codegen profile
# hash so changing it regenerates instead of reusing stale output.
CODEGEN_USER_ARGS ?=

# Stack census declarations and runtime support are compiled only for builds
# whose generated code contains the opt-in instrumentation.
STACK_CENSUS_CFLAG :=
ifneq ($(filter --stack-census,$(CODEGEN_USER_ARGS)),)
STACK_CENSUS_CFLAG := -DSR_STACK_CENSUS_ENABLED
override CFLAGS += $(STACK_CENSUS_CFLAG)
endif

# Opt-in LLVM IR backend (#330, docs/LLVM_BACKEND.md). CODEGEN_BACKEND=llvm makes
# codegen lower every function it supports to LLVM IR (<game>_recomp_<n>.ll) and keep
# C for the rest, with each reason in <game>_recomp_llvm.json. Each .ll is linked with
# the runtime ABI bitcode (src/rt/llvm_abi.c) by LLVM_LINK, which also runs LLVM's
# verifier, and compiled by LLVM_CLANG at RECOMP_OPT. The toolchain identity joins the
# generated-code profile only in an LLVM build, so a C build's profile is unchanged.
CODEGEN_BACKEND ?= c
LLVM_CLANG ?= clang
LLVM_LINK ?= llvm-link
RECOMP_LLVM_PROFILE_ENTRY :=
ifeq ($(CODEGEN_BACKEND),llvm)
override CODEGEN_USER_ARGS += --backend=llvm
RECOMP_LLVM_PROFILE_ENTRY := LLVM_TOOLCHAIN=$(strip $(shell "$(LLVM_CLANG)" --version 2>&1) / $(shell "$(LLVM_LINK)" --version 2>&1))
else ifneq ($(CODEGEN_BACKEND),c)
$(error CODEGEN_BACKEND must be c or llvm, not $(CODEGEN_BACKEND))
endif
RECOMP_LLVM_HASH_ARG := $(if $(RECOMP_LLVM_PROFILE_ENTRY),--recomp-entry "$(RECOMP_LLVM_PROFILE_ENTRY)",)
RECOMP_LLVM_ENTRY_ARG := $(if $(RECOMP_LLVM_PROFILE_ENTRY),--entry "$(RECOMP_LLVM_PROFILE_ENTRY)",)

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

# The runtime builds only the public backends: the project-authored PGF reader and
# ISO backend, the fail-closed PGD/amctrl backend and the SDL3 host-audio backend. The
# private-backend mode (PUBLIC_SAFE=0) was retired on 2026-10-25; PUBLIC_SAFE=1 stays
# accepted so existing command lines keep working.
PUBLIC_SAFE ?= 1
ifneq ($(PUBLIC_SAFE),1)
$(error PUBLIC_SAFE=0 was retired; the runtime builds only the public backends)
endif
PGF_BACKEND_SRC := src/rt/pgf_public.c
PGD_BACKEND_SRC := src/rt/pgd_unavailable.c
ISO_BACKEND_SRC := src/rt/iso_public.c
AUDIO_BACKEND_SRC := src/rt/audio_sdl3.c
ASSET_COPY_ARGS := -ExcludeOptionalFonts
# `override` on purpose: a plain += is discarded when a caller passes CFLAGS on the command
# line, and once any append to CFLAGS uses override, GNU make ignores later ordinary
# assignments to it. Losing this define would make the build stop declaring itself
# public-safe, and the production-smoke evidence check fails on exactly that.
override CFLAGS += -DSR_PUBLIC_SAFE

include mk/build_common.mk
include mk/nk_ui.mk

BUILD_PROFILE_TOOL := tools/build_profile.py
CPU_STATE_ABI_HEADER := src/rt/recomp.h

include mk/title_config.mk

include mk/runtime_sources.mk

include mk/public_targets.mk

include mk/diagnostics.mk

include mk/smoke_gates.mk

include mk/pipeline.mk

include mk/cosim_snapshot.mk

include mk/housekeeping.mk

include mk/selftests.mk

include mk/fuzz_verify.mk

include mk/native_ui_tests.mk
