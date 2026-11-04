# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: guest-input tracking, the codegen pipeline, the player link and the player UI regressions.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# Re-checked on every invocation that needs a guest input (hence FORCE), but rewritten
# only when an input's identity actually changed, so dependents do not rebuild spuriously.
# GAME_PSP_HEADER and GAME_EXTRA_ELFS_ENV are optional; GAME_ELF is not.
$(GAME_INPUT_STAMP): $(BUILD_PROFILE_TOOL) FORCE
	"$(PYTHON)" $(BUILD_PROFILE_TOOL) stamp-inputs --env GAME_ELF --optional-env GAME_PSP_HEADER --optional-env GAME_EXTRA_ELFS_ENV --list-env GAME_EXTRA_ELFS_ENV --out $@

FORCE:

pipeline: $(BUILD_DIR)/$(GAME_NAME)_image.bin $(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_imports.toml

$(BUILD_DIR)/$(GAME_NAME)_image.bin: $(GAME_INPUT_PREREQ) tools/prxload.py
	"$(PYTHON)" tools/prxload.py --env-elf $(GAME_BASE) $(PSP_HEADER_ENV_ARG) --out=$@

ifeq ($(NK_AOT_PREGENERATED),1)
$(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h:
	$(if $(wildcard $@),,$(error NK_AOT_PREGENERATED requires $@))
else
$(BUILD_DIR)/$(GAME_NAME)_recomp.c $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h: $(GAME_INPUT_PREREQ) $(CODEGEN_TOOL) tools/codegen.py tools/analyze.py tools/analyze_cfg.py tools/entry_frame_balance.py tools/prxload.py tools/host_stubs.py tools/codegen_hst_stubs.py tools/imports.py $(CODEGEN_PROFILE_STAMP) $(SYMBOL_MAP) $(NAME_IMPORTS_PREREQ)
	"$(PYTHON)" $(CODEGEN_TOOL) --env-elf $(BUILD_DIR)/$(GAME_NAME)_recomp.c --base=$(GAME_BASE) $(CODEGEN_PROFILE_ARG) $(EXTRA_SPAN_ARG) $(EXTRA_ELF_ENV_ARG) --funcs-per-chunk=$(FUNCS_PER_CHUNK) $(CHUNK_BYTES_ARG) $(SYMBOL_MAP_ARG) $(NAME_IMPORTS_ARG) $(ENHANCEMENT_PACKAGE_ARG) $(CODEGEN_USER_ARGS)
endif

$(BUILD_DIR)/$(GAME_NAME)_imports.toml: $(GAME_INPUT_PREREQ) tools/imports.py tools/analyze.py tools/analyze_cfg.py tools/prxload.py
	"$(PYTHON)" tools/imports.py --env-elf $(GAME_BASE) --toml=$@

ifndef NK_BUILD_DIR_READY
RUNTIME_PROFILE_HASH := $(call profile_hash,NK_RUNTIME_PROFILE_ENTRIES,$(CC),$(BUILD_DIR)/.runtime-profile-entries)
endif
RUNTIME_PROFILE_STAMP := $(BUILD_DIR)/.runtime-profile-$(RUNTIME_PROFILE_HASH)
# The objects the runtime profile invalidates go in a list file that make writes (see the record
# below): one --invalidate per object repeats the whole BUILD_DIR prefix, and under a long
# scratch BUILD_ROOT that line overran the shell's limit and was cut without an error.
RUNTIME_INVALIDATE_LIST := $(BUILD_DIR)/.runtime-invalidate.lst

$(RUNTIME_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	$(file >$(RUNTIME_INVALIDATE_LIST),$(RT_GE_O) $(RT_OBJS))
	"$(PYTHON)" $(BUILD_PROFILE_TOOL) record --output "$(RUNTIME_PROFILE_MANIFEST)" --section runtime --compiler "$(CC)" --entries-env NK_RUNTIME_PROFILE_ENTRIES --file "$(CPU_STATE_ABI_HEADER)" --stamp "$@" --stale-glob ".runtime-profile-*" --invalidate-file "$(RUNTIME_INVALIDATE_LIST)"

$(RT_GE_O): src/rt/ge.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(GE_CFLAGS) $(DEPFLAGS) -c src/rt/ge.c -o $@

# Make the object flavour explicit. Switching TRACE forces only the generated
# chunks to rebuild, avoiding a stale trace-enabled object in a release binary
# (or a trace-disabled object in an oracle run).
TRACE_STAMP := $(BUILD_DIR)/.recomp-trace-$(TRACE)
$(TRACE_STAMP):
	"$(PYTHON)" $(BUILD_PROFILE_TOOL) stamp --output "$@" --stale-glob ".recomp-trace-*" --value "$(TRACE)"

ifndef NK_BUILD_DIR_READY
RECOMP_PROFILE_HASH := $(shell "$(PYTHON)" $(BUILD_PROFILE_TOOL) hash --compiler "$(CC)" --entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --entry "TRACE=$(TRACE)" --entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" $(RECOMP_LLVM_ENTRY_ARG) --file "$(CPU_STATE_ABI_HEADER)")
endif
RECOMP_PROFILE_STAMP := $(BUILD_DIR)/.recomp-profile-$(RECOMP_PROFILE_HASH)
$(RECOMP_PROFILE_STAMP): $(BUILD_PROFILE_TOOL)
	"$(PYTHON)" $(BUILD_PROFILE_TOOL) record --output "$(RECOMP_PROFILE_MANIFEST)" --section generated --compiler "$(CC)" --entry "RECOMP_FLAGS=$(RECOMP_FLAGS)" --entry "TRACE=$(TRACE)" --entry "PERF_AOT_INSTRUCTIONS=$(PERF_AOT_INSTRUCTIONS)" $(RECOMP_LLVM_ENTRY_ARG) --file "$(CPU_STATE_ABI_HEADER)" --stamp "$@" --stale-glob ".recomp-profile-*" --invalidate-glob "$(BUILD_DIR)/$(GAME_NAME)_recomp*.o" --invalidate-glob "$(BUILD_DIR)/nk_lib_*.o"

# Compile the chunked generated C code.
#
# A unit holding one generated body over codegen's BODY_FALLBACK_BYTES compiles at codegen's
# fallback level (#1730): <game>_recomp_opt.mk sets RECOMP_UNIT_OPT_<object stem>, which comes
# after RECOMP_FLAGS (the last -O wins), so the level is in the command and its cache key.
-include $(BUILD_DIR)/$(GAME_NAME)_recomp_opt.mk
RECOMP_UNIT_OPT = $(RECOMP_UNIT_OPT_$(basename $(notdir $@)))

$(BUILD_DIR)/$(GAME_NAME)_recomp_%.o: $(BUILD_DIR)/$(GAME_NAME)_recomp_%.c src/rt/recomp.h $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	$(CC) $(RECOMP_FLAGS) $(RECOMP_UNIT_OPT) -I$(BUILD_DIR) -Isrc/rt -DSR_SDL3VK $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/$(GAME_NAME)_recomp.o: $(BUILD_DIR)/$(GAME_NAME)_recomp.c src/rt/recomp.h $(BUILD_DIR)/$(GAME_NAME)_recomp_funcs.h $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	$(CC) $(RECOMP_FLAGS) -I$(BUILD_DIR) -Isrc/rt -DSR_SDL3VK $(DEPFLAGS) -c $< -o $@

# --backend=llvm (#330). The runtime ABI shim is compiled once to bitcode with the
# generated-code flags (TRACE, PERF_AOT_INSTRUCTIONS and the stack census change what
# its macros expand to). Each chunk's IR is linked with only the shim functions it
# uses, internalized as the C chunks' static inline helpers are; llvm-link verifies the
# input and the linked module, and Clang compiles the result at RECOMP_OPT.
LLVM_RECOMP_FLAGS = $(filter-out -ftrack-macro-expansion=0,$(RECOMP_FLAGS))
LLVM_ABI_BC := $(BUILD_DIR)/llvm_abi.bc
$(LLVM_ABI_BC): src/rt/llvm_abi.c src/rt/llvm_abi.h src/rt/recomp.h $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	"$(LLVM_CLANG)" $(LLVM_RECOMP_FLAGS) -Isrc/rt -DSR_SDL3VK -emit-llvm -c $< -o $@

$(BUILD_DIR)/$(GAME_NAME)_recomp_%_ll.o: $(BUILD_DIR)/$(GAME_NAME)_recomp_%.ll $(LLVM_ABI_BC) $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	"$(LLVM_LINK)" $< --only-needed --internalize $(LLVM_ABI_BC) -o $(@:.o=.bc)
	"$(LLVM_CLANG)" $(RECOMP_OPT) -Wno-override-module -c $(@:.o=.bc) -o $@

# A system library carried once is its own translation unit (tools/codegen.py
# shared_library_source_text), named by the module's bytes and including only the
# runtime header, so every game's build compiles the identical file and a package
# build reuses its object (#1306).
$(BUILD_DIR)/nk_lib_%.o: $(BUILD_DIR)/nk_lib_%.c src/rt/recomp.h $(TRACE_STAMP) $(RECOMP_PROFILE_STAMP)
	$(CC) $(RECOMP_FLAGS) $(RECOMP_UNIT_OPT) -Isrc/rt -DSR_SDL3VK $(DEPFLAGS) -c $< -o $@

# Compile runtime sources.
$(BUILD_DIR)/%.o: src/rt/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/rt/gpu_sdl3vk/%.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/core/%.c $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# The pause overlay draws its pages with nk_ui (src/ui, src/rt/overlay); only the window
# presenter and those sources see the UI include path. SDL3_TTF_AVAILABLE is set further down;
# these flags are read when a recipe runs.
RT_NK_UI_FLAGS = -Isrc/ui -Isrc/rt/overlay -Ithird_party/clay -DNK_UI_WITH_SDL -DNK_UI_WITH_SDL3 \
                 $(if $(filter yes,$(SDL3_TTF_AVAILABLE)),$(SDL3_TTF_CFLAGS) -DNK_UI_WITH_SDL_TTF,)

$(BUILD_DIR)/%.o: src/ui/%.c $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(RT_NK_UI_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/ui/backend/%.c $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(RT_NK_UI_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/rt/overlay/%.c $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(RT_NK_UI_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/gui.o: src/rt/gui.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(RT_NK_UI_FLAGS) $(DEPFLAGS) -c $< -o $@

# Ace Mode's in-game UI (src/rt/gpu_sdl3vk/ace_ui.c) and the nk_ui Vulkan backend it draws
# through, which is built in only here (NK_UI_WITH_VULKAN; it loads Vulkan at run time).
$(BUILD_DIR)/ace_ui.o: src/rt/gpu_sdl3vk/ace_ui.c src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(RT_NK_UI_FLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/nk_ui_vk.o: src/ui/backend/nk_ui_vk.c $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(RT_NK_UI_FLAGS) -DNK_UI_WITH_VULKAN $(DEPFLAGS) -c $< -o $@

# The one translation unit that reads the build-local generated configuration. Every
# other runtime source consumes the generic accessors in src/rt/title_config.h, so the
# generated include path stops here rather than leaking into CFLAGS.
$(BUILD_DIR)/title_config.o: src/rt/title_config.c src/rt/title_config.h $(TITLE_CONFIG_HEADER) $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) -I$(TITLE_CONFIG_DIR) $(DEPFLAGS) -c $< -o $@

$(PORTABLE_CORE_DIR)/title_config.o: src/rt/title_config.c src/rt/title_config.h $(TITLE_CONFIG_HEADER)
	$(CC) $(PORTABLE_CORE_CFLAGS) -I$(TITLE_CONFIG_DIR) $(DEPFLAGS) -c $< -o $@

$(HLE_OBJS): $(BUILD_DIR)/%.o: src/rt/%.c $(HLE_HDRS) src/rt/audio_resample.h src/rt/osk_overlay.h src/rt/osk_text_entry.h src/rt/flash0_font.h src/rt/asset_index.h src/rt/archive_vfs.h src/rt/pgf_api.h src/rt/atrac3p_bridge.h src/rt/mp3_bridge.h src/rt/gpu_sdl3vk/ge_gpu.h src/rt/scripted_input.h src/rt/recomp.h $(RUNTIME_PROFILE_STAMP)
	$(CC) $(CFLAGS) $(HLE_INCLUDES) $(DEPFLAGS) -c $< -o $@
$(BUILD_DIR)/pgf_public.o: src/rt/pgf_public.c src/rt/pgf_api.h src/rt/recomp.h src/rt/ge_shared.h

runtime-objects: shader-verify $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o

$(PORTABLE_CORE_DIR)/%.o: src/rt/%.c src/rt/recomp.h
	$(CC) $(PORTABLE_CORE_CFLAGS) $(DEPFLAGS) -c $< -o $@

portable-core-objects: $(PORTABLE_CORE_OBJS)

# Fixed-width values printed with plain %u/%x/%d compile silently where uint32_t is
# unsigned int and fail on 32-bit newlib targets; the checker reproduces that with a
# shim and a 32-bit long (-m32 on LP64 hosts, see tools/format_portability_check.py).
format-portability-check:
	"$(PYTHON)" tools/format_portability_check.py

# Only the flight recorder embeds the reproducible build identity. A stamp named
# after the identity makes flight_recorder.o rebuild when the commit or
# SOURCE_DATE_EPOCH changes, without touching the other runtime objects.
FLIGHT_IDENTITY_STAMP := $(BUILD_DIR)/.flight-identity-$(or $(SR_SOURCE_COMMIT),none)-$(or $(SR_SOURCE_DATE_EPOCH),none)
$(FLIGHT_IDENTITY_STAMP):
	@"$(PYTHON)" -c "from pathlib import Path; p = Path(r'$@'); p.parent.mkdir(parents=True, exist_ok=True); [s.unlink() for s in p.parent.glob('.flight-identity-*') if s != p]; p.touch()"

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
	@"$(PYTHON)" -c "import sys; sys.exit(sys.argv[1] or None)" "$(SDL3_ERROR)"

# libchdr (third_party/libchdr, BSD-3-Clause; see its PROVENANCE.md) reads CHD disc images
# for the import step (src/core/nk_disc_chd.c). It is built once into its own archive with
# upstream's default configuration: the bundled miniz, single-file zstd decompressor and LZMA
# SDK decoder, no assembly, and upstream code compiled without the project's warning flags.
# Only the player, the player UI test binary and the disc-expansion selftest link it; the
# runtime never does.
LIBCHDR_DIR := third_party/libchdr
LIBCHDR_SRCS := $(addprefix $(LIBCHDR_DIR)/src/,libchdr_bitstream.c libchdr_cdrom.c libchdr_chd.c \
	libchdr_codec_avhuff.c libchdr_codec_cdfl.c libchdr_codec_cdlz.c libchdr_codec_cdzl.c \
	libchdr_codec_cdzs.c libchdr_codec_flac.c libchdr_codec_huff.c libchdr_codec_lzma.c \
	libchdr_codec_zlib.c libchdr_codec_zstd.c libchdr_flac.c libchdr_huffman.c) \
	$(LIBCHDR_DIR)/deps/lzma-26.02/src/LzmaDec.c $(LIBCHDR_DIR)/deps/miniz-3.1.2/miniz.c \
	$(LIBCHDR_DIR)/deps/zstd-1.5.7/zstddeclib.c
LIBCHDR_OBJ_DIR := $(BUILD_ROOT)/libchdr
LIBCHDR_OBJS := $(addprefix $(LIBCHDR_OBJ_DIR)/,$(notdir $(LIBCHDR_SRCS:.c=.o)))
LIBCHDR_LIB := $(LIBCHDR_OBJ_DIR)/libnkchdr.a
LIBCHDR_INC := -I$(LIBCHDR_DIR)/include
LIBCHDR_CFLAGS := -std=c99 -O2 $(LIBCHDR_INC) -I$(LIBCHDR_DIR)/deps/lzma-26.02/include \
	-I$(LIBCHDR_DIR)/deps/miniz-3.1.2 -I$(LIBCHDR_DIR)/deps/zstd-1.5.7 \
	-DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_STDIO -DMINIZ_NO_TIME
# The CHD backend over libchdr, compiled into the programs that read CHD.
PLAYER_CHD_SRCS := src/core/nk_disc_chd.c

$(LIBCHDR_OBJ_DIR):
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(LIBCHDR_OBJ_DIR)').mkdir(parents=True, exist_ok=True)"

# One object per source, from each folder libchdr keeps its sources in.
$(LIBCHDR_OBJ_DIR)/%.o: $(LIBCHDR_DIR)/src/%.c | $(LIBCHDR_OBJ_DIR)
	$(CC) $(LIBCHDR_CFLAGS) -c $< -o $@
$(LIBCHDR_OBJ_DIR)/%.o: $(LIBCHDR_DIR)/deps/lzma-26.02/src/%.c | $(LIBCHDR_OBJ_DIR)
	$(CC) $(LIBCHDR_CFLAGS) -c $< -o $@
$(LIBCHDR_OBJ_DIR)/%.o: $(LIBCHDR_DIR)/deps/miniz-3.1.2/%.c | $(LIBCHDR_OBJ_DIR)
	$(CC) $(LIBCHDR_CFLAGS) -c $< -o $@
$(LIBCHDR_OBJ_DIR)/%.o: $(LIBCHDR_DIR)/deps/zstd-1.5.7/%.c | $(LIBCHDR_OBJ_DIR)
	$(CC) $(LIBCHDR_CFLAGS) -c $< -o $@

$(LIBCHDR_LIB): $(LIBCHDR_OBJS)
	$(AR) rcs $@ $(LIBCHDR_OBJS)

PLAYER_EXE ?= $(BUILD_ROOT)/nakagawa_player$(EXE_EXT)
PLAYER_UI_TEST_EXE ?= $(BUILD_ROOT)/nakagawa_player_ui_test$(EXE_EXT)
PLAYER_CORE_SOURCES := src/core/nk_font.c src/rt/pgf_public.c src/core/nk_pgf_host.c src/core/nk_iso.c src/core/nk_library.c src/core/nk_launch.c src/core/nk_title_manifest.c src/core/nk_xb.c src/core/nk_input_profile.c src/core/nk_json.c src/core/nk_psp_crypto.c src/core/nk_psp_keystore.c src/core/nk_psp_builtin_keys.c src/core/nk_psp_aes.c src/core/nk_psp_sha1.c src/core/nk_psp_ec.c src/core/nk_psp_kirk.c src/core/nk_psp_prx.c src/core/nk_psp_kle.c src/core/nk_psp_inflate.c src/core/nk_psp_container.c src/core/nk_psp_amctrl.c src/core/nk_psp_lzrc.c src/core/nk_psp_npumdimg.c src/core/nk_disc_expand.c src/core/nk_disc_verify.c src/core/generated/nk_title_catalog.c
PLAYER_CORE_SRCS := $(PLAYER_CORE_SOURCES) $(PLAYER_PLATFORM_SRC)
PLAYER_NK_UI_SRCS := $(NK_UI_CORE_SRCS) $(NK_UI_EDGE_SRCS) src/ui/backend/nk_ui_sdlr.c $(NK_UI_PLAYER_C5_SRCS) $(NK_UI_PLAYER_HOST_SRCS)
# The player's About page shows "v<version> (MM-DD-YYYY)" (the version itself is
# NK_PLAYER_VERSION in src/player/ui/player_ui_nk.h). The date is
# SOURCE_DATE_EPOCH when the build sets it, else the source commit's time, so
# two builds of one commit show the same date; a tree without git shows
# "dev build". Only the player's compile sees these defines.
NK_PLAYER_BUILD_EPOCH := $(or $(SR_SOURCE_DATE_EPOCH),$(strip $(shell "$(PYTHON)" -c "import shutil, subprocess, sys; exe = shutil.which(sys.argv[1]); out = subprocess.run([exe, 'log', '-1', '--format=%ct'], capture_output=True, text=True).stdout.strip() if exe else ''; print(out if out.isdigit() else '')" "$(GIT)")))
NK_PLAYER_BUILD_COMMIT := $(shell "$(PYTHON)" -c "import sys; print(sys.argv[1][:7])" "$(SR_SOURCE_COMMIT)")
NK_PLAYER_IDENTITY_DEFS := $(if $(NK_PLAYER_BUILD_EPOCH),-DNK_PLAYER_BUILD_EPOCH=$(NK_PLAYER_BUILD_EPOCH)LL,) $(if $(strip $(NK_PLAYER_BUILD_COMMIT)),-DNK_PLAYER_BUILD_COMMIT=\"$(strip $(NK_PLAYER_BUILD_COMMIT))\",)
PLAYER_NK_UI_DEFS = -DNK_UI_WITH_SDL -DNK_UI_WITH_SDL3 $(if $(filter yes,$(SDL3_TTF_AVAILABLE)),-DNK_UI_WITH_SDL_TTF,) $(NK_PLAYER_IDENTITY_DEFS)
# Check for updates verifies a minisign signature over each release's checksum file
# (src/player/update_check.h) with nk_minisign_verify (src/core/nk_sig.c) on the
# vendored Monocypher Ed25519 and BLAKE2b code.
UPDATE_SIG_SRCS := src/core/nk_sig.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
# The update-signing public key (docs/RELEASE_SIGNING.md): the minisign public key the
# owner commits as this file. When it is there, its base64 line is compiled into the
# player and the update check's test as NK_UPDATE_SIGNING_KEY; when it is not, the build
# carries no key and the player never downloads an update. A file that is there but is
# not a minisign public key (an optional "untrusted comment: " line, then one base64 line
# of "Ed", an 8-byte key ID and a 32-byte Ed25519 key) stops the build. The key comes
# only from this committed file, so two builds of one commit embed the same key.
NK_UPDATE_KEY_FILE := assets/update_signing_key.pub
NK_UPDATE_KEY_LINE := $(if $(wildcard $(NK_UPDATE_KEY_FILE)),$(strip $(shell "$(PYTHON)" -c "import base64, sys; lines = [l.strip() for l in open(sys.argv[1], encoding='ascii').read().splitlines() if l.strip()]; key = lines[-1] if 1 <= len(lines) <= 2 and (len(lines) == 1 or lines[0].startswith('untrusted comment: ')) else ''; raw = base64.b64decode(key, validate=True) if len(key) == 56 else b''; print(key if len(raw) == 42 and raw[:2] == b'Ed' else '')" "$(NK_UPDATE_KEY_FILE)")),)
ifneq ($(wildcard $(NK_UPDATE_KEY_FILE)),)
ifeq ($(NK_UPDATE_KEY_LINE),)
$(error $(NK_UPDATE_KEY_FILE) is not a minisign public key; see docs/RELEASE_SIGNING.md)
endif
endif
NK_UPDATE_KEY_DEFS := $(if $(NK_UPDATE_KEY_LINE),-DNK_UPDATE_SIGNING_KEY=\"$(NK_UPDATE_KEY_LINE)\",)
PLAYER_SRCS := src/player/main.c src/player/player_host.c src/player/player_state.c src/player/input_settings.c src/player/iso_reader.c src/player/setup_staging.c src/player/ui_renderer.c src/player/package_builder.c src/player/update_check.c $(UPDATE_SIG_SRCS) $(PLAYER_NK_UI_SRCS) $(PLAYER_CORE_SRCS)
# The UI regression tests' scripted-event harness; only $(PLAYER_UI_TEST_EXE) links it.
PLAYER_UI_TEST_SRCS := src/player/player_ui_test.c
PLAYER_INCLUDES := -Isrc/player -Isrc/player/ui -Isrc/ui -Ithird_party/clay -Isrc/core -Isrc/core/generated -Isrc/rt -Ithird_party/monocypher $(SDL3_INC_FLAGS) $(PLAYER_VULKAN_INC)

# Both icon objects come from this one rule; windres finds the icon beside the script.
ifeq ($(OS),Windows_NT)
$(sort $(PLAYER_ICON_OBJ) $(TITLE_ICON_OBJ)): %/nakagawa_icon.o: assets/brand/nakagawa.rc assets/brand/nakagawa.ico
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(@D)').mkdir(parents=True, exist_ok=True)"
	$(WINDRES) --input-format=rc --output-format=coff -i $< -o $@
endif

$(PLAYER_EXE): | player-vulkan-check sdl3-check

$(PLAYER_EXE): $(PLAYER_SRCS) $(PLAYER_CHD_SRCS) $(LIBCHDR_LIB) src/core/nk_disc_expand.h src/core/nk_disc_chd.h src/player/player_state.h src/player/player_host.h src/player/input_settings.h src/player/iso_reader.h src/player/setup_staging.h src/player/ui_renderer.h src/player/ui/player_ui_nk.h src/ui/nk_ui.h src/ui/backend/nk_ui_sdlr.h src/player/package_builder.h src/player/update_check.h src/core/nk_types.h src/core/nk_font.h src/core/nk_iso.h src/core/nk_library.h src/core/nk_launch.h src/core/nk_title_manifest.h src/core/nk_xb.h src/core/nk_input_profile.h src/core/nk_json.h src/core/generated/nk_title_catalog.h src/core/nk_user_data_schema.h src/core/nk_sig.h $(wildcard $(NK_UPDATE_KEY_FILE)) $(PLAYER_ICON_OBJ)
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) $(RUNTIME_OPT) -Wall -Wextra $(PLAYER_NK_UI_DEFS) $(NK_UPDATE_KEY_DEFS) $(PLAYER_INCLUDES) $(LDFLAGS) $(PLAYER_VULKAN_LIB) $(PLAYER_GUI_LDFLAGS) $(PLAYER_CHD_SRCS) $(LIBCHDR_INC) $(PLAYER_SRCS) $(PLAYER_ICON_OBJ) -lSDL3 $(LIBCHDR_LIB) $(PLAYER_EXTRA_LIBS) $(REPRODUCIBLE_LINK_FLAG) -o $@

# Stage the SDL3/SDL3_ttf runtime DLL closure and its licence notices beside
# the player (#421) so launches from Explorer or a plain cmd.exe (no MSYS2 on
# PATH) use the readable TTF font instead of the bitmap fallback.
.PHONY: player
player: $(PLAYER_EXE)
	"$(PYTHON)" tools/stage_runtime_dlls.py --target $(patsubst %/,%,$(dir $(PLAYER_EXE))) --include-ui-fonts --refresh

$(PLAYER_UI_TEST_EXE): | player-vulkan-check sdl3-check

$(PLAYER_UI_TEST_EXE): $(PLAYER_SRCS) $(PLAYER_UI_TEST_SRCS) $(PLAYER_CHD_SRCS) $(LIBCHDR_LIB) src/player/player_ui_test.h src/core/nk_disc_expand.h src/core/nk_disc_chd.h src/player/player_state.h src/player/player_host.h src/player/input_settings.h src/player/iso_reader.h src/player/setup_staging.h src/player/ui_renderer.h src/player/ui/player_ui_nk.h src/ui/nk_ui.h src/ui/backend/nk_ui_sdlr.h src/player/package_builder.h src/player/update_check.h src/core/nk_types.h src/core/nk_font.h src/core/nk_iso.h src/core/nk_library.h src/core/nk_launch.h src/core/nk_title_manifest.h src/core/nk_xb.h src/core/nk_input_profile.h src/core/nk_json.h src/core/generated/nk_title_catalog.h src/core/nk_user_data_schema.h src/core/nk_sig.h $(wildcard $(NK_UPDATE_KEY_FILE))
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) $(RUNTIME_OPT) -Wall -Wextra -DNK_PLAYER_UI_REGRESSION_TEST $(PLAYER_NK_UI_DEFS) $(NK_UPDATE_KEY_DEFS) $(PLAYER_INCLUDES) $(LDFLAGS) $(PLAYER_VULKAN_LIB) $(PLAYER_SRCS) $(PLAYER_UI_TEST_SRCS) $(PLAYER_CHD_SRCS) $(LIBCHDR_INC) $(LIBCHDR_LIB) -lSDL3 $(PLAYER_EXTRA_LIBS) $(REPRODUCIBLE_LINK_FLAG) -o $@

# The regressions are independent player processes, so they run as parallel shards
# (tests/native/test_player_ui.py, NAKAGAWA_PLAYER_UI_SHARD): every shard runs side by
# side and together they run each test exactly once. --output-sync keeps each shard's
# report in one piece. The shard rules name their targets literally, so an edit to the
# recipe stays recipe-only for tools/ci_paths.py.
PLAYER_UI_SHARDS := 0 1 2 3
.PHONY: player-ui-regressions player-ui-regressions-shard-0 player-ui-regressions-shard-1 player-ui-regressions-shard-2 player-ui-regressions-shard-3
player-ui-regressions: $(PLAYER_UI_TEST_EXE)
	$(MAKE) --no-print-directory -j$(words $(PLAYER_UI_SHARDS)) --output-sync=target $(addprefix player-ui-regressions-shard-,$(PLAYER_UI_SHARDS))

# Target-specific export puts the variables into the recipe's child process
# environment across platforms, avoiding POSIX shell syntax (e.g. VAR=val cmd)
# that fails under Windows cmd.exe.
player-ui-regressions-shard-0 player-ui-regressions-shard-1 player-ui-regressions-shard-2 player-ui-regressions-shard-3: export NAKAGAWA_PLAYER_UI_TEST_EXE := $(PLAYER_UI_TEST_EXE)
player-ui-regressions-shard-0 player-ui-regressions-shard-1 player-ui-regressions-shard-2 player-ui-regressions-shard-3: export NAKAGAWA_PLAYER_UI_SHARD = $(patsubst player-ui-regressions-shard-%,%,$@)/$(words $(PLAYER_UI_SHARDS))
player-ui-regressions-shard-0 player-ui-regressions-shard-1 player-ui-regressions-shard-2 player-ui-regressions-shard-3:
	"$(PYTHON)" -m unittest discover -s tests/native -p "test_player_ui.py" -v

CHUNK_OBJS = $(patsubst %.c,%.o,$(wildcard $(BUILD_DIR)/$(GAME_NAME)_recomp_*.c))
# --backend=llvm: the LLVM IR half of a chunk is its own object (#330).
CHUNK_OBJS += $(patsubst %.ll,%_ll.o,$(wildcard $(BUILD_DIR)/$(GAME_NAME)_recomp_*.ll))
LIBRARY_OBJS = $(patsubst %.c,%.o,$(wildcard $(BUILD_DIR)/nk_lib_*.c))
# The title link passes its objects through a GCC response file. A package build
# under the player's user-data folder has long object paths, and the expanded list
# overran the Windows command-line limit: the command was cut mid-path and ld
# reported a missing ".../atrac3p" input. GCC reads backslashes in a response file
# as escapes, so the paths are written with forward slashes; BUILD_DIR is already
# refused when it contains whitespace, so each object stays one argument.
LINK_OBJECTS = $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(CHUNK_OBJS) $(LIBRARY_OBJS) $(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(TITLE_ICON_OBJ)
LINK_OBJECTS_RSP = $(BUILD_DIR)/link-objects.rsp
DEP_FILES = $(patsubst %.o,%.d,$(RT_GE_O) $(RT_OBJS) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o $(PORTABLE_CORE_OBJS) $(CHUNK_OBJS) $(LIBRARY_OBJS) $(BUILD_DIR)/$(GAME_NAME)_recomp.o $(BUILD_DIR)/vfpu_fuzz.o)
