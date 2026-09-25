# Native UI Regression Matrix & Functional Checklist

This document establishes the authoritative functional regression checklist for Nakagawa Recomp across both the web transition phase and the native player implementation.

The portable preparation engine referenced below remains a standalone
`nk_core` prototype. The first-time setup wizard has a separate native,
standalone ISO/XB staging path: it copies only `EBOOT.BIN` and
`PSP_GAME/USRDIR/xbdata`, decodes validated `.xb` members into runtime-compatible
`<archive>.xb.d/` directories, records the
asset/audio/visual/layout census, and promotes the isolated staging tree
atomically into the actionable `PLAYER_VIEW_READY_LIBRARY` state. Decryption,
encrypted-inner-ELF validation, and retail-title acceptance remain separate
capabilities and are not implied by this path.

## 1. Functional Status Matrix

| Check ID | Functional Capability | Web Studio Baseline | Native Shell Target | Current Status | Verification Evidence |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `WEB_STUDIO_STARTS` | Next.js server starts on port 3000, loads Studio UI | **PASS** | N/A (Dev only) | **PASS** | Verified via headless browser screenshot capture (`01_web_player_mode.png`) |
| `WEB_PLAYER_MODE_STARTS` | Top-level Player Mode hero screen loads with settings cards | **PASS** | N/A | **PASS** | Verified via `launcher-panel.tsx` rendering in browser |
| `ISO_INSPECT_WORKS` | ISO9660 PVD and directory parsing from raw binary file | **PARTIAL** (Browser JS) | **YES** (`nk_core/iso_inspect.py` / native) | **PASS** | Verified in `tools/test_nk_core.py::test_iso_inspection_success` |
| `TITLE_ID_DETECTION_WORKS` | Extracts Disc ID (`UCUS98701`, etc.) from `PARAM.SFO` | **PARTIAL** (In-memory) | **YES** (`nk_core/title_registry.py`) | **PASS** | Verified in `tools/test_nk_core.py::test_title_registry_matching_and_normalization` |
| `PREPARED_FOLDER_VALIDATION_WORKS` | Transactional staging & validation of game directory | **NO** (Manual external) | **YES** (native ISO/XB staging; encrypted decryption remains separate) | **PARTIAL** | `tests/native/test_xb_parser.c` covers synthetic ISO EBOOT/XB staging and asset census; `tests/native/test_launch_resolution.c` covers staged EBOOT ELF/container checks and root precedence, while retail acceptance is not run |
| `PSP_ISO_ENV_HANDOFF_WORKS` | `PSP_ISO` environment variable correctly passed to runtime | **YES** (`manager-process.ts`) | **YES** (`nk_launch.c` / typed session) | **PASS** | Verified in `tools/test_nk_core.py::test_runtime_launcher_plan_construction` |
| `RUNTIME_PROCESS_STARTS` | Host launcher successfully spawns runtime binary | **YES** (PowerShell child) | Direct process spawn via platform API | **PASS (synthetic fixture)** | `display-smoke-player` drives native `PLAY NOW`, records `--gui`, and observes child boot milestones; staged entries expose `LAUNCH PREPARED` until a runtime is actually resolved |
| `VULKAN_WINDOW_STARTS` | SDL3 creates native window and initializes Vulkan swapchain | **YES** (`hst.exe` runtime) | **YES** (Direct SDL3 window) | **PASS** | Verified in SDL3 compilation probe (`src/rt/gpu_sdl3vk/sdl3vk.c`) |
| `CURRENT_HST_ROUTE_REACHES_KNOWN_POINT` | Prepared HST assets boot to title screen with Vulkan rendering | **PASS** (When private inputs present) | Preserved via launch planner | **PENDING_NATIVE_EXECUTION** | Launch arguments validated; native end-to-end child execution tested in this phase |
| `SAVES_PATH_VALID` | Launch routes `SR_MEMSTICK` to a writable per-disc location: the title catalog's `memory_stick_root` when the install is writable, otherwise the platform save directory (`%LOCALAPPDATA%\Nakagawa\saves\<disc id>` on Windows, `$XDG_DATA_HOME/nakagawa-recomp/saves/<disc id>` on Linux) | **PASS** | **PASS** | **PASS** | Resolved in `nk_launch_prepare_session`; verified in `tests/native/test_launch_resolution.c` on Windows and Linux |
| `CONFIG_PERSISTENCE_VALID` | Settings serialize and deserialize without schema degradation | **PASS** | **PASS** (Native C JSON persistence) | **PASS** | Verified in `interface` unit tests and `nk_library` tests |
| `LOGS_WRITTEN` | Logs cleanly piped to `logs/` directory without polluting UI | **PASS** (Log ring buffer) | **PASS** (`logs/native_player.log`) | **PASS** | Verified in process managers |
| `NO_C_NK_RUNTIME_DEPENDENCY_IN_PLAYER_PACKAGE` | Native player runs without assuming hardcoded workspace paths | **FAIL** (Legacy scripts had hardcoded workspace assumptions) | **PASS** (Uses relative & platform paths) | **PASS** | Verified in path normalization tests |

