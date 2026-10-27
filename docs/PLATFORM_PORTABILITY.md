# Platform portability

The runtime is currently a Windows application. Several subsystems already have portable seams,
but a successful object compile is not the same as a linked, running port.

## Current dependency map

| Area | Current state | Porting consequence |
| --- | --- | --- |
| Recompiler and guest memory | Mostly ISO C; guest arena is a 192 MiB host allocation | Compile on 64-bit targets, then validate address arithmetic, alignment, and memory budgets per device. |
| Thread coroutines | Win32 fibers on Windows; `ucontext` plus `mmap` on POSIX | Suitable for an initial glibc Linux port. Android and consoles need a supported platform backend behind `sr_coro`. |
| Window, input, and presentation | SDL3/Vulkan path exists, but public `gui_*` functions are inside Windows-only `gui.c`; GDI is the fallback | Move the SDL path into platform-neutral code and keep GDI in a Windows backend. |
| Audio | SDL3 audio stream | Portable in principle; validate device lifecycle, latency, and suspend/resume on each platform. |
| Video decode | The common project-authored PSMF producer owns stream demux and AU boundaries; Windows uses Media Foundation; elsewhere the pinned, vendored FFmpeg n4.4 H.264 decoder subset (`src/rt/h264_ffmpeg_sub/`, LGPL-2.1-or-later, generic C, statically linked, no system FFmpeg) is the default `sr_h264` backend, and `H264_FFMPEG=0` keeps the fail-closed null backend | Android needs a build of the same subset for its ABI; validate decode speed on target devices (the C-only DSP paths are slower than SIMD). |
| HLE host services | `hle.c` directly uses Win32 time, directory, file, sleep, and string APIs | Extract filesystem, clock/timezone, sleep, and process-exit services into a host-platform layer. |
| On-screen keyboard | Win32 dialog and a `wchar_t` interface | Change the seam to explicit UTF-16 code units and provide SDL/mobile/platform UI backends. |
| Savedata and ISO access | Already contain Windows/POSIX branches | Consolidate them into the same host filesystem layer and test path/case semantics. |
| Build and packaging | GNU Make plus PowerShell, Windows library names, SDK paths, and `.exe`/DLL packaging | Preserve the working Windows flow while introducing a cross-platform build definition with equivalent targets. |
| Diagnostic tooling | Some live process inspection is Windows-only | Keep it optional; ports need not block on debugger feature parity. |

## Staged plan

### 1. Keep Windows green and expose portable code

- Keep `nk_manager.ps1` and the current Makefile as the validated Windows path.
- Run the synthetic Windows runtime-object CI gate; it needs no proprietary inputs.
- Run the Linux host-neutral object gate. It deliberately excludes unresolved GUI/HLE platform
  work and must not be described as Linux support.

### 2. Linux desktop

1. Add a CMake target alongside the Makefile and prove object/source parity before switching the
   canonical build.
2. Split portable SDL3 GUI code from the GDI fallback.
3. Introduce host filesystem, time, sleep, and OSK interfaces; implement POSIX backends.
4. Validate the H.264 backend on Linux: the vendored FFmpeg decoder subset is the default off Windows
   (see the decision record below).
5. Link and run headless smoke tests, then visually validate SDL3/Vulkan and audio on Linux.

### 3. Android

Android and similar hosts are a planned target (maintainer decision, 2026-10-23). Every host needs a
C toolchain for the user's own game, because the project never publishes recompiled game code; a
user may build on their PC and move the result to their own device (`AGENTS.md`, section 5).

1. Drive the native target with the Android NDK's CMake toolchain and Gradle packaging.
2. Add an Android-compatible coroutine backend instead of assuming `ucontext` availability.
3. Implement app lifecycle, scoped-storage/content access, touch/controller input, audio focus,
   and suspend/resume behavior.
4. Select and validate a platform-appropriate video decoder.
5. Measure the guest arena and graphics memory footprint on representative 64-bit ARM devices.

### 4. Consoles

