# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: the nk_ui and player UI test binaries and the parser fuzz target.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# $(1) object directory, $(2) compile flags. Objects compile in source order and the first
# failure stops the line, as it stopped each per-binary compile before.
define NC_COMPILE_OBJECTS
@"$(PYTHON)" -c "import os, sys; os.makedirs(sys.argv[1], exist_ok=True)" $(1)
$(foreach s,$(NC_SOURCES),$(CC) $(2) -c $(s) -o $(1)/$(notdir $(s:.c=.o)) &&) true
endef

native-core-objects-core:
	$(call NC_COMPILE_OBJECTS,$(NC_OBJ_DIR)/core,$(NC_CFLAGS_CORE))

native-core-objects-player:
	$(call NC_COMPILE_OBJECTS,$(NC_OBJ_DIR)/player,$(NC_CFLAGS_PLAYER))

# Test-binary recipes, shared by the standalone *-test-bin targets and native-core-tests.
define PLAYER_STATE_TEST_BIN_CMD
$(CC) -std=c99 -Wall -Wextra -DNK_TITLE_MANIFEST_TEST_SEAMS -DNK_SAVES_TEST_SEAMS -Isrc/core -Isrc/core/generated -Isrc/rt -Isrc/player -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
	$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/player/input_settings.c src/player/player_state.c src/player/iso_reader.c src/player/package_builder.c src/player/setup_staging.c \
	tests/native/native_test_isolation.c tests/native/test_player_state.c $(PLAYER_EXTRA_LIBS) -o $(BUILD_ROOT)/test_player_state$(EXE_EXT)
endef

define INPUT_SETTINGS_TEST_BIN_CMD
$(CC) $(NC_CFLAGS_PLAYER) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_PLAYER) src/player/input_settings.c \
	tests/native/native_test_isolation.c tests/native/test_input_settings.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_input_settings$(EXE_EXT)
endef

define PACKAGE_BUILDER_TEST_BIN_CMD
$(CC) $(NC_CFLAGS_PLAYER) -DNK_PLAYER_UI_REGRESSION_TEST -DPYTHON_VERSION_PROBE_TIMEOUT_MS=3000 -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_PLAYER) src/player/package_builder.c \
	tests/native/native_test_isolation.c tests/native/test_package_builder.c $(PLAYER_EXTRA_LIBS) -o $(BUILD_ROOT)/test_package_builder$(EXE_EXT)
endef

# Check for updates: answers from a fake transport and the recorded Releases API fixture,
# never the internet (AGENTS.md section 5). Runs from the repository root for the fixture.
# It signs with a TEST-ONLY key of its own and, when the build embeds the update-signing
# key, checks that the embedded key reads back.
define UPDATE_CHECK_TEST_BIN_CMD
$(CC) $(NC_CFLAGS_PLAYER) -Ithird_party/monocypher $(NK_UPDATE_KEY_DEFS) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_PLAYER) src/player/package_builder.c src/player/update_check.c $(UPDATE_SIG_SRCS) \
	tests/native/native_test_isolation.c tests/native/test_update_check.c $(PLAYER_EXTRA_LIBS) -o $(BUILD_ROOT)/test_update_check$(EXE_EXT)
endef

player-state-test-bin:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(call PLAYER_STATE_TEST_BIN_CMD)

# osk-overlay-paint-selftest - the in-window on-screen keyboard drawn over a synthetic frame
# (src/rt/osk_overlay_paint.c): the highlight, a dimmed key, the text field, and an unchanged
# frame while closed. SDL3 software renderer, no window. Writes the frame as a BMP into build/
# for the visual check. Needs SDL3, so it runs with the player UI tests.
osk-overlay-paint-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Isrc/rt $(SDL3_INC_FLAGS) \
		-o $(BUILD_DIR)/osk_overlay_paint_selftest$(EXE_EXT) \
		tests/native/osk_overlay_paint_selftest.c src/rt/osk_overlay.c src/rt/osk_overlay_paint.c $(LDFLAGS) $(LIBS)
	$(BUILD_DIR)/osk_overlay_paint_selftest$(EXE_EXT) $(BUILD_DIR)/osk_overlay_paint_selftest.bmp

