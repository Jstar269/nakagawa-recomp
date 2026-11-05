# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: object lists, link flags and the HLE, presenter and vendored-codec source sets.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# HLE ATRAC decode (h_AtracDecodeData). The codec-specific decoder bridges
# and imported FFmpeg n4.4 decoder TUs
# join the runtime link; the include paths are relative to src/rt/atrac3p/.
# Objects are prefixed so nested import paths cannot collide with flat
# runtime object names.
ATRAC3P_OBJS := $(patsubst src/rt/atrac3p/%.c,$(BUILD_DIR)/atrac3p_%.o,$(ATRAC3P_SRCS))
ATRAC3P_OBJ_DIRS := $(sort $(patsubst %/,%,$(dir $(ATRAC3P_OBJS))))

# Vendored FFmpeg H.264 subset objects, prefixed like the ATRAC3+ ones. They are appended to
# RT_OBJS below so every runtime link that carries h264_ffmpeg.o carries its decoder.
H264_SUB_OBJS := $(patsubst src/rt/h264_ffmpeg_sub/%.c,$(BUILD_DIR)/h264sub_%.o,$(H264_SUB_SRCS))
H264_SUB_OBJ_DIRS := $(sort $(patsubst %/,%,$(dir $(H264_SUB_OBJS))))

# Ensure the build directory and nested object directories exist up front so no
# per-recipe mkdir is needed.
# Use Python for fully portable directory creation across Windows cmd.exe, MSYS2,
# PowerShell, and POSIX environments. A clean preview must not create the state
# it is about to plan over, so it skips this parse-time mkdir entirely.
ifeq ($(strip $(filter clean-preview,$(MAKECMDGOALS))),)
ifndef NK_INFO_ONLY
_MKDIRS := $(shell "$(PYTHON)" -c "import os, sys; [os.makedirs(d, exist_ok=True) for d in sys.argv[1:]]" "$(BUILD_DIR)" "$(BUILD_DIR)/portable-core" $(ATRAC3P_OBJ_DIRS) $(H264_SUB_OBJ_DIRS))
NK_BUILD_DIR_READY := 1
endif
endif

ifeq ($(OS),Windows_NT)
PLAYER_PLATFORM_SRC := src/core/nk_platform_win32.c
PLAYER_PLATFORM_LIBS := -lshell32 -lole32 -luuid
PLAYER_EXTRA_LIBS   := $(PLAYER_PLATFORM_LIBS) -lwinhttp -lbcrypt
PLAYER_PLAT_SOURCES := $(PLAYER_PLATFORM_SRC)
# The Windows player is a GUI-subsystem program (-mwindows) so a double-click opens no console window;
# main.c attaches to a parent terminal console or logs to the logs folder. The UI test binary keeps
# the console subsystem because its harness reads stdout.
PLAYER_GUI_LDFLAGS  := -mwindows
# The app icon (assets/brand/nakagawa.rc) and the VERSIONINFO that gives Task Manager and
# Explorer a friendly name, as one Windows resource object linked into the player and each
# title executable; SDL gives every window the executable's first icon group. Each object
# is compiled from its own generated script (tools/make_app_icon.py) because the
# player and every title name themselves differently. Other systems set the same picture
# at run time (nk_ui_app_icon_rgba in src/ui/nk_ui_widgets.c), so they link no object.
WINDRES             ?= windres
PLAYER_ICON_OBJ     := $(BUILD_ROOT)/nakagawa_icon.o
TITLE_ICON_OBJ      = $(BUILD_DIR)/nakagawa_icon.o
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
PLAYER_VULKAN_INC   := -I"$(VULKAN_SDK)/Include" -I"$(VULKAN_SDK)/include"
PLAYER_VULKAN_LIB   :=
EXE_EXT             := .exe
# Post-link asset copy for the runtime `compile` link. On Windows this is the
# existing PowerShell step, so the Windows link line stays byte-identical. On
# Linux there is no SDL3.dll to stage and the runtime resolves font/ through
# its own executable-anchored root, so the step is absent rather than emulated
# by a second copy script. A missing asset is reported by the runtime itself
# (font_load / data walk), never silently ignored here.
ASSET_COPY_STEP     = $(POWERSHELL) -NoProfile -ExecutionPolicy Bypass -File tools/copy_build_assets.ps1 -BuildDir "$(BUILD_DIR)" -ProjectFontDir "$(PROJECT_FONT_GENERATED_DIR)" -Sdl3DllPath "$(SDL3_DLL)" $(ASSET_COPY_ARGS)
else
PLAYER_PLATFORM_SRC := src/core/nk_platform_posix.c
PLAYER_PLATFORM_LIBS :=
PLAYER_GUI_LDFLAGS  :=
PLAYER_ICON_OBJ     :=
TITLE_ICON_OBJ      :=
# The nk_ui draw and wave code calls libm (exp, sinf); glibc keeps it out of libc.
PLAYER_EXTRA_LIBS   := -lm
PLAYER_PLAT_SOURCES := $(PLAYER_PLATFORM_SRC)
PLAYER_VULKAN_INC   := $(VULKAN_INC_FLAGS)
PLAYER_VULKAN_LIB   :=
EXE_EXT             :=
ASSET_COPY_STEP     =
endif

