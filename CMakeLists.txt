# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

cmake_minimum_required(VERSION 3.20)
project(nakagawa_recomp LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

# -----------------------------------------------------------------------------
# Core Recompilation & Platform Library
# -----------------------------------------------------------------------------
set(CORE_SOURCES
    src/core/nk_iso.c
    src/core/nk_library.c
    src/core/nk_launch.c
    src/core/nk_title_manifest.c
    src/core/generated/nk_title_catalog.c
)

if(WIN32)
    list(APPEND CORE_SOURCES src/core/nk_platform_win32.c)
else()
    list(APPEND CORE_SOURCES src/core/nk_platform_posix.c)
endif()

add_library(nakagawa_core STATIC ${CORE_SOURCES})

target_include_directories(nakagawa_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/generated
)

# -----------------------------------------------------------------------------
# Native Core Test Harness
# -----------------------------------------------------------------------------
enable_testing()

add_executable(test_core_catalog tests/native/test_core_catalog.c)
target_link_libraries(test_core_catalog PRIVATE nakagawa_core)
add_test(NAME core_catalog_test COMMAND test_core_catalog)

add_executable(test_parsers_hostile tests/native/test_parsers_hostile.c)
target_link_libraries(test_parsers_hostile PRIVATE nakagawa_core)
add_test(NAME parsers_hostile_test COMMAND test_parsers_hostile)

add_executable(test_manifest_parser tests/native/test_manifest_parser.c)
target_link_libraries(test_manifest_parser PRIVATE nakagawa_core)
add_test(NAME manifest_parser_test COMMAND test_manifest_parser --check)

if(WIN32)
    add_executable(argv_echo_helper tests/native/argv_echo_helper.c)
    target_link_libraries(argv_echo_helper PRIVATE shell32)

    add_executable(test_win32_process tests/native/test_win32_process.c)
    target_link_libraries(test_win32_process PRIVATE nakagawa_core)
    add_test(NAME win32_process_test COMMAND test_win32_process)
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
        message(STATUS "System SDL3 not found; bootstrapping pinned official release SDL3 3.2.4")
        include(FetchContent)
        FetchContent_Declare(
            SDL3
            URL "https://github.com/libsdl-org/SDL/releases/download/release-3.2.4/SDL3-3.2.4.tar.gz"
            URL_HASH SHA256=2938328317301dfbe30176d79c251733aa5e7ec5c436c800b99ed4da7adcb0f0
        )
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(SDL3)
    endif()

    set(PLAYER_SOURCES
        src/player/main.c
        src/player/player_state.c
        src/player/iso_reader.c
        src/player/ui_renderer.c
    )

    add_executable(nakagawa_player ${PLAYER_SOURCES})
    target_include_directories(nakagawa_player PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/player
    )
    target_link_libraries(nakagawa_player PRIVATE nakagawa_core)

    if(TARGET SDL3::SDL3)
        target_link_libraries(nakagawa_player PRIVATE SDL3::SDL3)
    elseif(SDL3_LIBRARIES)
        target_link_libraries(nakagawa_player PRIVATE ${SDL3_LIBRARIES})
        if(SDL3_INCLUDE_DIRS)
            target_include_directories(nakagawa_player PRIVATE ${SDL3_INCLUDE_DIRS})
        endif()
    else()
        # Fallback to direct library link
        target_link_libraries(nakagawa_player PRIVATE SDL3)
    endif()

    if(WIN32)
        # Windows subsystem or console
        if(DEFINED ENV{VULKAN_SDK})
            target_include_directories(nakagawa_player PRIVATE
                "$ENV{VULKAN_SDK}/Include"
                "$ENV{VULKAN_SDK}/include"
            )
            target_link_directories(nakagawa_player PRIVATE
                "$ENV{VULKAN_SDK}/Lib"
                "$ENV{VULKAN_SDK}/lib"
            )
        endif()
    endif()
endif()
