# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

cmake_minimum_required(VERSION 3.20)
project(nakagawa_recomp LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# -----------------------------------------------------------------------------
# Core Recompilation & Platform Library
# -----------------------------------------------------------------------------
set(CORE_SOURCES
    src/core/nk_font.c
    src/core/nk_pgf_host.c
    src/rt/pgf_public.c
    src/core/nk_iso.c
    src/core/nk_library.c
    src/core/nk_launch.c
    src/core/nk_title_manifest.c
    src/core/nk_xb.c
    src/core/nk_input_profile.c
    src/core/nk_json.c
    src/core/nk_psp_crypto.c
    src/core/nk_psp_keystore.c
    src/core/nk_psp_builtin_keys.c
    src/core/nk_psp_aes.c
    src/core/nk_psp_sha1.c
    src/core/nk_psp_ec.c
    src/core/nk_psp_kirk.c
    src/core/nk_psp_prx.c
    src/core/nk_psp_kle.c
    src/core/nk_psp_inflate.c
    src/core/nk_psp_container.c
    src/core/nk_psp_amctrl.c
    src/core/nk_psp_lzrc.c
    src/core/nk_psp_npumdimg.c
    src/core/generated/nk_title_catalog.c
)

if(WIN32)
    list(APPEND CORE_SOURCES src/core/nk_platform_win32.c)
else()
    list(APPEND CORE_SOURCES src/core/nk_platform_posix.c)
endif()

add_library(nakagawa_core STATIC ${CORE_SOURCES})
if(WIN32)
    target_link_libraries(nakagawa_core PUBLIC shell32 ole32 uuid)
endif()

target_include_directories(nakagawa_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rt
)

# CMake has no private PGF/PGD/ISO/audio backend to select, so this library is always the
# public-safe build. It selects the public PGF reader (src/rt/pgf_public.c), and the Makefile's
# PUBLIC_SAFE=1 branch defines SR_PUBLIC_SAFE for every object. PUBLIC carries the same define to
# the player and to the tests that link this library. tools/test_public_ci_wiring.py checks that
# both build files agree.
target_compile_definitions(nakagawa_core PUBLIC SR_PUBLIC_SAFE)

# -----------------------------------------------------------------------------
# Native Core Test Harness
# -----------------------------------------------------------------------------
enable_testing()
# Several native CTests write relative scratch files under build/. Keep that
# directory in the CMake binary tree for out-of-source builds.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build")

add_executable(test_core_catalog
    tests/native/test_core_catalog.c
    tests/native/native_test_isolation.c
)
target_link_libraries(test_core_catalog PRIVATE nakagawa_core)
add_test(NAME core_catalog_test COMMAND test_core_catalog)

add_executable(test_parsers_hostile
    tests/native/test_parsers_hostile.c
    tests/native/native_test_isolation.c
)
target_link_libraries(test_parsers_hostile PRIVATE nakagawa_core)
add_test(NAME parsers_hostile_test COMMAND test_parsers_hostile)

add_executable(test_manifest_parser tests/native/test_manifest_parser.c)
target_link_libraries(test_manifest_parser PRIVATE nakagawa_core)
add_test(NAME manifest_parser_test COMMAND test_manifest_parser --check)

add_executable(test_launch_resolution
    tests/native/test_launch_resolution.c
    tests/native/native_test_isolation.c
)
target_link_libraries(test_launch_resolution PRIVATE nakagawa_core)
add_test(NAME launch_resolution_test COMMAND test_launch_resolution)

add_executable(test_pgf_public
    tests/native/test_pgf_public.c
    src/rt/pgf_public.c
)
target_include_directories(test_pgf_public PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rt
)
# This target compiles src/rt/pgf_public.c itself instead of linking nakagawa_core, so it
# carries the public-safe define directly.
target_compile_definitions(test_pgf_public PRIVATE SR_PUBLIC_SAFE)
add_test(NAME pgf_public_test COMMAND test_pgf_public)

add_executable(test_xb_parser
    tests/native/test_xb_parser.c
    tests/native/native_test_isolation.c
    src/player/setup_staging.c
    src/rt/archive_vfs.c
)
target_include_directories(test_xb_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rt
)
target_link_libraries(test_xb_parser PRIVATE nakagawa_core)
add_test(NAME xb_parser_test COMMAND test_xb_parser)

# player_state.c is deliberately free of SDL, so the library-state regression
# links here without pulling the player application into the test build.
add_executable(test_player_state
    tests/native/test_player_state.c
    tests/native/native_test_isolation.c
    src/player/player_state.c
    src/player/input_settings.c
    src/player/iso_reader.c
    src/player/package_builder.c
    src/player/setup_staging.c
)
target_include_directories(test_player_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)
target_link_libraries(test_player_state PRIVATE nakagawa_core)
if(WIN32)
    target_link_libraries(test_player_state PRIVATE winhttp bcrypt)
endif()
add_test(NAME player_state_test COMMAND test_player_state)
# The sample player state uses a source-relative fixture path, so this CTest
# must run from the source tree just like the Makefile test target.
# Scratch files go to the binary tree, so an out-of-source build is
# self-sufficient and never needs <source>/build to exist.
set_tests_properties(player_state_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    ENVIRONMENT "NK_TEST_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/build")

# The nk_ui core and its C3a reserved stubs stay SDL and Vulkan free. Keep this
# explicit source inventory in sync with mk/nk_ui.mk.
set(NK_UI_CORE_SOURCES
    src/ui/nk_ui.c
    src/ui/nk_ui_clay.c
    src/ui/nk_ui_theme.c
    src/ui/nk_ui_draw.c
    src/ui/nk_ui_text.c
    src/ui/nk_ui_widgets.c
    src/ui/nk_ui_waves.c
    src/ui/nk_ui_dump.c
    src/ui/nk_ui_focus.c
    src/ui/nk_ui_input.c
    src/ui/nk_ui_prompts.c
    src/ui/nk_ui_image.c
    src/ui/nk_ui_assets.c
)
set(NK_UI_EDGE_SOURCES
    src/ui/nk_ui_sdl_input.c
    src/ui/nk_ui_ttf.c
)
set(NK_UI_BACKEND_SOURCES
    src/ui/backend/nk_ui_sdlr.c
    src/ui/backend/nk_ui_vk.c
)
set(NK_UI_RESERVED_STUB_SOURCES ${NK_UI_EDGE_SOURCES} ${NK_UI_BACKEND_SOURCES})
set(NK_UI_PLAYER_SCREEN_SOURCES
    src/player/ui/player_ui_nk.c
    src/player/ui/ui_router.c
    src/player/ui/scr_library.c
    src/player/ui/scr_title.c
    src/player/ui/scr_settings.c
    src/player/ui/scr_first_run.c
    src/player/ui/scr_dialogs.c
)
set(NK_UI_PLAYER_C5_SOURCES
    src/player/ui/player_ui_nk.c
    src/player/ui/ui_router.c
    src/player/ui/scr_library.c
    src/player/ui/scr_title.c
)
# The next UI's SDL host pieces: disc art on a worker thread, launcher sounds.
set(NK_UI_PLAYER_HOST_SOURCES
    src/player/ui/player_ui_art.c
    src/player/ui/player_ui_sound.c
)
set(NK_UI_OVERLAY_SOURCES
    src/rt/overlay/nk_overlay.c
    src/rt/overlay/overlay_pause.c
    src/rt/overlay/overlay_quick.c
    src/rt/overlay/overlay_hud.c
    src/rt/overlay/overlay_toast.c
    src/rt/overlay/overlay_dialogs.c
    src/rt/overlay/overlay_controls.c
    src/rt/overlay/overlay_dev.c
    src/rt/overlay/overlay_host.c
)
# The overlay pages built so far (C9c, and the developer pages of #314), SDL-free; overlay_host.c builds only into the runtime.
set(NK_UI_OVERLAY_C9C_SOURCES
    src/rt/overlay/nk_overlay.c
    src/rt/overlay/overlay_pause.c
    src/rt/overlay/overlay_quick.c
    src/rt/overlay/overlay_dialogs.c
    src/rt/overlay/overlay_controls.c
    src/rt/overlay/overlay_dev.c
)
set(NK_UI_TEST_SOURCES
    tests/native/test_nk_ui_core.c
    tests/native/test_nk_ui_focus.c
    tests/native/test_nk_ui_sdlr.c
    tests/native/test_nk_ui_vk.c
    tests/native/test_player_screens.c
    tests/native/test_overlay_screens.c
)
add_executable(test_nk_ui_core
    tests/native/test_nk_ui_core.c
    ${NK_UI_CORE_SOURCES}
    ${NK_UI_RESERVED_STUB_SOURCES}
)
target_include_directories(test_nk_ui_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
)
if(NOT WIN32)
    target_link_libraries(test_nk_ui_core PRIVATE m)
endif()
add_test(NAME nk_ui_core_test COMMAND test_nk_ui_core)
set_tests_properties(nk_ui_core_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# C5 Library, Title detail and router snapshots use only the SDL-free nk_ui
# core. Hosted Linux can run them without SDL3 headers or a graphics driver.
add_executable(test_player_screens
    tests/native/test_player_screens.c
    ${NK_UI_CORE_SOURCES}
    src/player/ui/ui_router.c
    src/player/ui/scr_library.c
    src/player/ui/scr_title.c
)
target_include_directories(test_player_screens PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
)
target_compile_definitions(test_player_screens PRIVATE
    SR_SELFTEST_BUILD_ROOT="${CMAKE_CURRENT_BINARY_DIR}/build"
)
if(NOT WIN32)
    target_link_libraries(test_player_screens PRIVATE m)
endif()
add_test(NAME player_screens_test COMMAND test_player_screens)
set_tests_properties(player_screens_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The in-game overlay's pages (#1159, C9c): flow, focus, fit and fixtures/nk_ui/overlay.golden.
add_executable(test_overlay_screens
    tests/native/test_overlay_screens.c
    ${NK_UI_CORE_SOURCES}
    ${NK_UI_OVERLAY_C9C_SOURCES}
)
target_include_directories(test_overlay_screens PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rt/overlay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
)
# The controls page reads the input profile's keys and their names.
target_link_libraries(test_overlay_screens PRIVATE nakagawa_core)
if(NOT WIN32)
    target_link_libraries(test_overlay_screens PRIVATE m)
endif()
add_test(NAME overlay_screens_test COMMAND test_overlay_screens)
set_tests_properties(overlay_screens_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The SDL_Renderer backend and SDL3_ttf text engine (lane C3b). With SDL3 and
# SDL3_ttf headers found the test is built for real; without them it is the SKIP
# variant, and ctest reports exit 77 as skipped. SDL3_ttf itself is loaded at run
# time, so only its headers matter to the build.
find_package(SDL3 CONFIG QUIET)
find_package(SDL3_ttf CONFIG QUIET)
add_executable(test_nk_ui_sdlr
    tests/native/test_nk_ui_sdlr.c
    ${NK_UI_CORE_SOURCES}
    src/ui/nk_ui_ttf.c
    src/ui/backend/nk_ui_sdlr.c
)
target_include_directories(test_nk_ui_sdlr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
)
target_compile_definitions(test_nk_ui_sdlr PRIVATE
    SR_SELFTEST_BUILD_ROOT="${CMAKE_CURRENT_BINARY_DIR}/build"
)
if(TARGET SDL3::SDL3 AND TARGET SDL3_ttf::SDL3_ttf)
    target_compile_definitions(test_nk_ui_sdlr PRIVATE NK_UI_WITH_SDL NK_UI_WITH_SDL_TTF)
    target_link_libraries(test_nk_ui_sdlr PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)
endif()
if(NOT WIN32)
    target_link_libraries(test_nk_ui_sdlr PRIVATE m)
endif()
add_test(NAME nk_ui_sdlr_test COMMAND test_nk_ui_sdlr)
set_tests_properties(nk_ui_sdlr_test PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    SKIP_RETURN_CODE 77)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/build")
# Focus graph, input model and prompt glyphs: SDL-free. The SDL_Event translation leg is
# registered after SDL3 is resolved below, and reports SKIP (exit 77) without it.
add_executable(test_nk_ui_focus
    tests/native/test_nk_ui_focus.c
    ${NK_UI_CORE_SOURCES}
    src/ui/nk_ui_sdl_input.c
)
target_include_directories(test_nk_ui_focus PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
)
if(NOT WIN32)
    target_link_libraries(test_nk_ui_focus PRIVATE m)
endif()
add_test(NAME nk_ui_focus_test COMMAND test_nk_ui_focus)
set_tests_properties(nk_ui_focus_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_package_builder
    tests/native/test_package_builder.c
    tests/native/native_test_isolation.c
    src/player/package_builder.c
)
target_include_directories(test_package_builder PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)
target_link_libraries(test_package_builder PRIVATE nakagawa_core)
if(WIN32)
    target_link_libraries(test_package_builder PRIVATE winhttp bcrypt)
endif()
# The test resolves the CLI relative to the working directory, so it must run
# from the repository root (where tools/nk_cli.py lives), not from the build dir.
add_test(NAME package_builder_test COMMAND test_package_builder)
set_tests_properties(package_builder_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Check for updates on a fake transport and the recorded Releases API answer; it
# reads its fixture from the repository root and never touches the internet.
add_executable(test_update_check
    tests/native/test_update_check.c
    tests/native/native_test_isolation.c
    src/player/package_builder.c
    src/player/update_check.c
)
target_include_directories(test_update_check PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)
target_link_libraries(test_update_check PRIVATE nakagawa_core)
if(WIN32)
    target_link_libraries(test_update_check PRIVATE winhttp bcrypt)
endif()
add_test(NAME update_check_test COMMAND test_update_check)
set_tests_properties(update_check_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_input_settings
    tests/native/test_input_settings.c
    tests/native/native_test_isolation.c
    src/player/input_settings.c
)
target_include_directories(test_input_settings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/player
)
target_link_libraries(test_input_settings PRIVATE nakagawa_core)
add_test(NAME input_settings_test COMMAND test_input_settings)

if(WIN32)
    add_executable(argv_echo_helper tests/native/argv_echo_helper.c)
    target_link_libraries(argv_echo_helper PRIVATE shell32)

    add_executable(test_win32_process tests/native/test_win32_process.c)
    target_link_libraries(test_win32_process PRIVATE nakagawa_core)
    add_test(NAME win32_process_test COMMAND test_win32_process)
else()
    # The POSIX counterpart to test_win32_process. Registering only the Win32
    # one let a CTest build on Linux or macOS report success without ever
    # exercising the wait-timeout and exit-status behaviour of the backend it
    # actually compiled.
    add_executable(test_posix_process tests/native/test_posix_process.c)
    target_link_libraries(test_posix_process PRIVATE nakagawa_core)
    add_test(NAME posix_process_test COMMAND test_posix_process)
endif()

# -----------------------------------------------------------------------------
# Native Player Application (SDL3)
# -----------------------------------------------------------------------------
option(BUILD_PLAYER "Build the native cross-platform SDL3 player application" ON)
option(NK_BOOTSTRAP_SDL3 "Enable downloading and compiling pinned official SDL3 release if system package is not found" OFF)

if(BUILD_PLAYER)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        find_package(SDL3 QUIET)
    endif()

    if(NOT SDL3_FOUND AND NK_BOOTSTRAP_SDL3)
        message(STATUS "System SDL3 not found; bootstrapping pinned official release SDL3 3.4.18")
        include(FetchContent)
        FetchContent_Declare(
            SDL3
            URL "https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz"
            URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
        )
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    set(PLAYER_SOURCES
        src/player/main.c
        src/player/player_state.c
        src/player/input_settings.c
        src/player/iso_reader.c
        src/player/setup_staging.c
        src/player/ui_renderer.c
        src/player/package_builder.c
        src/player/update_check.c
        ${NK_UI_CORE_SOURCES}
        ${NK_UI_EDGE_SOURCES}
        src/ui/backend/nk_ui_sdlr.c
        ${NK_UI_PLAYER_C5_SOURCES}
        ${NK_UI_PLAYER_HOST_SOURCES}
    )

    if(ANDROID)
        # Android starts the launcher from Java (SDLActivity), which loads libmain.so and calls
        # its main(); see android/ and docs/ANDROID_DEVELOPMENT.md.
        add_library(nakagawa_player SHARED ${PLAYER_SOURCES})
        set_target_properties(nakagawa_player PROPERTIES OUTPUT_NAME main)
        target_link_libraries(nakagawa_player PRIVATE log android)
    else()
        add_executable(nakagawa_player ${PLAYER_SOURCES})
    endif()
    target_include_directories(nakagawa_player PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/player
        ${CMAKE_CURRENT_SOURCE_DIR}/src/player/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
    )
    target_compile_definitions(nakagawa_player PRIVATE NK_UI_WITH_SDL NK_UI_WITH_SDL3)
    # The About page's build date: SOURCE_DATE_EPOCH when set, else the source
    # commit's time (the Makefile does the same; see NK_PLAYER_BUILD_EPOCH).
    set(NK_PLAYER_BUILD_EPOCH "$ENV{SOURCE_DATE_EPOCH}")
    if(NOT NK_PLAYER_BUILD_EPOCH)
        find_package(Git QUIET)
        if(GIT_FOUND)
            execute_process(COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ct
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE NK_PLAYER_BUILD_EPOCH
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        endif()
    endif()
    if(NK_PLAYER_BUILD_EPOCH MATCHES "^[0-9]+$")
        target_compile_definitions(nakagawa_player PRIVATE
            NK_PLAYER_BUILD_EPOCH=${NK_PLAYER_BUILD_EPOCH}LL)
    endif()
    target_link_libraries(nakagawa_player PRIVATE nakagawa_core)
    add_custom_command(TARGET nakagawa_player POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/ui"
            "$<TARGET_FILE_DIR:nakagawa_player>/assets/fonts/ui"
        COMMENT "Staging the bundled UI font assets beside nakagawa_player"
        VERBATIM
    )
    if(WIN32)
        target_link_libraries(nakagawa_player PRIVATE winhttp bcrypt)
    else()
        target_link_libraries(nakagawa_player PRIVATE m)
    endif()

    if(TARGET SDL3::SDL3)
        target_link_libraries(nakagawa_player PRIVATE SDL3::SDL3)
        set(NK_UI_SDL3_LINK SDL3::SDL3)
    elseif(TARGET SDL3::SDL3-shared)
        target_link_libraries(nakagawa_player PRIVATE SDL3::SDL3-shared)
        set(NK_UI_SDL3_LINK SDL3::SDL3-shared)
    elseif(SDL3_LIBRARIES)
        target_link_libraries(nakagawa_player PRIVATE ${SDL3_LIBRARIES})
        if(SDL3_INCLUDE_DIRS)
            target_include_directories(nakagawa_player PRIVATE ${SDL3_INCLUDE_DIRS})
        endif()
    else()
        if(WIN32)
            message(FATAL_ERROR "SDL3 dependency is missing. Install with: pacman -S mingw-w64-ucrt-x86_64-sdl3 (or enable -DNK_BOOTSTRAP_SDL3=ON)")
        else()
            message(FATAL_ERROR "SDL3 dependency is missing. Install with: sudo apt install libsdl3-dev (or enable -DNK_BOOTSTRAP_SDL3=ON)")
        endif()
    endif()

    if(TARGET SDL3_ttf::SDL3_ttf)
        target_compile_definitions(nakagawa_player PRIVATE NK_UI_WITH_SDL_TTF)
        target_link_libraries(nakagawa_player PRIVATE SDL3_ttf::SDL3_ttf)
    endif()

    if(WIN32)
        # GUI subsystem, so a double-click opens no console window (see the Makefile PLAYER_GUI_LDFLAGS note)
        set_target_properties(nakagawa_player PROPERTIES WIN32_EXECUTABLE TRUE)
        # The app icon; SDL gives every window the executable's first icon group (see the
        # Makefile PLAYER_ICON_OBJ note). Other systems set it at run time instead.
        enable_language(RC)
        target_sources(nakagawa_player PRIVATE assets/brand/nakagawa.rc)
        if(DEFINED ENV{VULKAN_SDK})
            target_include_directories(nakagawa_player PRIVATE
                "$ENV{VULKAN_SDK}/Include"
                "$ENV{VULKAN_SDK}/include"
            )
            if(EXISTS "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
                target_link_libraries(nakagawa_player PRIVATE "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
            elseif(EXISTS "$ENV{VULKAN_SDK}/lib/vulkan-1.lib")
                target_link_libraries(nakagawa_player PRIVATE "$ENV{VULKAN_SDK}/lib/vulkan-1.lib")
            else()
                target_link_libraries(nakagawa_player PRIVATE vulkan-1)
            endif()
        endif()
    endif()
endif()

# SDL_Event translation leg of the nk_ui input tests (exit 77 is a SKIP, never a pass).
if(NK_UI_SDL3_LINK)
    add_executable(test_nk_ui_focus_sdl
        tests/native/test_nk_ui_focus.c
        ${NK_UI_CORE_SOURCES}
        src/ui/nk_ui_sdl_input.c
    )
    target_compile_definitions(test_nk_ui_focus_sdl PRIVATE NK_UI_WITH_SDL3)
    target_include_directories(test_nk_ui_focus_sdl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clay
    )
    target_link_libraries(test_nk_ui_focus_sdl PRIVATE ${NK_UI_SDL3_LINK})
    if(NOT WIN32)
        target_link_libraries(test_nk_ui_focus_sdl PRIVATE m)
    endif()
    add_test(NAME nk_ui_sdl_input_test COMMAND test_nk_ui_focus_sdl --sdl-input)
else()
    add_test(NAME nk_ui_sdl_input_test COMMAND test_nk_ui_focus --sdl-input)
endif()
set_tests_properties(nk_ui_sdl_input_test PROPERTIES SKIP_RETURN_CODE 77
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# -----------------------------------------------------------------------------
# Title runtime: one game's generated C linked with the runtime
# -----------------------------------------------------------------------------
# The Makefile links a title's runtime as an executable (driven by tools/title_codegen_plan.py).
# nk_add_title_runtime() builds the same translation units from a package's generated sources
# with CMake; the Android build (android/) uses it to make libnk_title_<id>.so. The inventory
# mirrors the Makefile's RT_SRCS (public backends, POSIX platform), RT_GE_O, ATRAC3P_SRCS plus
# the bridge, and H264_SUB_SRCS; tools/test_android_build.py checks that they agree.
set(NK_RUNTIME_SOURCES
    src/rt/recomp.c
    src/rt/flight_recorder.c
    src/rt/cpu_lle.c
    src/rt/domain_mode.c
    src/rt/nested_frames.c
    src/rt/stale_code.c
    src/rt/guest_interp.c
    src/rt/title_config.c
    src/rt/vfpu_tables.c
    src/rt/archive_vfs.c
    src/rt/debug.c
    src/rt/watchpoints_file.c
    src/rt/guest_printf.c
    src/rt/perf.c
    src/rt/fbcap_policy.c
    src/rt/fbcap.c
    src/rt/ge_capture.c
    src/rt/vfpu_interp.c
    src/rt/hle.c
    src/rt/audio_resample.c
    src/rt/flash0_font.c
    src/rt/mp3_bridge.c
    src/core/nk_xb.c
    src/rt/hle_power.c
    src/rt/prx_loader.c
    src/rt/sched.c
    src/rt/sr_coro.c
    src/rt/iso_public.c
    src/rt/pgd_unavailable.c
    src/rt/mpeg.c
    src/rt/psmf_producer.c
    src/rt/pgf_public.c
    src/rt/gui.c
    src/rt/audio_unavailable.c
    src/rt/h264_mf.c
    src/rt/h264_null.c
    src/rt/h264_ffmpeg.c
    src/rt/savedata.c
    src/rt/osk_win.c
    src/rt/osk_text_entry.c
    src/rt/osk_overlay.c
    src/rt/osk_overlay_paint.c
    src/rt/driver.c
    src/rt/gpu_sdl3vk/sdl3vk.c
    src/rt/gpu_sdl3vk/ge_gpu.c
    src/core/nk_input_profile.c
    src/core/nk_font.c
    src/core/nk_json.c
    src/core/nk_platform_posix.c
)
set(NK_RUNTIME_GE_SOURCE src/rt/ge.c)
set(NK_RUNTIME_ATRAC3P_SOURCES
    src/rt/atrac3p/atrac3p_api.c
    src/rt/atrac3p/libavcodec/atrac.c
    src/rt/atrac3p/libavcodec/atrac3.c
    src/rt/atrac3p/libavcodec/atrac3plus.c
    src/rt/atrac3p/libavcodec/atrac3plusdec.c
    src/rt/atrac3p/libavcodec/atrac3plusdsp.c
    src/rt/atrac3p/libavcodec/bitstream.c
    src/rt/atrac3p/libavcodec/fft_float.c
    src/rt/atrac3p/libavcodec/fft_init_table.c
    src/rt/atrac3p/libavcodec/mdct_float.c
    src/rt/atrac3p/libavcodec/sinewin.c
    src/rt/atrac3p/libavutil/float_dsp.c
    src/rt/atrac3p/libavutil/intmath.c
    src/rt/atrac3p/libavutil/log2_tab.c
    src/rt/atrac3p/libavutil/mem.c
    src/rt/atrac3p/libavutil/reverse.c
)
set(NK_RUNTIME_ATRAC3P_BRIDGE_SOURCE src/rt/atrac3p_bridge.c)
set(NK_RUNTIME_H264_SUB_SOURCES
    src/rt/h264_ffmpeg_sub/nk_h264_api.c
    src/rt/h264_ffmpeg_sub/libavcodec/allcodecs.c
    src/rt/h264_ffmpeg_sub/libavcodec/atsc_a53.c
    src/rt/h264_ffmpeg_sub/libavcodec/avcodec.c
    src/rt/h264_ffmpeg_sub/libavcodec/avpacket.c
    src/rt/h264_ffmpeg_sub/libavcodec/bitstream.c
    src/rt/h264_ffmpeg_sub/libavcodec/bitstream_filters.c
    src/rt/h264_ffmpeg_sub/libavcodec/bsf.c
    src/rt/h264_ffmpeg_sub/libavcodec/cabac.c
    src/rt/h264_ffmpeg_sub/libavcodec/codec_desc.c
    src/rt/h264_ffmpeg_sub/libavcodec/codec_par.c
    src/rt/h264_ffmpeg_sub/libavcodec/decode.c
    src/rt/h264_ffmpeg_sub/libavcodec/encode.c
    src/rt/h264_ffmpeg_sub/libavcodec/error_resilience.c
    src/rt/h264_ffmpeg_sub/libavcodec/golomb.c
    src/rt/h264_ffmpeg_sub/libavcodec/h2645_parse.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_cabac.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_cavlc.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_direct.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_loopfilter.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_mb.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_parse.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_picture.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_ps.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_refs.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_sei.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264_slice.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264chroma.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264data.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264dec.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264dsp.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264idct.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264pred.c
    src/rt/h264_ffmpeg_sub/libavcodec/h264qpel.c
    src/rt/h264_ffmpeg_sub/libavcodec/mathtables.c
    src/rt/h264_ffmpeg_sub/libavcodec/me_cmp.c
    src/rt/h264_ffmpeg_sub/libavcodec/null_bsf.c
    src/rt/h264_ffmpeg_sub/libavcodec/options.c
    src/rt/h264_ffmpeg_sub/libavcodec/profiles.c
    src/rt/h264_ffmpeg_sub/libavcodec/simple_idct.c
    src/rt/h264_ffmpeg_sub/libavcodec/startcode.c
    src/rt/h264_ffmpeg_sub/libavcodec/utils.c
    src/rt/h264_ffmpeg_sub/libavcodec/videodsp.c
    src/rt/h264_ffmpeg_sub/libavutil/avstring.c
    src/rt/h264_ffmpeg_sub/libavutil/bprint.c
    src/rt/h264_ffmpeg_sub/libavutil/buffer.c
    src/rt/h264_ffmpeg_sub/libavutil/channel_layout.c
    src/rt/h264_ffmpeg_sub/libavutil/dict.c
    src/rt/h264_ffmpeg_sub/libavutil/display.c
    src/rt/h264_ffmpeg_sub/libavutil/error.c
    src/rt/h264_ffmpeg_sub/libavutil/eval.c
    src/rt/h264_ffmpeg_sub/libavutil/fifo.c
    src/rt/h264_ffmpeg_sub/libavutil/file_open.c
    src/rt/h264_ffmpeg_sub/libavutil/frame.c
    src/rt/h264_ffmpeg_sub/libavutil/hwcontext.c
    src/rt/h264_ffmpeg_sub/libavutil/imgutils.c
    src/rt/h264_ffmpeg_sub/libavutil/log.c
    src/rt/h264_ffmpeg_sub/libavutil/log2_tab.c
    src/rt/h264_ffmpeg_sub/libavutil/mathematics.c
    src/rt/h264_ffmpeg_sub/libavutil/mem.c
    src/rt/h264_ffmpeg_sub/libavutil/opt.c
    src/rt/h264_ffmpeg_sub/libavutil/parseutils.c
    src/rt/h264_ffmpeg_sub/libavutil/pixdesc.c
    src/rt/h264_ffmpeg_sub/libavutil/random_seed.c
    src/rt/h264_ffmpeg_sub/libavutil/rational.c
    src/rt/h264_ffmpeg_sub/libavutil/reverse.c
    src/rt/h264_ffmpeg_sub/libavutil/samplefmt.c
    src/rt/h264_ffmpeg_sub/libavutil/sha.c
    src/rt/h264_ffmpeg_sub/libavutil/stereo3d.c
    src/rt/h264_ffmpeg_sub/libavutil/time.c
    src/rt/h264_ffmpeg_sub/libavutil/timecode.c
    src/rt/h264_ffmpeg_sub/libavutil/utils.c
    src/rt/h264_ffmpeg_sub/libavutil/video_enc_params.c
)

# nk_add_title_runtime(<target> <package-dir> <title-id>)
# <package-dir> holds a package's generated sources: <title-id>_recomp.c, <title-id>_recomp_<n>.c,
# <title-id>_recomp_funcs.h, any nk_lib_*.c, and sr_title_config.h. It is a build input outside
# the repository (generated C is never committed). Needs the SDL3::SDL3 target, and
# SDL3_ttf::SDL3_ttf for its headers (the library itself is loaded at run time).
function(nk_add_title_runtime target package_dir title_id)
    # Called from another project (android/app): the repository root and its source lists are
    # this file's directory and that directory's variables, not the caller's.
    set(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    foreach(list_name NK_RUNTIME_SOURCES NK_RUNTIME_GE_SOURCE NK_RUNTIME_ATRAC3P_SOURCES
            NK_RUNTIME_ATRAC3P_BRIDGE_SOURCE NK_RUNTIME_H264_SUB_SOURCES NK_UI_CORE_SOURCES
            NK_UI_EDGE_SOURCES NK_UI_OVERLAY_C9C_SOURCES)
        get_directory_property(${list_name} DIRECTORY "${root}" DEFINITION ${list_name})
    endforeach()
    file(GLOB generated CONFIGURE_DEPENDS
        "${package_dir}/${title_id}_recomp.c"
        "${package_dir}/${title_id}_recomp_*.c"
        "${package_dir}/nk_lib_*.c")
    if(NOT generated OR NOT EXISTS "${package_dir}/sr_title_config.h")
        message(FATAL_ERROR "nk_add_title_runtime: ${package_dir} has no generated sources for "
            "${title_id} (expected ${title_id}_recomp*.c and sr_title_config.h)")
    endif()
    list(TRANSFORM NK_RUNTIME_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE rt_sources)
    list(TRANSFORM NK_UI_CORE_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE ui_sources)
    list(TRANSFORM NK_UI_EDGE_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE ui_edge)
    list(TRANSFORM NK_UI_OVERLAY_C9C_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE overlay)
    list(TRANSFORM NK_RUNTIME_ATRAC3P_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE atrac)
    list(TRANSFORM NK_RUNTIME_H264_SUB_SOURCES PREPEND "${root}/" OUTPUT_VARIABLE h264sub)
    set(rt_defs SR_SDL3VK SR_FLIGHT_RECORDER_LINKED PERF_AOT_INSTRUCTIONS=0 SR_PUBLIC_SAFE
        SR_H264_FFMPEG)

    # The runtime proper: the Makefile's CFLAGS, with RT_NK_UI_FLAGS and HLE_INCLUDES, which add
    # only include paths and the NK_UI_WITH_* switches the UI files read. The top-level -Werror
    # is the host tests' policy; the Makefile builds the runtime without it.
    add_library(${target}_rt OBJECT ${rt_sources} ${ui_sources} ${ui_edge}
        "${root}/src/ui/backend/nk_ui_sdlr.c" ${overlay} "${root}/src/rt/overlay/overlay_host.c")
    target_include_directories(${target}_rt PRIVATE
        "${root}/src/rt" "${root}/src/core" "${root}/src/ui" "${root}/src/rt/overlay"
        "${root}/third_party/clay" "${root}/src/rt/atrac3p" "${root}/src/rt/atrac3p/libavcodec"
        "${root}/src/rt/atrac3p/libavutil" "${package_dir}")
    target_compile_options(${target}_rt PRIVATE -O2 -fno-strict-aliasing -Wno-error)
    target_compile_definitions(${target}_rt PRIVATE ${rt_defs} NK_UI_WITH_SDL NK_UI_WITH_SDL3
        NK_UI_WITH_SDL_TTF)
    target_link_libraries(${target}_rt PRIVATE SDL3::SDL3)
    if(TARGET SDL3_ttf::SDL3_ttf)
        target_include_directories(${target}_rt PRIVATE
            $<TARGET_PROPERTY:SDL3_ttf::SDL3_ttf,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()

    # RT_GE_O: GE_CFLAGS.
    add_library(${target}_ge OBJECT "${root}/${NK_RUNTIME_GE_SOURCE}")
    target_include_directories(${target}_ge PRIVATE "${root}/src/rt")
    target_compile_options(${target}_ge PRIVATE -O2 -fno-math-errno -Wno-error)
    target_compile_definitions(${target}_ge PRIVATE SR_SDL3VK)

    # The ATRAC3+ import and its bridge: CFLAGS plus the import's include paths.
    add_library(${target}_atrac OBJECT ${atrac} "${root}/${NK_RUNTIME_ATRAC3P_BRIDGE_SOURCE}")
    target_include_directories(${target}_atrac PRIVATE "${root}/src/rt" "${root}/src/core"
        "${root}/src/rt/atrac3p" "${root}/src/rt/atrac3p/libavcodec"
        "${root}/src/rt/atrac3p/libavutil")
    target_compile_options(${target}_atrac PRIVATE -O2 -fno-strict-aliasing -Wno-error)
    target_compile_definitions(${target}_atrac PRIVATE ${rt_defs})
    target_link_libraries(${target}_atrac PRIVATE SDL3::SDL3)

    # The FFmpeg H.264 subset: the Makefile's h264sub_% rule (upstream code, warnings off).
    add_library(${target}_h264 OBJECT ${h264sub})
    target_include_directories(${target}_h264 PRIVATE "${root}/src/rt/h264_ffmpeg_sub"
        "${root}/src/rt")
    target_compile_options(${target}_h264 PRIVATE -O2 -fno-strict-aliasing -w -std=c11
        -include "${root}/src/rt/h264_ffmpeg_sub/nk_h264_symbols.h")
    target_compile_definitions(${target}_h264 PRIVATE ${rt_defs} HAVE_AV_CONFIG_H _ISOC99_SOURCE
        _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE _POSIX_C_SOURCE=200112 _XOPEN_SOURCE=600)

    # Generated C: RECOMP_FLAGS (-O0 -w), with the package directory on the include path.
    add_library(${target}_gen OBJECT ${generated})
    target_include_directories(${target}_gen PRIVATE "${package_dir}" "${root}/src/rt")
    target_compile_options(${target}_gen PRIVATE -O0 -w)
    target_compile_definitions(${target}_gen PRIVATE SR_SDL3VK SR_FLIGHT_RECORDER_LINKED)

    add_library(${target} SHARED
        $<TARGET_OBJECTS:${target}_rt> $<TARGET_OBJECTS:${target}_ge>
        $<TARGET_OBJECTS:${target}_atrac> $<TARGET_OBJECTS:${target}_h264>
        $<TARGET_OBJECTS:${target}_gen>)
    target_link_libraries(${target} PRIVATE SDL3::SDL3 vulkan m)
    if(ANDROID)
        target_link_libraries(${target} PRIVATE log android)
    endif()
endfunction()