RT_GE_O    := $(BUILD_DIR)/ge.o
# The pause overlay (#1159): nk_ui, its SDL edge and SDL_Renderer backend, the overlay's pages
# and their runtime host; Ace Mode's in-game UI draws through the Vulkan backend.
# and their runtime host. SDL3_ttf is loaded at run time, so the runtime needs only its headers.
RT_NK_UI_SRCS := $(NK_UI_CORE_SRCS) $(NK_UI_EDGE_SRCS) src/ui/backend/nk_ui_sdlr.c \
                 src/ui/backend/nk_ui_vk.c \
                 $(NK_UI_OVERLAY_C9C_SRCS) $(NK_UI_OVERLAY_HOST_SRCS)

# The runtime HLE translation units (#307), in one place: every recipe that links HLE compiles
# or links all of them, with that recipe's own defines. tools/hle_sources.py lists the same files
# and tools/test_hle_sources.py fails when the two lists differ. HLE_HDRS are their private headers.
HLE_SRCS   := src/rt/hle.c src/rt/hle_audio.c src/rt/hle_audio_codec.c src/rt/hle_cache_dma.c \
              src/rt/hle_data.c src/rt/hle_display.c src/rt/hle_font.c src/rt/hle_ge.c \
              src/rt/hle_host.c src/rt/hle_input.c src/rt/hle_io.c src/rt/hle_kernel_interrupt.c \
              src/rt/hle_kernel_memory.c src/rt/hle_kernel_module.c src/rt/hle_kernel_sync.c \
              src/rt/hle_kernel_thread.c src/rt/hle_kernel_time.c src/rt/hle_kernel_utils.c \
              src/rt/hle_mpeg.c src/rt/hle_network.c src/rt/hle_npdrm.c src/rt/hle_power.c \
              src/rt/hle_route.c src/rt/hle_rtc.c src/rt/hle_system.c src/rt/hle_umd.c \
              src/rt/hle_utility.c
HLE_HDRS   := src/rt/hle_internal.h src/rt/hle_power.h
HLE_OBJS   := $(addprefix $(BUILD_DIR)/,$(notdir $(HLE_SRCS:.c=.o)))

RT_SRCS    := src/rt/recomp.c \
              src/rt/flight_recorder.c \
              src/rt/ace.c \
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
              src/rt/fbcap.c \
              src/rt/ge_capture.c \
              src/rt/vfpu_interp.c \
              $(HLE_SRCS) src/rt/audio_resample.c src/rt/flash0_font.c \
              src/rt/mp3_bridge.c \
              src/core/nk_xb.c \
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
              src/rt/h264_null.c src/rt/h264_ffmpeg.c \
              src/rt/savedata.c \
              src/rt/osk_win.c \
              src/rt/osk_text_entry.c \
              src/rt/osk_overlay.c \
              src/rt/osk_overlay_paint.c \
              src/rt/driver.c \
              src/rt/gpu_sdl3vk/sdl3vk.c \
              src/rt/gpu_sdl3vk/ge_gpu.c \
              src/rt/gpu_sdl3vk/ace_ui.c \
              src/rt/gpu_sdl3vk/ace_smooth.c \
              src/rt/gpu_sdl3vk/fsr1_pass.c \
              src/core/nk_input_profile.c \
              src/core/nk_font.c \
              src/core/nk_json.c \
              $(RT_NK_UI_SRCS) \
              $(PLAYER_PLATFORM_SRC)

# Verify build (#1843): `make VERIFY=1 ...` compiles the sampled in-play checker (src/rt/verify.c)
# and its hooks into the runtime and the generated code, and links the reference interpreter
# (src/ref) beside it. It is off by default and the default objects do not change. Run the
# resulting player with SR_VERIFY=k and SR_VERIFY_REPORT=file (src/rt/verify.h lists the
# settings). The reference's sr_bitrev is renamed in its own object, as for snapshot-selftest.
VERIFY ?= 0
ifeq ($(VERIFY),1)
override CFLAGS += -DSR_VERIFY
RT_SRCS += src/rt/verify.c src/rt/nksnap.c
override LIBS += -lstdc++
VERIFY_REF_OBJS := $(BUILD_DIR)/verify_ref_interp.o $(BUILD_DIR)/verify_ref.o
$(BUILD_DIR)/verify_ref_interp.o: src/ref/interp.cpp
	$(CXX) -std=c++17 -O1 -Isrc/ref -Isrc/rt -fno-exceptions -Dsr_bitrev=sr_ref_bitrev -c $< -o $@
$(BUILD_DIR)/verify_ref.o: src/ref/snapshot_ref.cpp src/ref/snapshot_ref.h
	$(CXX) -std=c++17 -O1 -Isrc/ref -Isrc/rt -fno-exceptions -Dsr_bitrev=sr_ref_bitrev -c $< -o $@
