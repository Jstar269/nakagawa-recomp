# Linux development

Status: in progress.

This guide covers developing Nakagawa Recomp on Linux, either natively or under
WSL 2. It is a portability probe, not a statement that the runtime is
Linux-supported: see [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) for the
staged port plan and the current host-neutral object gate. The supported and
tested core build remains Windows 11 x64 (see [`SETUP.md`](SETUP.md)).

## Supported setup

- Ubuntu 24.04, native or WSL 2. Under WSL 2, clone the repository inside the
  Linux filesystem (for example `~/src/nakagawa-recomp`), not under `/mnt/c`:
  builds are much faster there, and `git` works normally.
- If you instead open a checkout that was created on Windows from WSL, its
  worktree `.git` file holds a Windows path that Linux `git` cannot resolve.
  Run `git` for that checkout from Windows, and use WSL only to compile and run
  tests.

  **Never set or export `GIT_DIR`, `GIT_WORK_TREE`, or `GIT_COMMON_DIR` to work
  around this.** Tests that create scratch repositories then write into the
  repository those variables name. That has corrupted a shared repository
  config before.

## Prerequisites

Install with `sudo apt update && sudo apt install`:

- `build-essential`, `pkg-config`, `cmake`, `ninja-build`, `git`
- `gcc-mipsel-linux-gnu` (AOT test targets that emit MIPS objects)
- `python3.14`, `python3.14-venv`, `python3.14-dev`. CPython 3.14.x is
  required, and Ubuntu 24.04's own archive does not carry it: enable the
  deadsnakes PPA first (`sudo add-apt-repository ppa:deadsnakes/ppa`). Create a
  project environment (for example `~/.venvs/nakagawa`), and keep `ruff`,
  `pre-commit` and the hash-locked tools in it.
- `libvulkan-dev`, `vulkan-tools`, `glslc`, `spirv-tools` (Vulkan headers and shader tools)
- `libsdl3-dev` (SDL3 3.4.x; if your distribution does not package it, build
  SDL3 from source into `/usr/local`)

SDL3 from source (when `libsdl3-dev` is unavailable):

```bash
git clone --depth 1 --branch release-3.4.8 https://github.com/libsdl-org/SDL.git
cmake -S SDL -B SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build SDL/build
sudo cmake --install SDL/build
sudo ldconfig
pkg-config --modversion sdl3   # should print 3.4.8
```

## What works today

Verified on Ubuntu 24.04 with gcc 13.3.0, SDL3 3.4.8, Vulkan 1.3.275, CMake
3.28, Ninja, glslc, and Python 3.14.6.

### CMake build (with the player)

```bash
cmake -S . -B build/cmake-linux -DBUILD_PLAYER=ON -DCMAKE_C_COMPILER=gcc
cmake --build build/cmake-linux -j4
(cd build/cmake-linux && ctest --output-on-failure)   # 9/9 pass
```

The player binary builds and starts:

```bash
SDL_VIDEODRIVER=offscreen ./build/cmake-linux/nakagawa_player --help
# Usage: nakagawa_player [--iso=<path>] [--stage|--stage-only] ...
```

### Makefile

```bash
# Core object build (no SDL3/Vulkan needed)
make CC=gcc GAME_NAME=ci BUILD_DIR=build/portable-check portable-core-objects

# Native selftests
make CC=gcc GAME_NAME=ci BUILD_DIR=build/nt-check native-core-tests

# Makefile selftests that pass on Linux
make CC=gcc GAME_NAME=ci BUILD_DIR=build/slt-check \
  sched-selftest vfpu-tables-selftest vfpu-interp-selftest heap-selftest \
  dispatch-isolation-selftest watchpoints-file-selftest profiler-selftest \
  fp-convert-selftest stale-code-selftest strbuf-selftest coro-selftest \
  cpu-lle-selftest domain-mode-selftest psmf-producer-selftest \
  psmf-media-selftest atrac3p-selftest atrac3p-bridge-selftest cosim-selftest
```

### Python test suites

```bash
PYTHONPATH=tools python -m unittest tools.test_vulkan_sdk
PYTHONPATH=tools python -m unittest tools.test_build_system_parity
python tools/lint_docs.py
```

## What doesn't yet

| Gate | Status | Issue |
| --- | --- | --- |
| `runtime-objects` (the full runtime) | Fails at `src/rt/hle.c`, which includes Windows-only headers (`process.h`, `io.h`, `windows.h`). The portable subset builds with `portable-core-objects`. | #306 |
| `hle-thread-selftest` and `hle-title-selftest` | Not built on Linux: `src/rt/hle_thread_selftest.c` and `src/rt/hle.c` call Win32 host APIs (`windows.h`, `process.h`, `_open_osfhandle`, `CreateDirectoryA`, `DeleteFileA`, `GetCurrentDirectoryA`) that have no Linux equivalent. These targets are Windows-only by design; a portable file/IO seam is the next step. | #306 |
| `make player` (native graphics) | Builds and starts, but the presenter is the SDL3+Vulkan window path (`src/rt/gpu_sdl3vk/sdl3vk.c`), which needs a working Vulkan driver. `SDL_VIDEODRIVER=offscreen` runs the startup path headlessly; a bounded headless run of a real title is not yet wired. | #306 |
| `src/rt/hle.c` host I/O | `ms0_fopen_utf8` uses `_open_osfhandle`/`_fdopen`/`_O_BINARY` to wrap a Win32 `HANDLE` as a `FILE*`. On Linux the contained-file open needs a POSIX seam. | #306 |
| Python suites that call `git` | `tools/test_lint_docs.py`, `tools/test_publication_policy_gate.py`, and `tools/publish_audit.py` use `git` on the checkout. They work in an ordinary Linux clone; they fail in a Windows checkout opened from WSL, because its worktree paths are Windows paths. Never set `GIT_DIR` to work around it. | #306 |
| Full `python -m unittest discover -s tools` | Takes several minutes; CI splits it into shards. Run the focused suites above while developing. | #306 |

## See also

- [`SETUP.md`](SETUP.md) — Windows development baseline.
- [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — portability plan and the
  host-neutral object gate.
- [`CI.md`](CI.md) — hosted CI routes (the Linux native test job uses
  `make CC=gcc native-core-tests`).

<!-- issue-links: #306 -->
