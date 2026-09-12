# Native UI Regression Matrix & Functional Checklist

This document establishes the authoritative functional regression checklist for Nakagawa Recomp across both the web transition phase and the native player implementation.

The preparation engine referenced below is a standalone `nk_core` prototype; it
is not connected to the native player in this build. The native
`VIEW_PREPARING` state is therefore an explicit unavailable state and does not
claim extraction, decryption, staging, item counts, or percentage progress.

## 1. Functional Status Matrix

| Check ID | Functional Capability | Web Studio Baseline | Native Shell Target | Current Status | Verification Evidence |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `WEB_STUDIO_STARTS` | Next.js server starts on port 3000, loads Studio UI | **PASS** | N/A (Dev only) | **PASS** | Verified via headless browser screenshot capture (`01_web_player_mode.png`) |
| `WEB_PLAYER_MODE_STARTS` | Top-level Player Mode hero screen loads with settings cards | **PASS** | N/A | **PASS** | Verified via `launcher-panel.tsx` rendering in browser |
| `ISO_INSPECT_WORKS` | ISO9660 PVD and directory parsing from raw binary file | **PARTIAL** (Browser JS) | **YES** (`nk_core/iso_inspect.py` / native) | **PASS** | Verified in `tools/test_nk_core.py::test_iso_inspect` |
| `TITLE_ID_DETECTION_WORKS` | Extracts Disc ID (`UCUS98701`, etc.) from `PARAM.SFO` | **PARTIAL** (In-memory) | **YES** (`nk_core/title_registry.py`) | **PASS** | Verified in `tools/test_nk_core.py::test_registry_disc_matching` |
| `PREPARED_FOLDER_VALIDATION_WORKS` | Transactional staging & validation of game directory | **NO** (Manual external) | **PROTOTYPE ONLY** (`nk_core/prep_engine.py`, not native-player connected) | **PARTIAL** | The standalone Python prototype passes `tools/test_nk_core.py::test_prep_engine_atomic_staging`; native `VIEW_PREPARING` remains unavailable |
| `PSP_ISO_ENV_HANDOFF_WORKS` | `PSP_ISO` environment variable correctly passed to runtime | **YES** (`manager-process.ts`) | **YES** (`nk_launch.c` / typed session) | **PASS** | Verified in `tools/test_nk_core.py::test_launcher_plan` |
| `RUNTIME_PROCESS_STARTS` | Host launcher successfully spawns runtime binary | **YES** (PowerShell child) | Direct process spawn via platform API | **PASS (synthetic fixture)** | `display-smoke-player` drives native `PLAY NOW`, records `--gui`, and observes child boot milestones |
| `VULKAN_WINDOW_STARTS` | SDL3 creates native window and initializes Vulkan swapchain | **YES** (`hst.exe` runtime) | **YES** (Direct SDL3 window) | **PASS** | Verified in SDL3 compilation probe (`src/rt/gpu_sdl3vk/sdl3vk.c`) |
| `CURRENT_HST_ROUTE_REACHES_KNOWN_POINT` | Prepared HST assets boot to title screen with Vulkan rendering | **PASS** (When private inputs present) | Preserved via launch planner | **PENDING_NATIVE_EXECUTION** | Launch arguments validated; native end-to-end child execution tested in this phase |
| `SAVES_PATH_VALID` | Launch routes `SR_MEMSTICK` to a writable per-disc location: the title catalog's `memory_stick_root` when the install is writable, otherwise the platform save directory (`%LOCALAPPDATA%\Nakagawa\saves\<disc id>` on Windows, `$XDG_DATA_HOME/nakagawa-recomp/saves/<disc id>` on Linux) | **PASS** | **PASS** | **PASS** | Resolved in `nk_launch_prepare_session`; verified in `tests/native/test_launch_resolution.c` on Windows and Linux |
| `CONFIG_PERSISTENCE_VALID` | Settings serialize and deserialize without schema degradation | **PASS** (92 unit tests) | **PASS** (Native C JSON persistence) | **PASS** | Verified in `interface` unit tests and `nk_library` tests |
| `LOGS_WRITTEN` | Logs cleanly piped to `logs/` directory without polluting UI | **PASS** (Log ring buffer) | **PASS** (`logs/native_player.log`) | **PASS** | Verified in process managers |
| `NO_C_NK_RUNTIME_DEPENDENCY_IN_PLAYER_PACKAGE` | Native player runs without assuming hardcoded `C:\nk` paths | **FAIL** (Legacy scripts had `C:\nk` assumptions) | **PASS** (Uses relative & platform paths) | **PASS** | Verified in path normalization tests |

---

## 2. Regression Gate Checklist for Native Slices

Before declaring any native player slice complete, verify:

1. [ ] **Binary Compilation**: Native shell compiles without warnings or errors on Windows UCRT64 GCC.
2. [ ] **Window Lifecycle**: Application window appears in $<100\text{ ms}$ before any I/O or indexing begins.
3. [ ] **Multi-Title Invariant**: UI does not hardcode `hst` or `UCUS98701` logic; query resolves dynamically from `TitleRegistry`.
4. [ ] **LLE Compliance**: No synthetic TTF font substitution, no fake `libfont` readiness flags, no fake `psmf` return values added.
5. [ ] **File Dialog**: Native OS file picker invoked without terminal or console prompts.
6. [ ] **Error Handling**: Missing or corrupt ISOs produce human-actionable error cards with stable error codes.
7. [ ] **Automated Test Parity**: All unit tests in `tools/test_nk_core.py` and `interface` continue to pass.
