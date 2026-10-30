# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

# The pinned native dependencies of the Android app, fetched from source: SDL3 3.4.18 and
# SDL3_ttf 3.2.2 at the commits hosted CI builds for Linux (.github/workflows/ci.yml), SDL3_ttf
# with its FreeType, HarfBuzz and PlutoSVG submodules. Included by android/app/CMakeLists.txt, and
# run as a script (cmake -DNK_ANDROID_DEPS_DIR=<dir> -P deps.cmake) by Gradle before Java
# compilation, because the app's Java build uses SDL's android-project classes from the same
# checkout. A checkout already at the pinned commit is reused with no network access.

set(NK_SDL3_URL https://github.com/libsdl-org/SDL.git)
set(NK_SDL3_COMMIT 829a65d769d935c4852f8159e964312c0957260a)        # release-3.4.18
set(NK_SDL3_TTF_URL https://github.com/libsdl-org/SDL_ttf.git)
set(NK_SDL3_TTF_COMMIT a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b)    # release-3.2.2

if(NOT NK_ANDROID_DEPS_DIR)
    message(FATAL_ERROR "deps.cmake: set NK_ANDROID_DEPS_DIR")
endif()
find_package(Git REQUIRED)

# Fetch <url> at <commit> into <deps>/<name> unless it is already there. Gradle configures the
# ABIs in parallel, so a file lock serializes the fetch. core.longpaths: HarfBuzz's test data has
# paths longer than the Windows default limit.
function(nk_fetch_pinned name url commit submodules out_dir)
    set(dir "${NK_ANDROID_DEPS_DIR}/${name}")
    file(MAKE_DIRECTORY "${NK_ANDROID_DEPS_DIR}")
    file(LOCK "${NK_ANDROID_DEPS_DIR}/${name}.lock" GUARD FUNCTION TIMEOUT 3600)
    set(head "")
    if(EXISTS "${dir}/.git")
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
            OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT head STREQUAL commit)
        message(STATUS "Fetching ${name} ${commit} into ${dir}")
        file(REMOVE_RECURSE "${dir}")
        set(git "${GIT_EXECUTABLE}" -c core.longpaths=true -c advice.detachedHead=false)
        foreach(step
                "init;-q;${dir}"
                "-C;${dir};remote;add;origin;${url}"
                "-C;${dir};fetch;-q;--depth;1;origin;${commit}"
                "-C;${dir};checkout;-q;--detach;FETCH_HEAD")
            execute_process(COMMAND ${git} ${step} RESULT_VARIABLE rc)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "Fetching ${name} failed at: git ${step}")
            endif()
        endforeach()
        if(submodules)
            execute_process(COMMAND ${git} -C "${dir}" submodule update -q --init --depth 1
                RESULT_VARIABLE rc)
            if(NOT rc EQUAL 0)
                message(FATAL_ERROR "Fetching ${name} submodules failed")
            endif()
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
            OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT head STREQUAL commit)
            message(FATAL_ERROR "${name}: fetched ${head}, expected ${commit}")
        endif()
    endif()
    set(${out_dir} "${dir}" PARENT_SCOPE)
endfunction()

nk_fetch_pinned(SDL "${NK_SDL3_URL}" "${NK_SDL3_COMMIT}" FALSE NK_SDL3_DIR)
nk_fetch_pinned(SDL_ttf "${NK_SDL3_TTF_URL}" "${NK_SDL3_TTF_COMMIT}" TRUE NK_SDL3_TTF_DIR)