# Resolve the already-supported SDL3_ttf dependency for the font selftest. Linux
# CI installs sdl3-ttf in the SDL3_ttf job; Windows uses the discovered UCRT64 SDK.
ifeq ($(OS),Windows_NT)
SDL3_TTF_AVAILABLE := $(shell "$(PYTHON)" -c "from pathlib import Path; import sys; root=Path(r'$(SDL3_DIR)'); sys.exit(0 if (root/'include/SDL3_ttf/SDL_ttf.h').is_file() and (root/'lib/libSDL3_ttf.dll.a').is_file() and (root/'bin/SDL3_ttf.dll').is_file() else 1)" && echo yes)
SDL3_TTF_CFLAGS := $(SDL3_INC_FLAGS)
SDL3_TTF_LIBS := $(SDL3_LDFLAGS) -lSDL3_ttf -lSDL3
else
SDL3_TTF_AVAILABLE := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl3-ttf && echo yes)
SDL3_TTF_CFLAGS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --cflags sdl3-ttf)
SDL3_TTF_LIBS := $(shell command -v pkg-config >/dev/null 2>&1 && pkg-config --libs sdl3-ttf)
endif

.PHONY: ui-font-selftest
ifeq ($(SDL3_TTF_AVAILABLE),yes)
ui-font-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c11 -Wall -Wextra -Werror -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(SDL3_TTF_CFLAGS) tests/native/test_ui_font_assets.c \
		-o $(BUILD_ROOT)/test_ui_font_assets$(EXE_EXT) $(SDL3_TTF_LIBS)
	$(BUILD_ROOT_RUN)/test_ui_font_assets$(EXE_EXT) \
		assets/fonts/ui/noto-sans-jp/NotoSansJP-Regular.ttf \
		assets/fonts/ui/chakra-petch/ChakraPetch-SemiBold.ttf
else
ui-font-selftest:
	@echo "SKIP: UI font native selftest requires the SDL3_ttf development package (sdl3-ttf on Linux or the MSYS2 SDL3_ttf import library on Windows)."
endif

# Player UI tests link SDL3 (software renderer, no window), so they run where the
# player itself builds rather than in the SDL-free native-core-tests set. The
# optional UI font selftest reports a named skip when SDL3_ttf is unavailable.
player-ui-tests: osk-overlay-paint-selftest ui-font-selftest nk-ui-player-screens-selftest nk-ui-overlay-screens-selftest nk-ui-overlay-host-selftest
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra $(PLAYER_INCLUDES) $(LDFLAGS) tests/native/test_ui_clip.c -lSDL3 \
		-o $(BUILD_ROOT)/test_ui_clip$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_ui_clip$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Werror -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(PLAYER_INCLUDES) $(LDFLAGS) tests/native/test_player_ui_art.c \
		$(NK_UI_PLAYER_HOST_SRCS) src/ui/nk_ui_image.c src/ui/nk_ui_assets.c -lSDL3 \
		-o $(BUILD_ROOT)/test_player_ui_art$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_player_ui_art$(EXE_EXT)

.PHONY: nk-ui-player-screens-capture
ifeq ($(SDL3_TTF_AVAILABLE),yes)
ifeq ($(strip $(NK_UI_CAPTURE_DIR)),)
nk-ui-player-screens-capture:
	@echo "ERROR: set NK_UI_CAPTURE_DIR to a directory for synthetic screen captures"; exit 2
else
nk-ui-player-screens-capture: export SDL_VIDEODRIVER := dummy
nk-ui-player-screens-capture: export SDL_RENDER_DRIVER := software
nk-ui-player-screens-capture:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -DNK_UI_WITH_SDL -DNK_UI_WITH_SDL_TTF -DNK_UI_SCREEN_CAPTURE \
		-Isrc/ui -Isrc/player/ui -Ithird_party/clay $(SDL3_TTF_CFLAGS) \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) src/ui/nk_ui_ttf.c src/ui/backend/nk_ui_sdlr.c \
		src/player/ui/ui_router.c src/player/ui/scr_library.c $(NK_UI_PLAYER_TITLE_SRCS) \
		tests/native/test_player_screens.c $(SDL3_TTF_LIBS) -lm \
		-o $(BUILD_ROOT)/test_player_screens_capture$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_player_screens_capture$(EXE_EXT) --capture-dir="$(NK_UI_CAPTURE_DIR)"
endif
else
nk-ui-player-screens-capture:
	@echo "SKIP: offscreen screen captures require the SDL3_ttf development package"
endif

