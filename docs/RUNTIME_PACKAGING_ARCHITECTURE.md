# Runtime Packaging Architecture

> **Status: CURRENT — maintained packaging decision record.** The process-isolation
> boundary and a local developer AOT package build are implemented. The player does
> not yet consume the generated package (#297). ISO unpacking remains #295; installers
> and complete end-user preparation are not built.

## 1. Context and Goals

Nakagawa Recomp is evolving from developer-centric tooling into an authentic cross-platform PSP recompilation platform.
The ultimate end-user experience target is:

~~~text
Download/install Nakagawa Recomp
  → Select legally owned PSP ISO
  → Instant recognition & authentic local preparation
  → Launch & Play
~~~

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

> **Implementation boundary:** the public source validates and launches a prepared
> runtime as a child process. It does not create that runtime from an arbitrary ISO;
> the packaging and preparation steps below remain target work.

The native player (`nakagawa_player`) acts as an authentic front-end and library manager. When a game is launched, it constructs a typed `NkLaunchSession` and spawns the title's standalone recompiled runtime as an isolated child process via `nk_platform_spawn_process`.
Communication and handoff occur via:

* Clean environment variables: `PSP_ISO`, `SR_FPS_CAP`, `SR_GPU_GE=1`, `SR_DEBUG`, `SR_DISPATCH_FATAL=1`.
* Process lifecycle tracking: native wait, status queries, and platform-specific termination (forced on Windows; SIGTERM with bounded grace and SIGKILL escalation on POSIX).

* **Advantages:**
  * **Total Fault Isolation:** A crash or abort in guest execution never brings down the launcher UI. The launcher detects child process exit, captures the exit code, and reports diagnostic facts cleanly to the user.
  * **Independent Optimization:** Each title binary is compiled with its exact manifest tuning (`RUNTIME_OPT`, `RECOMP_OPT`, custom span definitions) without polluting other titles.
  * **Sandboxing & OS Portability:** Native child processes follow standard OS process lifecycles on Windows, Linux (SteamOS/Steam Deck), and macOS.
  * **Boundary benefit:** Keeps the launcher and title runtime in separate processes;
    regression risk and title acceptance still require focused testing.

---

## 3. Concrete Implementation in `src/core/`

The adopted process boundary is implemented in the following public source slice; this
list is not evidence that a distributable installer or title package exists:

* `src/core/nk_types.h`: `NkGameEntry`, `NkResult`.
* `src/core/nk_platform.h`: `NkProcessHandle`, `nk_platform_spawn_process`, `nk_platform_is_process_running`, `nk_platform_wait_process`.
* `src/core/nk_platform_win32.c`: Win32 `CreateProcessW` with UTF-16 executable/command-line conversion, environment block generation, and wait handles.
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

## 5. Local AOT package contract (v1)

The planner can run the existing analyzer, code generator, and two-phase Make
pipeline for a plaintext executable ELF. Run it from the repository root with the
documented UCRT64 toolchain:

~~~powershell
$env:Path = "C:\msys64\ucrt64\bin;$env:Path"
python tools/title_codegen_plan.py assets/titles/my-title.json `
  --package `
  --game-elf place_game_here/EBOOT.elf `
  --output-dir build/my-title
~~~

When the manifest requires PSP-header BSS metadata, also pass
`--psp-header <path>`. Guest PRXs are resolved by their manifest names beneath
`--module-dir <directory>`; required PRXs are selected automatically. Add one
`--include-optional-module <manifest-name>` for each optional PRX to include in
the AOT build. `--game-name` defaults to the manifest's `game_name`, then to its
portable `id`. The manifest's `codegen_profile` is authoritative. `--public-safe`
selects the synthetic/public-safe runtime backends for fixture builds.

The output directory must be dedicated and untracked (use an ignored `build/`
subdirectory or a directory outside the repository). The command refuses a
tracked in-repository output and refuses to reuse an existing package directory
when its manifest, executable, selected PRXs, or PSP header hashes differ. The
manifest remains unchanged. Make's `all` target still runs the generation and
compile phases separately so generated chunks are linked on a clean build.
Current Make recipes cannot transport whitespace or shell-sensitive characters
in bound paths. Inputs at such paths are copied byte-for-byte into
`<output-dir>/staged-inputs/` (hashes are unchanged). When the output directory
contains spaces, packaging builds in a Make-safe workspace (`NK_BUILD_ROOT` or
Windows 8.3 short paths) and atomically promotes into the destination; otherwise
an unsupported path is rejected as `PACKAGE_UNSUPPORTED_PATH` (#296).

`package.json` is canonical JSON with this versioned shape:

| Field | Meaning |
| --- | --- |
| `format`, `schema_version` | `nakagawa-aot-package`, version `1`. |
| `title` | Manifest `id`, display name, kind, raw manifest SHA-256, and protected-semantics digest. |
| `inputs` | SHA-256 for the manifest, executable ELF, selected PRXs with load addresses, and optional PSP header. Absolute input paths are omitted. |
| `runtime` | `CpuState` ABI version and header hash, resolved guest run entry, runtime contract, bindings, and required bindings. |
| `executable` | Relative native executable path and hash plus the guest ELF entry address. |
| `generated_objects` | Sorted relative object paths and SHA-256 hashes. |
| `required_local_assets` | Manifest title-data/resource roots, selected or optional guest PRXs, and host SDL3/Vulkan runtime requirements. These are references; title assets are not copied into the package. |
| `build_report` | Relative path `build-report.json`. |

`build-report.json` has format `nakagawa-build-report`, schema version `1`, the
same `input_hashes`, Python/planner/analyzer/codegen/Make versions, the compiler
identity, runtime ABI, and deterministic coverage counts (`analyzed_functions`,
`aot_entries`, callable/resume entries, fallbacks, and unsupported imports,
instructions, and regions). `unsupported.imports`, `unsupported.instructions`,
and `unsupported.regions` are always arrays. Each listed boundary includes a
reason, status `in the works`, and its tracking issue: import coverage uses #71;
unsupported Allegrex translation and AOT function regions use #118. Empty arrays
mean the analyzer/code generator did not report that boundary for these inputs;
they do not establish title acceptance. These records do not patch behavior:
generated unsupported functions retain the runtime's fail-closed path.

This package command still requires the developer toolchain. It does not unwrap
an ISO or make the player launch the generated package. The player-side consumer
and run-directory provisioning are tracked by #297; analyzer ownership and
stack-balance gates remain tracked by #291.
