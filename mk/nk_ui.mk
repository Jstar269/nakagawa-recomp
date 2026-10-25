# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
# One owned inventory for current and reserved nk_ui build inputs.
NK_UI_CORE_SRCS := \
	src/ui/nk_ui.c \
	src/ui/nk_ui_clay.c \
	src/ui/nk_ui_theme.c \
	src/ui/nk_ui_draw.c \
	src/ui/nk_ui_text.c \
	src/ui/nk_ui_widgets.c \
	src/ui/nk_ui_waves.c \
	src/ui/nk_ui_dump.c \
	src/ui/nk_ui_focus.c \
	src/ui/nk_ui_input.c \
	src/ui/nk_ui_prompts.c \
	src/ui/nk_ui_image.c \
	src/ui/nk_ui_assets.c

NK_UI_EDGE_SRCS := \
	src/ui/nk_ui_sdl_input.c \
	src/ui/nk_ui_ttf.c

NK_UI_BACKEND_SRCS := \
	src/ui/backend/nk_ui_sdlr.c \
	src/ui/backend/nk_ui_vk.c

NK_UI_PLAYER_SCREEN_SRCS := \
	src/player/ui/player_ui_nk.c \
	src/player/ui/ui_router.c \
	src/player/ui/scr_library.c \
	src/player/ui/scr_title.c \
	src/player/ui/scr_settings.c \
	src/player/ui/scr_first_run.c \
	src/player/ui/scr_dialogs.c

NK_UI_PLAYER_C5_SRCS := \
	src/player/ui/player_ui_nk.c \
	src/player/ui/ui_router.c \
	src/player/ui/scr_library.c \
	src/player/ui/scr_title.c

# The next UI's SDL host pieces: disc art on a worker thread, launcher sounds.
NK_UI_PLAYER_HOST_SRCS := \
	src/player/ui/player_ui_art.c \
	src/player/ui/player_ui_sound.c

NK_UI_OVERLAY_SRCS := \
	src/rt/overlay/nk_overlay.c \
	src/rt/overlay/overlay_pause.c \
	src/rt/overlay/overlay_quick.c \
	src/rt/overlay/overlay_hud.c \
	src/rt/overlay/overlay_toast.c \
	src/rt/overlay/overlay_dialogs.c \
	src/rt/overlay/overlay_controls.c \
	src/rt/overlay/overlay_host.c

# The overlay pages built so far (C9c); the HUD port and toasts keep their reserved names.
# They are SDL-free; overlay_host.c is the runtime's SDL side and builds only into it.
NK_UI_OVERLAY_C9C_SRCS := \
	src/rt/overlay/nk_overlay.c \
	src/rt/overlay/overlay_pause.c \
	src/rt/overlay/overlay_quick.c \
	src/rt/overlay/overlay_dialogs.c \
	src/rt/overlay/overlay_controls.c

NK_UI_OVERLAY_HOST_SRCS := src/rt/overlay/overlay_host.c

NK_UI_NATIVE_CORE_SRCS := $(NK_UI_CORE_SRCS) $(NK_UI_EDGE_SRCS) $(NK_UI_BACKEND_SRCS)

NK_UI_TEST_SRCS := \
	tests/native/test_nk_ui_core.c \
	tests/native/test_nk_ui_focus.c \
	tests/native/test_nk_ui_sdlr.c \
	tests/native/test_nk_ui_vk.c \
	tests/native/test_player_screens.c \
	tests/native/test_overlay_screens.c