input-settings-test-bin: native-core-objects-player
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(call INPUT_SETTINGS_TEST_BIN_CMD)

package-builder-test-bin: native-core-objects-player
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(call PACKAGE_BUILDER_TEST_BIN_CMD)

.PHONY: clay-layout-selftest
clay-layout-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_DIR)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Ithird_party/clay -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		-o $(BUILD_DIR)/clay_layout_selftest$(EXE_EXT) tests/native/test_clay_layout.c
	$(BUILD_DIR)/clay_layout_selftest$(EXE_EXT)

# nk_ed25519_verify (src/core/nk_sig.c) on the vendored Monocypher Ed25519 check,
# against the RFC 8032 section 7.1 vectors, and nk_minisign_verify against real
# minisign output made with a TEST-ONLY key. The player's update check links both.
ED25519_VERIFY_SELFTEST_SRCS := src/core/nk_sig.c third_party/monocypher/monocypher.c \
	third_party/monocypher/monocypher-ed25519.c tests/native/test_nk_sig.c
define ED25519_VERIFY_SELFTEST_CMD
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Ithird_party/monocypher $(ED25519_VERIFY_SELFTEST_SRCS) \
		-o $(BUILD_ROOT)/test_nk_sig$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_nk_sig$(EXE_EXT)
endef

.PHONY: ed25519-verify-selftest
ed25519-verify-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(call ED25519_VERIFY_SELFTEST_CMD)

.PHONY: nk-ui-core-selftest
nk-ui-core-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Ithird_party/clay \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_NATIVE_CORE_SRCS) tests/native/test_nk_ui_core.c -lm \
		-o $(BUILD_ROOT)/test_nk_ui_core$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_nk_ui_core$(EXE_EXT)

.PHONY: nk-ui-player-screens-selftest
nk-ui-player-screens-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Isrc/player/ui -Ithird_party/clay \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) $(filter-out src/player/ui/player_ui_nk.c,$(NK_UI_PLAYER_C5_SRCS)) \
		tests/native/test_player_screens.c -lm -o $(BUILD_ROOT)/test_player_screens$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_player_screens$(EXE_EXT)

# nk-ui-overlay-screens-selftest - the in-game overlay's pages (#1159, C9c) on the SDL-free core:
# page flow, focus, actions, fit at every capture size, and fixtures/nk_ui/overlay.golden.
.PHONY: nk-ui-overlay-screens-selftest
nk-ui-overlay-screens-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Isrc/rt/overlay -Isrc/core -Ithird_party/clay \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) $(NK_UI_OVERLAY_C9C_SRCS) $(NK_UI_OVERLAY_CORE_SRCS) \
		tests/native/test_overlay_screens.c $(PLAYER_PLATFORM_LIBS) -lm \
		-o $(BUILD_ROOT)/test_overlay_screens$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_overlay_screens$(EXE_EXT)

# nk-ui-overlay-screens-capture - every overlay page, as the launcher's captures do for its
# screens: drawn by the SDL_Renderer backend with the bundled fonts and saved as BMPs for review.
.PHONY: nk-ui-overlay-screens-capture
ifeq ($(SDL3_TTF_AVAILABLE),yes)
ifeq ($(strip $(NK_UI_CAPTURE_DIR)),)
nk-ui-overlay-screens-capture:
	@echo "ERROR: set NK_UI_CAPTURE_DIR to a directory for overlay captures"; exit 2
else
nk-ui-overlay-screens-capture: export SDL_VIDEODRIVER := dummy
nk-ui-overlay-screens-capture: export SDL_RENDER_DRIVER := software
nk-ui-overlay-screens-capture:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -DNK_UI_WITH_SDL -DNK_UI_WITH_SDL_TTF -DNK_UI_SCREEN_CAPTURE \
		-Isrc/ui -Isrc/rt/overlay -Isrc/core -Ithird_party/clay $(SDL3_TTF_CFLAGS) \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) src/ui/nk_ui_ttf.c src/ui/backend/nk_ui_sdlr.c \
		$(NK_UI_OVERLAY_C9C_SRCS) $(NK_UI_OVERLAY_CORE_SRCS) \
		tests/native/test_overlay_screens.c $(SDL3_TTF_LIBS) $(PLAYER_PLATFORM_LIBS) -lm \
		-o $(BUILD_ROOT)/test_overlay_screens_capture$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_overlay_screens_capture$(EXE_EXT) --capture-dir="$(NK_UI_CAPTURE_DIR)"
