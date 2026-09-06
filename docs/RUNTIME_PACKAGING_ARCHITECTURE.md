# Runtime Packaging Architecture

## 1. Context and Goals

Nakagawa Recomp is evolving from developer-centric tooling into an authentic cross-platform PSP recompilation platform.
The ultimate end-user experience target is:

```
Download/install Nakagawa Recomp
  → Select legally owned PSP ISO
  → Instant recognition & authentic local preparation
  → Launch & Play
```

A crucial design decision is how the native player (`nakagawa_player`) and the game execution runtime relate to each other at packaging and execution time.

---

## 2. Evaluation of Architectural Options

### Option A: Monolithic Multi-Title Executable
A single executable contains the UI launcher, the generic PSP runtime, the AOT recompilation chunks for all supported titles, and all HLE/LLE subsystems.

* **Advantages:**
  * Single binary distribution (`nakagawa.exe` / `nakagawa`).
  * No IPC or child process management.
* **Disadvantages:**
  * **Violates title isolation:** Different PSP titles require distinct code chunks, compiler optimization levels (e.g., HST uses `-O1` for massive dynamic recomp chunks to preserve compile time and `-O2` for runtime), and different executable span allocations.
  * **Binary bloat:** Recompiled C for a large PSP game generates tens of megabytes of native machine code. Bundling 10 games into one executable would result in gigabyte-scale binaries.
  * **Crash vulnerability:** A crash in a guest module immediately terminates the entire launcher application, destroying UI state and unsaved player configuration.
  * **LLE/Fidelity violation:** Guest memory arena mappings (e.g. 192 MiB fixed address reservations) would conflict if multiple titles or sessions were managed in the same address space.

### Option B: Core Engine + Dynamic Title Plug-in (`.dll` / `.so`)
The native player loads a generic runtime engine, which dynamically loads a title-specific shared library (`hst_recomp.dll`, `phase5_recomp.so`) providing the entry points and chunk dispatch tables.

* **Advantages:**
  * Clean modular separation between front-end UI, generic runtime, and title code.
  * Extensible without rebuilding the launcher.
* **Disadvantages:**
  * **Fragile ABI boundary:** Exposing C function pointer tables between host modules across different compilers (e.g. MinGW vs MSVC vs Clang) easily creates ABI mismatches.
  * **Shared address space risks:** Like Option A, guest memory arena collisions and unhandled guest exceptions still jeopardize host launcher stability.

### Option C: Isolated Process per Title + Native Host Launcher (Adopted Architecture)
The native player (`nakagawa_player`) acts as an authentic front-end and library manager. When a game is launched, it constructs a typed `NkLaunchSession` and spawns the title's standalone recompiled runtime as an isolated child process via `nk_platform_spawn_process`.
Communication and handoff occur via:
* Clean environment variables: `PSP_ISO`, `SR_FPS_CAP`, `SR_GPU_GE=1`, `SR_DEBUG`, `SR_DISPATCH_FATAL=1`.
* Process lifecycle tracking: native wait, status queries, and graceful termination.

* **Advantages:**
  * **Total Fault Isolation:** A crash or abort in guest execution never brings down the launcher UI. The launcher detects child process exit, captures the exit code, and reports diagnostic facts cleanly to the user.
  * **Independent Optimization:** Each title binary is compiled with its exact manifest tuning (`RUNTIME_OPT`, `RECOMP_OPT`, custom span definitions) without polluting other titles.
  * **Sandboxing & OS Portability:** Native child processes follow standard OS process lifecycles on Windows, Linux (SteamOS/Steam Deck), and macOS.
  * **Zero Regression Risk:** Preserves 100% of the battle-tested runtime and graphics behavior of Hot Shots Tennis while adding multi-title scalability.

---

## 3. Concrete Implementation in `src/core/`

The adopted architecture is implemented in:
* `src/core/nk_types.h`: `NkGameEntry`, `NkResult`.
* `src/core/nk_platform.h`: `NkProcessHandle`, `nk_platform_spawn_process`, `nk_platform_is_process_running`, `nk_platform_wait_process`.
* `src/core/nk_platform_win32.c`: Win32 `CreateProcessA` with environment block generation and wait handles.
* `src/core/nk_platform_posix.c`: POSIX `fork` + `execv` with environment configuration.
* `src/core/nk_launch.h` & `src/core/nk_launch.c`: typed `NkLaunchSession` candidate resolution, environment construction, and process lifecycle management.
* `src/player/ui_renderer.c`: UI "PLAY NOW" / "STOP GAME" action triggers and active PID indicators.

---

## 4. Packaging and Distribution Roadmap

| Target Environment | Packaging Strategy | Binary Artifacts |
| --- | --- | --- |
| **Windows x86-64** | Inno Setup / MSIX / Portable ZIP | `nakagawa_player.exe`, `bin/nakagawa_core.dll` (or static), `build/<game>/<game>.exe`, SDL3.dll |
| **Linux / Steam Deck** | AppImage / Flatpak | `nakagawa_player`, `bin/<game>`, `libSDL3.so` bundled in AppDir |
| **macOS ARM64** | App Bundle (`Nakagawa.app`) | `Nakagawa.app/Contents/MacOS/nakagawa_player`, helper executables in `MacOS/` |