Console work is platform- and authorization-dependent. Keep the runtime interfaces narrow enough
to supply platform graphics, input, audio, storage, timing, and coroutine backends without
changing guest semantics. Vulkan cannot be assumed. Any port must follow the target platform's
SDK, distribution, and homebrew or licensed-development rules; do not place proprietary SDK
material in this repository.

## H.264 backend decision record

**Status: decided and built.** The maintainer chose the FFmpeg H.264 decoder subset on 2026-10-24
([#283](https://github.com/Jstar269/nakagawa-recomp/issues/283)), and it landed in
[#1263](https://github.com/Jstar269/nakagawa-recomp/pull/1263). The shared stream path uses the
project-authored PSMF producer for program-stream parsing and AU boundaries. Windows decodes with
Media Foundation and other hosts with the vendored subset. A build without a usable backend
(`H264_FFMPEG=0` off Windows) returns `H.264 backend unavailable; in the works (#283)` and does not produce a placeholder frame.

### Backend conformance contract

The source-authored fixture is the acceptance corpus. A backend must:

- deliver exactly three pictures for its three authored I_PCM access units, including the final
  access unit released by EOS, with no duplicate after a repeated drain;
- report the fixture's 64x64 dimensions and the delivered row stride, and distinguish its native
  format from the delivered host/guest format;
- preserve known normalized video timestamps in nondecreasing order (`0`, `3003`); an access unit
  whose PES has no PTS remains unknown and is extrapolated by the consumer, never fabricated by
  the backend;
- make reset equivalent to a fresh decoder instance, with no picture from the previous stream;
- reject malformed access-unit input without writing a frame.

For the current host RGBA conversion, the selected-pixel contract is exact for every fixture pixel:
`(255,203,138,255)`, `(130,130,130,255)`, and `(79,144,195,255)`, in that order. A backend with a
different conversion must document its selected pixels and tolerance before it is accepted; it
must not silently weaken this contract.

### Candidate dependencies

| Candidate | Licence position | Packaging and decision still required |
| --- | --- | --- |
| Cisco OpenH264 | BSD-2-Clause is generally compatible with GPL-3.0-or-later, subject to the exact binary's notices and any codec/patent terms. | Prefer a pinned shared library or source-built package per host. Verify redistribution terms, notice retention, architecture coverage, and decoder error behavior. **Awaiting maintainer decision.** |
| FFmpeg/libavcodec H.264 decoder | **Chosen** (maintainer decision, 2026-10-24, #283). A pinned subset of FFmpeg n4.4 (the pin of the ATRAC3+ import) is vendored under `src/rt/h264_ffmpeg_sub/`: LGPL-2.1-or-later, LGPL configuration only (no `--enable-gpl`, no `--enable-nonfree`), combined with the project's GPL-3.0-or-later terms. | Statically linked with generic C paths only; byte-exact manifest in `src/rt/h264_ffmpeg_sub/PROVENANCE.md`, notices in `NOTICE.md` and `CREDITS.md`. H.264 is patent-encumbered in some jurisdictions: the project ships no encoder and the decoder runs only on the user's own PSP movies. Windows keeps Media Foundation. |
| dav1d | Not a candidate: it decodes AV1, not H.264. | No H.264 backend may be represented by dav1d. |

`H264_FFMPEG` selects the FFmpeg backend: `1` builds the vendored subset and makes it the
decoder where no platform decoder exists, `0` keeps `h264_null.c` failing closed. The default is
`1` off Windows and `0` on Windows, where Media Foundation (`h264_mf.c`) stays the backend. The
build needs no FFmpeg package, headers or `pkg-config`. Setting `SR_NOH264` still disables the
decoder at run time.

### FFmpeg H.264 selftest

On Ubuntu or WSL already configured with the SDL3 and Vulkan development prerequisites in
`docs/SETUP.md`, with no FFmpeg package installed, run the same source-authored fixture used in
CI:

```bash
make CC=gcc H264_FFMPEG=1 BUILD_ROOT=build/ffmpeg-h264 psmf-media-selftest
```

The test explicitly selects the FFmpeg backend, decodes the synthetic I_PCM pictures through
the vendored decoder, checks the guest-visible pixels and final-AU/EOS behavior, and asserts
guest-VRAM prepare/commit order. A build with `H264_FFMPEG=0` keeps the null backend fail-closed.
The executable links no `libavcodec` shared library.

## Completed Milestones (ux-investigation)

1. **Native Core Layer (`src/core/`):**
   - Clean ISO9660 & PARAM.SFO parser (`nk_iso.h`/`nk_iso.c`) with 64-bit file operations.
   - User games library persistence in platform user-data directories (`nk_library.h`/`nk_library.c`).
   - Host platform abstraction with separate Windows (`nk_platform_win32.c`) and POSIX (`nk_platform_posix.c`) backends.
   - Typed launch session management (`nk_launch.h`/`nk_launch.c`).
   - Autogenerated static title catalog (`src/core/generated/nk_title_catalog.h`/`.c`) directly compiled from single-source-of-truth `assets/titles/*.json`.

2. **Cross-Platform CMake Build System (`CMakeLists.txt`):**
   - Targets `nakagawa_core` (STATIC), `test_core_catalog` (EXECUTABLE), and `nakagawa_player` (EXECUTABLE).
   - Validated on Windows 11 with MinGW GCC 16.1.0 (`-Wall -Wextra -Werror` zero warnings).
   - Validated on Ubuntu 24.04 via WSL (`gcc 13.3.0`, CTest 100% pass).

### Tracked title-catalog artifacts

The generated catalog outputs
`src/core/generated/nk_title_catalog.h` and `src/core/generated/nk_title_catalog.c`
are intentionally tracked. Make and CMake builds consume these reviewed source-tree
artifacts during ordinary builds, so the catalog generator is not required as a build
prerequisite. To regenerate the outputs, run `python tools/title_catalog_codegen.py`;
to verify that the tracked outputs exactly match the public title manifests, run
`python tools/title_catalog_codegen.py --verify`. The exact-output regression is
`TitleCatalogTests.test_live_catalog_verify_passes` in `tools/test_title_catalog.py`.
These tracked source artifacts do not include generated binaries or private title data.

## Verifying the POSIX backend before CI does

The local inner loop on the Windows development host is mingw32 only, but hosted CI compiles
`src/core/nk_platform_posix.c` on the Linux runner. A change to a platform-backend pair can
therefore pass every local gate and still fail hosted CI, and the failure arrives as a *compile*
error inside unrelated-looking Python suites rather than as a portability complaint.

Three modules compile the POSIX backend. They are the ones to run:

```bash
cd tools && python3 -m unittest test_native_host_backends test_iso_parity test_second_title_ingest
```

On this host WSL reproduces the runner closely (Ubuntu 24.04, gcc 13.3.0):

```bash
wsl.exe -e bash -lc 'cd "$(wslpath "<workspace-root>/worktrees/<worktree>")/tools" && python3 -m unittest test_native_host_backends test_iso_parity test_second_title_ingest'
```

The harness compiles with `gcc -std=c99 -Wall -Wextra -Werror`, so a missing declaration is a
build failure rather than a warning.

### Feature-test macros are the trap

`-std=c99` selects strict ISO C, so POSIX and XSI declarations appear only when the translation
unit asks for them. Getting this wrong is invisible on Windows, because the Win32 backend never
compiles the guarded code at all.

`realpath()` is the worked example: it is XSI rather than base POSIX, and glibc guards its
declaration on `__USE_MISC || __USE_XOPEN_EXTENDED`. `_POSIX_C_SOURCE 200809L` sets neither — it
sets `__USE_XOPEN2K8`, which that guard does not test. The declaration stays hidden, the call
compiles to an implicit `int`, and `-Werror` rejects every suite that builds the backend.

Prefer `_XOPEN_SOURCE 700`, which is the portable spelling and implies POSIX.1-2008. glibc's
`_DEFAULT_SOURCE` exposes the same declarations but does not carry to musl or the BSDs, and this
file is the POSIX backend for every non-Windows target.

## Definition of support

A platform is not supported until a clean build, synthetic tests, bounded runtime smoke test,
input, audio, persistent savedata, and visually inspected frames all pass on that platform.