endif
else
nk-ui-overlay-screens-capture:
	@echo "SKIP: offscreen overlay captures require the SDL3_ttf development package"
endif

# nk-ui-overlay-host-selftest - the overlay's runtime host (#1159, C9c) on its production path:
# SDL3_ttf and the bundled UI fonts, real SDL key events, the pages drawn by the SDL_Renderer
# backend over a base surface, a resize onto a new renderer. Like nk-ui-sdlr-selftest it builds
# the SKIP variant without the SDL3 or SDL3_ttf headers and skips when the library cannot load.
# The host is runtime code and always needs SDL3, so the SKIP variant builds the test alone.
.PHONY: nk-ui-overlay-host-selftest
nk-ui-overlay-host-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	@sdl_defs=""; sdl_libs=""; host_srcs=""; \
	if printf '%s\n' '#include <SDL3/SDL.h>' '#include <SDL3_ttf/SDL_ttf.h>' '#include <SDL3_ttf/SDL_textengine.h>' | $(CC) -E -x c $(SDL3_INC_FLAGS) - >/dev/null 2>&1; then \
		sdl_defs="-DNK_UI_WITH_SDL -DNK_UI_WITH_SDL3 -DNK_UI_WITH_SDL_TTF"; sdl_libs="$(LDFLAGS) -lSDL3"; \
		host_srcs="$(NK_UI_CORE_SRCS) $(NK_UI_EDGE_SRCS) src/ui/backend/nk_ui_sdlr.c $(NK_UI_OVERLAY_C9C_SRCS) $(NK_UI_OVERLAY_HOST_SRCS) $(NK_UI_OVERLAY_CORE_SRCS)"; \
	else \
		echo "nk-ui-overlay-host-selftest: SDL3 or SDL3_ttf headers not found; building the SKIP variant"; \
	fi; \
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Isrc/rt/overlay -Isrc/core -Ithird_party/clay \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(SDL3_INC_FLAGS) $$sdl_defs $$host_srcs tests/native/test_overlay_host.c \
		$$sdl_libs $(PLAYER_PLATFORM_LIBS) -lm -o $(BUILD_ROOT)/test_overlay_host$(EXE_EXT) || exit 1; \
	rc=0; $(BUILD_ROOT_RUN)/test_overlay_host$(EXE_EXT) . || rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP nk-ui-overlay-host-selftest (exit 77; the reason is printed above)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL nk-ui-overlay-host-selftest (exit $$rc)"; exit $$rc; fi

# nk-ui-sdlr-selftest - the nk_ui SDL_Renderer backend and SDL3_ttf text engine (lane C3b) drawing a
# synthetic screen into a software-rendered surface at 1x, 1.25x and 2x, with Japanese shaping and
# kerning checks. SDL3_ttf is loaded at run time, so only its headers are needed to build. A host
# without the SDL3 or SDL3_ttf headers builds the SKIP variant, and a host that cannot load the
# library skips at run time. Both exit 77; this recipe then prints "SKIP nk-ui-sdlr-selftest" and
# succeeds, so a skip is visible in the log but does not fail native-core-tests.
.PHONY: nk-ui-sdlr-selftest
nk-ui-sdlr-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	@sdl_defs=""; sdl_libs=""; \
	if printf '%s\n' '#include <SDL3/SDL.h>' '#include <SDL3_ttf/SDL_ttf.h>' '#include <SDL3_ttf/SDL_textengine.h>' | $(CC) -E -x c $(SDL3_INC_FLAGS) - >/dev/null 2>&1; then \
		sdl_defs="-DNK_UI_WITH_SDL -DNK_UI_WITH_SDL_TTF"; sdl_libs="$(LDFLAGS) -lSDL3"; \
	else \
		echo "nk-ui-sdlr-selftest: SDL3 or SDL3_ttf headers not found; building the SKIP variant"; \
	fi; \
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Ithird_party/clay $(SDL3_INC_FLAGS) $$sdl_defs \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) src/ui/nk_ui_ttf.c src/ui/backend/nk_ui_sdlr.c tests/native/test_nk_ui_sdlr.c \
		$$sdl_libs -lm -o $(BUILD_ROOT)/test_nk_ui_sdlr$(EXE_EXT) || exit 1; \
	rc=0; $(BUILD_ROOT_RUN)/test_nk_ui_sdlr$(EXE_EXT) || rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP nk-ui-sdlr-selftest (exit 77; the reason is printed above)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL nk-ui-sdlr-selftest (exit $$rc)"; exit $$rc; fi

