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

# -----------------------------------------------------------------------------
# Native Player Application (SDL3)
# -----------------------------------------------------------------------------
option(BUILD_PLAYER "Build the native cross-platform SDL3 player application" ON)

if(BUILD_PLAYER)
    find_package(SDL3 QUIET)

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
