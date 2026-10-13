# Linux development

Status: bounded headless runtime route verified; broader Linux support is in progress.

This guide covers developing Nakagawa Recomp on Linux, either natively or under
WSL 2. The runtime builds and runs source-owned showcase fixtures headlessly;
general consumer-title support and interactive presentation remain in progress.
See [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) for the staged port plan.
The supported desktop baseline remains Windows 11 x64 (see [`SETUP.md`](SETUP.md)).

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

- `build-essential`, `pkg-config`, `cmake`, `ninja-build`, `git`, `curl`
- `gcc-mipsel-linux-gnu` (AOT test targets that emit MIPS objects)
- `python3.14`, `python3.14-venv`, `python3.14-dev`. CPython 3.14.x is
  required, and Ubuntu 24.04's own archive does not carry it: enable the
  deadsnakes PPA first (`sudo add-apt-repository ppa:deadsnakes/ppa`). Create a
  project environment (for example `~/.venvs/nakagawa`), and keep `ruff`,
  `pre-commit` and the hash-locked tools in it.
- `libvulkan-dev`, `vulkan-tools`, `glslc`, `spirv-tools` (Vulkan headers and shader tools)
- SDL3 3.4.16. When the distribution does not package SDL3, build the pinned
  3.4.16 source revision into `/usr/local`:

SDL3 from source (when `libsdl3-dev` is unavailable):

```bash
git init SDL
git -C SDL remote add origin https://github.com/libsdl-org/SDL.git
git -C SDL fetch --depth 1 origin fa2c02bb6e21974a89ea9824bc53c9932abe5f9c
git -C SDL checkout --detach FETCH_HEAD
cmake -S SDL -B SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_VULKAN=ON -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build SDL/build
sudo cmake --install SDL/build
sudo ldconfig
pkg-config --modversion sdl3   # should print 3.4.16
```

Install PSPDEV v20260601 at `/usr/local/pspdev` (the official
`pspdev-ubuntu-latest-x86_64.tar.gz` release asset); CI downloads its archive from
`assets/upstream/pspdev.evidence.json` and verifies the digest recorded in
`assets/upstream/pspdev.lock.json`.

## What works today

Verified on Ubuntu 24.04 with gcc 13.3.0, SDL3 3.4.16, Vulkan 1.3.275, CMake
3.28, Ninja, glslc, and Python 3.14.6.

Linux `ms0:` file opens, reads, writes, renames, directory creation, and removal
use the descriptor-relative containment seam. Absolute paths, traversal, symlinks,
and overlong components fail with a PSP illegal-path result; a symlink inside the
Memory Stick root is not followed.

### CMake build (with the player)

```bash
cmake -S . -B build/cmake-linux -DBUILD_PLAYER=ON -DCMAKE_C_COMPILER=gcc
cmake --build build/cmake-linux -j4
ctest --test-dir build/cmake-linux --output-on-failure
```

The player binary builds, and its CLI help works without opening a display:

```bash
SDL_VIDEODRIVER=offscreen ./build/cmake-linux/nakagawa_player --help
# Usage: nakagawa_player [--user-data-root=<folder>] [--iso=<path> [--stage|--stage-only|--launch-now]] ...
```

The bounded headless staging smoke uses the source-owned TEST00007 showcase ISO. It needs
PSPDEV because the showcase target first compiles its synthetic guest:

```bash
scratch=$(mktemp -d)
NK_SHOWCASE_BUILD_ROOT="$scratch/showcase-build" \
NK_SHOWCASE_DEMO_ROOT="$scratch/showcase-demo" make CC=gcc showcase-linux
XDG_DATA_HOME="$scratch/player-smoke-data" timeout 60s \
  build/cmake-linux/nakagawa_player \
  --iso="$scratch/showcase-build/showcase-scene-v1/TEST00007.iso" --stage-only
# Expect: STAGING_RESULT status=PASS
```

### Makefile

```bash
# Source-owned Linux runtime showcase; dummy video/audio, PSPDEV required.
make CC=gcc showcase-linux

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

`psmf-media-selftest` passes on Linux, but it is a local gate: no hosted CI job runs it, so a
green hosted run does not cover it. Run it locally with the command above.

### Python test suites

```bash
PYTHONPATH=tools python -m unittest tools.test_vulkan_sdk
PYTHONPATH=tools python -m unittest tools.test_build_system_parity
python tools/lint_docs.py
```

## What doesn't yet

| Gate | Status | Issue |
| --- | --- | --- |
| Consumer ISO compatibility | The automated Linux CMake/player and runtime gates use only the source-owned TEST00007/TEST00008 showcase fixtures. Compatibility across consumer ISOs and title-specific semantics remains in the works. | #306 |
| `hle-thread-selftest` and `hle-title-selftest` | These production-HLE harnesses still use Win32 test setup and are not built on Linux. The Linux showcase exercises the real runtime but does not replace those broader HLE suites, which remain in the works. | #306 |
| Interactive SDL3/Vulkan presentation | The showcase smoke uses dummy video/audio drivers and checks headless output. Interactive desktop presentation and a broader Linux device/display matrix remain in the works. | #306 |
| Python suites that call `git` | `tools/test_lint_docs.py`, `tools/test_publication_policy_gate.py`, and `tools/publish_audit.py` use `git` on the checkout. They work in an ordinary Linux clone; they fail in a Windows checkout opened from WSL, because its worktree paths are Windows paths. Never set `GIT_DIR` to work around it. | #306 |
| Full `python -m unittest discover -s tools` | Takes several minutes; CI splits it into shards. Run the focused suites above while developing. | #306 |

## See also

- [`SETUP.md`](SETUP.md) — Windows development baseline.
- [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — portability plan and the
  host-neutral object gate.
- [`CI.md`](CI.md) — hosted CI routes (the Linux native job builds the CMake player,
  runs CTest and the showcase headless gates, and runs `make CC=gcc native-core-tests`).

<!-- issue-links: #306 -->