endif

RT_OBJS   := $(addprefix $(BUILD_DIR)/,$(notdir $(RT_SRCS:.c=.o)))
RT_OBJS    += $(H264_SUB_OBJS)
ifeq ($(VERIFY),1)
RT_OBJS    += $(VERIFY_REF_OBJS)
endif

# Everything needed to COMPILE the HLE sources ($(HLE_SRCS)), in one place. They reach into
# the ATRAC decode bridges, so every target that compiles them needs these
# include paths -- not just the $(HLE_OBJS) rule. Keeping them in a
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
# The presenter also publishes its captures through the presenter-neutral
# capture service (sr_capture_*, src/rt/fbcap.c), the same hard dependency.
# tools/test_build_truth.py enforces that every user of sdl3vk.c supplies both.
# Ace Mode's run switch (src/rt/ace.c) decides whether the device asks for anisotropic filtering.
# Its event pump runs through sr_coro_call_on_thread_stack (src/rt/sr_coro.c), which Android
# needs for JNI; a selftest without coroutines calls straight through it.
SDL3VK_SRCS := src/rt/gpu_sdl3vk/sdl3vk.c src/rt/gpu_sdl3vk/fsr1_pass.c src/rt/fbcap_policy.c src/rt/fbcap.c src/rt/sr_coro.c src/rt/ace.c \
               src/core/nk_input_profile.c src/core/nk_json.c $(PLAYER_PLATFORM_SRC)

$(BUILD_DIR)/atrac3p_%.o: src/rt/atrac3p/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(DEPFLAGS) -c $< -o $@

# Imported FFmpeg n4.4 H.264 subset TUs. -w: upstream code, kept byte-identical, is not held to the
# project warning set. HAVE_AV_CONFIG_H and the feature-test macros are what FFmpeg's own build
# passes; nk_h264_symbols.h renames the few symbols shared with the ATRAC3+ subset.
$(BUILD_DIR)/h264sub_%.o: src/rt/h264_ffmpeg_sub/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) -I$(H264_SUB_DIR) $(CFLAGS) -w -std=c11 -DHAVE_AV_CONFIG_H -D_ISOC99_SOURCE -D_FILE_OFFSET_BITS=64 \
		-D_LARGEFILE_SOURCE -D_POSIX_C_SOURCE=200112 -D_XOPEN_SOURCE=600 \
		-include $(H264_SUB_DIR)/nk_h264_symbols.h $(DEPFLAGS) -c $< -o $@

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
# compile: a -D define.
#   * SOURCE_DATE_EPOCH, when the build sets it, is honoured as-is (the
#     reproducible-builds.org convention) and recorded as build.source_date_epoch.
#   * The source commit is recorded as build.build_id, resolved exactly like
#     PSP_ORACLE_SOURCE_COMMIT below (one guarded $(GIT) rev-parse HEAD, or the
#     identity the caller supplied). A source drop without
#     git and without SOURCE_DATE_EPOCH records no identity and tools/flight_diff.py
#     refuses such a bundle fail closed; it never falls back to a clock.
# The runtime profile hash is deliberately NOT the identity here: it hashes
# CFLAGS, which carries the SDK paths, so it varies with the host's toolchain
# layout and would reintroduce that variation into every binary. The
# identity defines are kept out of the global CFLAGS (and so out of
# RUNTIME_PROFILE_HASH): only flight_recorder.o embeds them, so a new commit
# rebuilds that one object instead of every runtime object (see
# FLIGHT_IDENTITY_STAMP below).
ifndef NK_INFO_ONLY
SR_SOURCE_COMMIT ?= $(NK_GIT_REV_HEAD)
SR_SOURCE_COMMIT := $(strip $(SR_SOURCE_COMMIT))
ifneq ($(strip $(SR_SOURCE_COMMIT)),)
FLIGHT_IDENTITY_DEFS += -DSR_BUILD_ID=\"$(SR_SOURCE_COMMIT)\"
endif
ifneq ($(strip $(SOURCE_DATE_EPOCH)),)
# Transported through the environment (see the guest-input transport above) so
# no operator value reaches a command interpreter as syntax. Fail closed on a
# non-decimal value: flight_recorder.c emits it as a JSON number.
export SOURCE_DATE_EPOCH
SR_SOURCE_DATE_EPOCH := $(shell "$(PYTHON)" -c "import os; v = os.environ.get('SOURCE_DATE_EPOCH', ''); print(v if v.isdigit() else '')")
ifeq ($(strip $(SR_SOURCE_DATE_EPOCH)),)
$(error SOURCE_DATE_EPOCH must be a decimal Unix timestamp (reproducible-builds.org), got "$(SOURCE_DATE_EPOCH)")
endif
FLIGHT_IDENTITY_DEFS += -DSR_SOURCE_DATE_EPOCH=$(SR_SOURCE_DATE_EPOCH)
endif
endif
