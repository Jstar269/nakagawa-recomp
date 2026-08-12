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
| Video decode | Windows Media Foundation; non-Windows builds use a timing-preserving null backend | Add a real non-Windows decoder behind `sr_h264` before movie playback can work. |
| HLE host services | `hle.c` directly uses Win32 time, directory, file, sleep, and string APIs | Extract filesystem, clock/timezone, sleep, and process-exit services into a host-platform layer. |
| On-screen keyboard | Win32 dialog and a `wchar_t` interface | Change the seam to explicit UTF-16 code units and provide SDL/mobile/platform UI backends. |
| Savedata and ISO access | Already contain Windows/POSIX branches | Consolidate them into the same host filesystem layer and test path/case semantics. |
| Build and packaging | GNU Make plus PowerShell, Windows library names, SDK paths, and `.exe`/DLL packaging | Preserve the working Windows flow while introducing a cross-platform build definition with equivalent targets. |
| Diagnostic tooling | Some live process inspection is Windows-only | Keep it optional; ports need not block on debugger feature parity. |

## Staged plan

### 1. Keep Windows green and expose portable code

- Keep `hst_manager.ps1` and the current Makefile as the validated Windows path.
- Run the synthetic Windows runtime-object CI gate; it needs no proprietary inputs.
- Run the Linux host-neutral object gate. It deliberately excludes unresolved GUI/HLE platform
  work and must not be described as Linux support.

### 2. Linux desktop

1. Add a CMake target alongside the Makefile and prove object/source parity before switching the
   canonical build.
2. Split portable SDL3 GUI code from the GDI fallback.
3. Introduce host filesystem, time, sleep, and OSK interfaces; implement POSIX backends.
4. Add a real H.264 backend after choosing its license and redistribution model.
5. Link and run headless smoke tests, then visually validate SDL3/Vulkan and audio on Linux.

### 3. Android

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

## Definition of support

A platform is not supported until a clean build, synthetic tests, bounded runtime smoke test,
input, audio, persistent savedata, and visually inspected frames all pass on that platform.