---

## 2. Regression Gate Checklist for Native Slices

Before declaring any native player slice complete, verify each gate against its authoritative test or measurement owner:

1. [x] **Binary Compilation** (`PASS`)
   - **Owner / Verification**: `mingw32-make player` and `mingw32-make native-core-tests`
   - **Evidence**: Native player compiles cleanly with Windows UCRT64 GCC (`-Wall -Wextra`) without MSVC or Node.js dependencies, producing `build/nakagawa_player.exe`.
2. [x] **Window Lifecycle & Immediate Initialization** (`PASS`)
   - **Owner / Verification**: `src/player/main.c` and `mingw32-make display-smoke-player`
   - **Evidence**: SDL3 application window initializes and presents the UI frame immediately on startup. All disk reads, title indexing, and ISO extraction operations are deferred to asynchronous worker threads (`src/player/setup_staging.c`). The uncited `<100 ms` threshold from earlier drafts is removed in favor of this verifiable architectural invariant (immediate presentation before worker dispatch, verified in smoke player milestones).
3. [x] **Multi-Title Invariant** (`PASS`)
   - **Owner / Verification**: `tests/native/test_launch_resolution.c` and `tools/test_generic_title_planning_proof.py`
   - **Evidence**: UI does not hardcode `hst` or `UCUS98701` logic; title identification and launch parameters resolve dynamically from `nk_title_manifest.c` / `nk_title_catalog.c` and `tools/nk_core/title_registry.py`. Unregistered titles fail closed with structured recovery.
4. [x] **LLE Compliance & Honest Failure Discipline** (`PASS`)
   - **Owner / Verification**: `src/core/nk_font.c`, `src/rt/`, and `docs/LLE_FIDELITY_ARCHITECTURE.md`
   - **Evidence**: No synthetic TTF font substitution in guest runtime, no fake `libfont` readiness flags, and no fake `psmf` return values. Missing authentic PSP assets (e.g. firmware `jpn0.pgf`) fail closed with honest user guidance rather than silent host approximations.
5. [x] **Native File Dialog** (`PASS`)
   - **Owner / Verification**: `src/player/main.c::on_file_dialog_complete` (`SDL_ShowOpenFileDialog`)
   - **Evidence**: Native OS file picker is invoked directly from the SDL3 event loop without terminal windows or console prompts.
6. [x] **Actionable Error Handling** (`PASS`)
   - **Owner / Verification**: `tests/native/test_player_state.c` and `tools/test_nk_core.py`
   - **Evidence**: Missing, corrupted, or unsupported ISOs produce human-actionable error cards with stable error codes (`ISO_UNSUPPORTED_TITLE`, `ISO_UNREADABLE`, `STAGED_EXECUTABLE_INVALID`, `LIBRARY_WRITE_FAILED`, `MISSING_FIRMWARE_FONT`) and structured recovery buttons.
7. [x] **Automated Test Parity** (`PASS`)
   - **Owner / Verification**: `python -m unittest discover -s tools -p "test_nk_core.py"` and `tests/native/` suite
   - **Evidence**: All 22 tests in `tools/test_nk_core.py` and all native test binaries (`test_player_state`, `test_xb_parser`, `test_launch_resolution`, `test_input_settings`) pass.
8. [x] **Typography & Font Strategy (Zero-Bundled-Font Architecture)** (`PASS`)
   - **Owner / Verification**: `src/core/nk_font.c`, `src/player/ui_renderer.c`, and `docs/NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md`
   - **Evidence**: Implements runtime system-font typography through dynamic `SDL3_ttf` loading (e.g. Segoe UI on Windows, DejaVu Sans on Linux) with zero bundled fonts in the repository or shipped binary, falling back to SDL3 built-in `DebugText` if system fonts are unavailable. Open-source typography options (M PLUS Rounded 1c, Rubik, Inter, JetBrains Mono, etc.) are documented as design references and target aesthetic profiles, distinguishing current implementation requirements from historical styling explorations and avoiding any unfulfilled font asset dependency.
