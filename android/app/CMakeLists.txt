# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

# Native half of the Android app, driven by Gradle (android/app/build.gradle.kts) through the
# NDK's CMake toolchain, once per ABI. It builds, from source:
#   - SDL3 3.4.18 and SDL3_ttf 3.2.2 at the commits hosted CI pins for Linux (libSDL3.so,
#     libSDL3_ttf.so; SDL3_ttf with its vendored FreeType and HarfBuzz);
#   - the player (launcher) as libmain.so, from the repository's own CMakeLists.txt;
#   - one runtime library per bundled title, libnk_title_<id>.so, from a package's generated C
#     (nk_add_title_runtime in the top-level CMakeLists.txt). Packages are built on a PC by the
#     ordinary route (fixtures/showcase/showcase.py for the showcase demos) and named by
#     NK_ANDROID_TITLES; no game data and no generated C is part of the repository.
# The dependency sources are fetched once into NK_ANDROID_DEPS_DIR (default build/android-deps,
# ignored) and checked against the pinned commits (deps.cmake).

cmake_minimum_required(VERSION 3.22)
project(nakagawa_android LANGUAGES C)

get_filename_component(NK_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NK_ANDROID_DEPS_DIR "${NK_REPO_ROOT}/build/android-deps" CACHE PATH
    "Where the pinned SDL3 and SDL3_ttf sources are fetched")
# Semicolon-separated <title-id>=<package-dir> pairs.
set(NK_ANDROID_TITLES "" CACHE STRING "Bundled title packages: <title-id>=<package-dir>;...")

include("${CMAKE_CURRENT_SOURCE_DIR}/deps.cmake")   # NK_SDL3_DIR, NK_SDL3_TTF_DIR

# SDL3: the shared library SDLActivity loads, with Vulkan.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory("${NK_SDL3_DIR}" sdl3 EXCLUDE_FROM_ALL)

# SDL3_ttf with the vendored FreeType and HarfBuzz, as CI builds it for Linux.
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_STRICT ON CACHE BOOL "" FORCE)
add_subdirectory("${NK_SDL3_TTF_DIR}" sdl3_ttf EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# The repository's own build: nakagawa_core, the player (libmain.so on Android) and
# nk_add_title_runtime(). EXCLUDE_FROM_ALL keeps its host tests out of the Android build.
set(BUILD_PLAYER ON CACHE BOOL "" FORCE)
add_subdirectory("${NK_REPO_ROOT}" nk EXCLUDE_FROM_ALL)

set(nk_android_targets nakagawa_player SDL3-shared SDL3_ttf-shared)
foreach(pair IN LISTS NK_ANDROID_TITLES)
    if(NOT pair MATCHES "^([A-Za-z0-9._-]+)=(.+)$")
        message(FATAL_ERROR "NK_ANDROID_TITLES entry '${pair}' is not <title-id>=<package-dir>")
    endif()
    set(title_id "${CMAKE_MATCH_1}")
    file(TO_CMAKE_PATH "${CMAKE_MATCH_2}" package_dir)   # Gradle on Windows passes backslashes
    set(target "nk_title_${title_id}")
    nk_add_title_runtime(${target} "${package_dir}" "${title_id}")
    # The Android process start and end (pid and exit status for the launcher).
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/nk_android_title.c")
    target_include_directories(${target} PRIVATE "${NK_REPO_ROOT}/src/core")
    # exit() and _exit() in the runtime go through the shim first (see nk_android_title.c).
    target_link_options(${target} PRIVATE -Wl,--wrap=exit -Wl,--wrap=_exit -Wl,--wrap=_Exit)
    list(APPEND nk_android_targets ${target})
endforeach()

# Gradle builds this target; it pulls in everything the APK packages.
add_custom_target(nk_android ALL)
add_dependencies(nk_android ${nk_android_targets})