# nk-ui-vk-selftest - the nk_ui Vulkan backend (lane C3c): draw lists rendered on a headless Vulkan
# device into an offscreen image and read back, checking scissors, text, images, waves and
# premultiplied blending pixel by pixel, with the Khronos validation layer on when it is installed.
# The backend and the test open the Vulkan loader at run time, so only the headers are needed to
# build. A host without the Vulkan headers builds the SKIP variant, and a host without a loader or
# a Vulkan 1.1 device skips at run time. Both exit 77; this recipe then prints
# "SKIP nk-ui-vk-selftest" and succeeds, so a skip is visible in the log but does not fail
# native-core-tests. NK_UI_VK_REQUIRE=1 makes a skip fail, for a job that provides a device.
NK_UI_VK_REQUIRE ?=
NK_UI_VK_SELFTEST_LIBS := $(if $(filter Windows_NT,$(OS)),,-ldl)
.PHONY: nk-ui-vk-selftest
nk-ui-vk-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	@vk_defs=""; \
	if printf '%s\n' '#include <vulkan/vulkan.h>' | $(CC) -E -x c $(VULKAN_INC_FLAGS) - >/dev/null 2>&1; then \
		vk_defs="-DNK_UI_WITH_VULKAN"; \
	else \
		echo "nk-ui-vk-selftest: Vulkan headers not found; building the SKIP variant"; \
	fi; \
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Ithird_party/clay $(VULKAN_INC_FLAGS) $$vk_defs \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) src/ui/backend/nk_ui_vk.c tests/native/test_nk_ui_vk.c \
		-lm $(NK_UI_VK_SELFTEST_LIBS) -o $(BUILD_ROOT)/test_nk_ui_vk$(EXE_EXT) || exit 1; \
	rc=0; $(BUILD_ROOT_RUN)/test_nk_ui_vk$(EXE_EXT) || rc=$$?; \
	if [ $$rc -eq 77 ] && [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then \
		echo "FAIL nk-ui-vk-selftest (exit 77, but NK_UI_VK_REQUIRE=1 requires a Vulkan device)"; exit 1; \
	elif [ $$rc -eq 77 ]; then echo "SKIP nk-ui-vk-selftest (exit 77; the reason is printed above)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL nk-ui-vk-selftest (exit $$rc)"; exit $$rc; fi

# ace-smooth-selftest - Ace Mode's in-between frames (src/rt/gpu_sdl3vk/ace_smooth.c) on a headless
# Vulkan device: a moving shape lands halfway, a HUD over motion keeps every pixel, a cut holds the
# old frame and the held frame is the new one, byte for byte. With the HUD mask and depth
# (ace_hud.c): the scene beside a HUD and at a camouflaged band's edge is right, and an empty
# mask changes no byte. Linked from the pass, ace_hud.c and the C
# library alone (invariant S1). No Vulkan headers, loader or device: SKIP (exit 77), visible in
# the log; NK_UI_VK_REQUIRE=1 makes a skip fail.
native-core-tests: ace-smooth-selftest
.PHONY: ace-smooth-selftest
ace-smooth-selftest: shader-verify
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	@if ! printf '%s\n' '#include <vulkan/vulkan.h>' | $(CC) -E -x c $(VULKAN_INC_FLAGS) - >/dev/null 2>&1; then \
		echo "SKIP ace-smooth-selftest (the Vulkan headers were not found)"; \
		if [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then exit 1; fi; exit 0; \
	fi; \
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt -Isrc/rt/gpu_sdl3vk $(VULKAN_INC_FLAGS) \
		src/rt/gpu_sdl3vk/ace_smooth.c src/rt/gpu_sdl3vk/ace_hud.c src/rt/gpu_sdl3vk/ace_smooth_selftest.c \
		$(NK_UI_VK_SELFTEST_LIBS) -lm -o $(BUILD_ROOT)/ace_smooth_selftest$(EXE_EXT) || exit 1; \
	rc=0; $(BUILD_ROOT_RUN)/ace_smooth_selftest$(EXE_EXT) $(BUILD_ROOT) || rc=$$?; \
	if [ $$rc -eq 77 ] && [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then \
		echo "FAIL ace-smooth-selftest (skipped, but NK_UI_VK_REQUIRE=1 requires a Vulkan device)"; exit 1; \
	elif [ $$rc -eq 77 ]; then echo "SKIP ace-smooth-selftest (exit 77)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL ace-smooth-selftest (exit $$rc)"; exit $$rc; fi

# fsr1-selftest - the presentation upscaler (src/rt/gpu_sdl3vk/fsr1_pass.c, AMD FSR 1 EASU + RCAS)
# on a headless Vulkan device: EASU alone at 2x keeps a vertical and a slanted step edge sharper
# than the presenter's bilinear blit by a measured margin; RCAS raises a soft edge's contrast over
# EASU alone by a measured factor, more at the sharpest setting, and overshoots the edge's range by
# 10 levels at most; a size past the device's image limit fails once and is not retried; a flat
# colour from a BGRA source stays exact at 480x272 to 1355x768; the source image is never written.
# --easu-spv and --rcas-spv load a mutant shader in place of the built-in one. Linked from the
# pass, the C library and libm alone. No Vulkan headers, loader or device: SKIP (exit 77), visible
# in the log; NK_UI_VK_REQUIRE=1 makes a skip fail.
native-core-tests: fsr1-selftest
.PHONY: fsr1-selftest
fsr1-selftest: shader-verify
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	@if ! printf '%s\n' '#include <vulkan/vulkan.h>' | $(CC) -E -x c $(VULKAN_INC_FLAGS) - >/dev/null 2>&1; then \
		echo "SKIP fsr1-selftest (the Vulkan headers were not found)"; \
		if [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then exit 1; fi; exit 0; \
	fi; \
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt -Isrc/rt/gpu_sdl3vk $(VULKAN_INC_FLAGS) \
		src/rt/gpu_sdl3vk/fsr1_pass.c src/rt/gpu_sdl3vk/fsr1_pass_selftest.c \
		$(NK_UI_VK_SELFTEST_LIBS) -lm -o $(BUILD_ROOT)/fsr1_selftest$(EXE_EXT) || exit 1; \
	rc=0; $(BUILD_ROOT_RUN)/fsr1_selftest$(EXE_EXT) || rc=$$?; \
	if [ $$rc -eq 77 ] && [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then \
		echo "FAIL fsr1-selftest (skipped, but NK_UI_VK_REQUIRE=1 requires a Vulkan device)"; exit 1; \
	elif [ $$rc -eq 77 ]; then echo "SKIP fsr1-selftest (exit 77)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL fsr1-selftest (exit $$rc)"; exit $$rc; fi

# ace-ui-selftest - Ace Mode's in-game UI (src/rt/gpu_sdl3vk/ace_ui.c): the readout, the launch
# notice and the crisp keyboard laid out at the overlay's sizes and two ultrawide windows (inside
# the 16 px safe area, apart from each other), then drawn on a headless Vulkan device over a
# cleared image, which keeps every pixel outside them; the image is written for review. Linked
# from the UI pass, the keyboard model and nk_ui alone (invariant S1). With SDL3_ttf the picture
# uses the launcher's fonts. No Vulkan headers, loader or device: SKIP (exit 77), visible in
# the log; NK_UI_VK_REQUIRE=1 makes a skip fail.
native-core-tests: ace-ui-selftest
.PHONY: ace-ui-selftest
ace-ui-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	@if ! printf '%s\n' '#include <vulkan/vulkan.h>' | $(CC) -E -x c $(VULKAN_INC_FLAGS) - >/dev/null 2>&1; then \
		echo "SKIP ace-ui-selftest (the Vulkan headers were not found)"; \
		if [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then exit 1; fi; exit 0; \
	fi; \
	ttf_defs=""; ttf_srcs=""; ttf_libs=""; \
	if printf '%s\n' '#include <SDL3/SDL.h>' '#include <SDL3_ttf/SDL_ttf.h>' '#include <SDL3_ttf/SDL_textengine.h>' | $(CC) -E -x c $(SDL3_INC_FLAGS) - >/dev/null 2>&1; then \
		ttf_defs="-DNK_UI_WITH_SDL -DNK_UI_WITH_SDL_TTF"; ttf_srcs="src/ui/nk_ui_ttf.c"; ttf_libs="$(LDFLAGS) -lSDL3"; \
	fi; \
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Isrc/rt -Isrc/rt/gpu_sdl3vk -Ithird_party/clay \
		$(VULKAN_INC_FLAGS) $(SDL3_INC_FLAGS) -DNK_UI_WITH_VULKAN $$ttf_defs \
		$(NK_UI_CORE_SRCS) src/ui/backend/nk_ui_vk.c $$ttf_srcs src/rt/osk_overlay.c \
		src/rt/gpu_sdl3vk/ace_ui.c src/rt/gpu_sdl3vk/ace_ui_selftest.c \
		$$ttf_libs -lm $(NK_UI_VK_SELFTEST_LIBS) -o $(BUILD_ROOT)/ace_ui_selftest$(EXE_EXT) || exit 1; \
	rc=0; $(BUILD_ROOT_RUN)/ace_ui_selftest$(EXE_EXT) $(BUILD_ROOT) --fonts . || rc=$$?; \
	if [ $$rc -eq 77 ] && [ "$(NK_UI_VK_REQUIRE)" = 1 ]; then \
		echo "FAIL ace-ui-selftest (the GPU leg skipped, but NK_UI_VK_REQUIRE=1 requires a Vulkan device)"; exit 1; \
	elif [ $$rc -eq 77 ]; then echo "SKIP ace-ui-selftest GPU leg (exit 77; the layout cells passed)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL ace-ui-selftest (exit $$rc)"; exit $$rc; fi

.PHONY: nk-ui-focus-selftest
# The focus, input and glyph checks are SDL-free. The SDL_Event translation leg is built from
# the same sources: with SDL3 (as found by build discovery) it translates real SDL events;
# without, it compiles without SDL and exits 77, which this recipe reports as SKIP.
NK_UI_FOCUS_SDL_READY := $(and $(filter-out none,$(SDL3_PROVIDER)),$(if $(strip $(SDL3_ERROR)),,yes))
NK_UI_FOCUS_SDL_CFLAGS := $(if $(NK_UI_FOCUS_SDL_READY),-DNK_UI_WITH_SDL3 $(SDL3_INC_FLAGS),)
NK_UI_FOCUS_SDL_LIBS := $(if $(NK_UI_FOCUS_SDL_READY),$(SDL3_LDFLAGS) -lSDL3,)
nk-ui-focus-selftest:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Ithird_party/clay \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) src/ui/nk_ui_sdl_input.c tests/native/test_nk_ui_focus.c -lm \
		-o $(BUILD_ROOT)/test_nk_ui_focus$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_nk_ui_focus$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc/ui -Ithird_party/clay $(NK_UI_FOCUS_SDL_CFLAGS) \
		-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(NK_UI_CORE_SRCS) src/ui/nk_ui_sdl_input.c tests/native/test_nk_ui_focus.c -lm \
		$(NK_UI_FOCUS_SDL_LIBS) -o $(BUILD_ROOT)/test_nk_ui_focus_sdl$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_nk_ui_focus_sdl$(EXE_EXT) --sdl-input; rc=$$?; \
		if [ "$$rc" = 77 ]; then echo "SKIP nk-ui-focus-selftest SDL translation leg (exit 77): SDL3 not found by build discovery"; \
		elif [ "$$rc" != 0 ]; then exit "$$rc"; fi

native-core-tests: cpu-lle-selftest domain-mode-selftest native-core-objects-core native-core-objects-player $(LIBCHDR_LIB) clay-layout-selftest nk-ui-core-selftest nk-ui-player-screens-selftest nk-ui-overlay-screens-selftest nk-ui-overlay-host-selftest nk-ui-sdlr-selftest nk-ui-vk-selftest nk-ui-focus-selftest
	$(CC) -std=c99 -Wall -Wextra -Isrc/rt -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" src/rt/pgf_public.c \
		tests/native/test_pgf_public.c -o $(BUILD_ROOT)/test_pgf_public$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_pgf_public$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/native_test_isolation.c tests/native/test_core_catalog.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_core_catalog$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_core_catalog$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/native_test_isolation.c tests/native/test_parsers_hostile.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_parsers_hostile$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_parsers_hostile$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/test_manifest_parser.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_manifest_parser$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_manifest_parser$(EXE_EXT) --check
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/native_test_isolation.c tests/native/test_launch_resolution.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_launch_resolution$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_launch_resolution$(EXE_EXT)
	$(call PLAYER_STATE_TEST_BIN_CMD)
	$(BUILD_ROOT_RUN)/test_player_state$(EXE_EXT)
	$(call INPUT_SETTINGS_TEST_BIN_CMD)
	$(BUILD_ROOT_RUN)/test_input_settings$(EXE_EXT)
	$(call PACKAGE_BUILDER_TEST_BIN_CMD)
	$(BUILD_ROOT_RUN)/test_package_builder$(EXE_EXT)
	$(call UPDATE_CHECK_TEST_BIN_CMD)
	$(BUILD_ROOT_RUN)/test_update_check$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/rt -Isrc/player -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/player/setup_staging.c src/rt/archive_vfs.c \
		tests/native/native_test_isolation.c tests/native/test_xb_parser.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_xb_parser$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_xb_parser$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -Werror $(LIBCHDR_INC) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) $(PLAYER_CHD_SRCS) \
		tests/native/native_test_isolation.c tests/native/test_disc_expand.c $(LIBCHDR_LIB) $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_disc_expand$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_disc_expand$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -Werror -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) 		tests/native/native_test_isolation.c tests/native/test_disc_verify.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_disc_verify$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_disc_verify$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -Wpedantic -Werror -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/native_test_isolation.c tests/native/test_nk_zip.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_nk_zip$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_nk_zip$(EXE_EXT)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" tests/native/test_safe_math.c \
		-o $(BUILD_ROOT)/test_safe_math$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_safe_math$(EXE_EXT)
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Itests/native -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		tests/native/pcm_wav_dump.c tests/native/test_pcm_wav_dump.c -o $(BUILD_ROOT)/test_pcm_wav_dump$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_pcm_wav_dump$(EXE_EXT)
	$(CC) -std=c99 -Wall -Wextra -Isrc/core -Isrc/core/generated -Isrc/rt -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c src/rt/archive_vfs.c \
		tests/native/test_fuzz_parsers.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_fuzz_parsers$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_fuzz_parsers$(EXE_EXT) --iters 100
	$(call PSMF_PRODUCER_SELFTEST_CMD,100)
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/test_input_profile.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_input_profile$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_input_profile$(EXE_EXT)
	$(call ED25519_VERIFY_SELFTEST_CMD)
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/native_test_isolation.c tests/native/test_portable_mode.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_portable_mode$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_portable_mode$(EXE_EXT)
ifeq ($(OS),Windows_NT)
	$(CC) -std=c99 -Wall -Wextra tests/native/argv_echo_helper.c -lshell32 -o $(BUILD_ROOT)/argv_echo_helper$(EXE_EXT)
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/test_win32_process.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_win32_process$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_win32_process$(EXE_EXT)
else
	$(CC) $(NC_CFLAGS_CORE) -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(NC_OBJS_CORE) \
		tests/native/test_posix_process.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_posix_process$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_posix_process$(EXE_EXT)
endif

FUZZ_ITERS ?= 5000
ifeq ($(OS),Windows_NT)
FUZZ_SAN_FLAGS :=
else
FUZZ_SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all
endif

fuzz-parsers:
	@"$(PYTHON)" -c "from pathlib import Path; Path('$(BUILD_ROOT)').mkdir(parents=True, exist_ok=True)"
	$(CC) -std=c99 -Wall -Wextra $(FUZZ_SAN_FLAGS) -Isrc/core -Isrc/core/generated -Isrc/rt -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(PLAYER_CORE_SOURCES) $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c src/rt/archive_vfs.c \
		tests/native/test_fuzz_parsers.c $(PLAYER_PLATFORM_LIBS) -o $(BUILD_ROOT)/test_fuzz_parsers$(EXE_EXT)
	$(BUILD_ROOT_RUN)/test_fuzz_parsers$(EXE_EXT) --iters $(FUZZ_ITERS)
	$(MAKE) --no-print-directory psmf-producer-selftest \
		PSMF_FUZZ_ITERS=$(FUZZ_ITERS) FUZZ_SAN_FLAGS="$(FUZZ_SAN_FLAGS)"
	$(MAKE) --no-print-directory psmf-media-selftest \
		MEDIA_FUZZ_ITERS=$(FUZZ_ITERS) FUZZ_SAN_FLAGS="$(FUZZ_SAN_FLAGS)"
