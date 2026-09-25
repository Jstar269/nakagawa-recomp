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
| `RUNTIME_PROCESS_STARTS` | Host launcher successfully spawns runtime binary | **YES** (PowerShell child) | Direct process spawn via platform API | **PASS (synthetic fixture)** | `display-smoke-player` drives native `PLAY NOW`, records `--gui`, and observes child boot milestones; staged entries expose `BUILD PACKAGE` for a missing package and a `RUNTIME REQUIRED` status when no runtime resolves, until a package or developer runtime is actually resolved ([#483](https://github.com/Jstar269/nakagawa-recomp/pull/483)) |
| `VULKAN_WINDOW_STARTS` | SDL3 creates native window and initializes Vulkan swapchain | **YES** (`hst.exe` runtime) | **YES** (Direct SDL3 window) | **PASS** | Verified in SDL3 compilation probe (`src/rt/gpu_sdl3vk/sdl3vk.c`) |
| `CURRENT_HST_ROUTE_REACHES_KNOWN_POINT` | Prepared HST assets boot to title screen with Vulkan rendering | **PASS** (When private inputs present) | Owned disc added, packaged and launched from the native player | **PASS (maintainer-local evidence)** | The owned disc was added in the player, built public-safe with `nk_cli build-package` (no private backend overlay) and launched with `--launch-index`. The runtime's per-second `SR_PERF_CSV` shows sustained presentation, and its own `SR_FBSNAP` framebuffer snapshots show the title menu rendered ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487), [#358](https://github.com/Jstar269/nakagawa-recomp/issues/358)). Private inputs are required, so CI cannot repeat this check |
| `SAVES_PATH_VALID` | Launch routes `SR_MEMSTICK` to a writable per-disc location: the title catalog's `memory_stick_root` when the install is writable, otherwise the platform save directory (`%LOCALAPPDATA%\Nakagawa\saves\<disc id>` on Windows, `$XDG_DATA_HOME/nakagawa-recomp/saves/<disc id>` on Linux) | **PASS** | **PASS** | **PASS** | Resolved in `nk_launch_prepare_session`; verified in `tests/native/test_launch_resolution.c` on Windows and Linux |
| `CONFIG_PERSISTENCE_VALID` | Settings serialize and deserialize without schema degradation | **PASS** | **PASS** (Native C JSON persistence) | **PASS** | Verified in `interface` unit tests and `nk_library` tests |
| `LOGS_WRITTEN` | Logs cleanly piped to `logs/` directory without polluting UI | **PASS** (Log ring buffer) | **PASS** (`logs/native_player.log`) | **PASS** | Verified in process managers |
| `NO_C_NK_RUNTIME_DEPENDENCY_IN_PLAYER_PACKAGE` | Native player runs without assuming hardcoded workspace paths | **FAIL** (Legacy scripts had hardcoded workspace assumptions) | **PASS** (Uses relative & platform paths) | **PASS** | Verified in path normalization tests |

---

## 2. Regression Gate Checklist for Native Slices

Before declaring any native player slice complete, verify each gate with its owner:

1. [ ] **Binary compilation:** `mingw32-make player` and `mingw32-make native-core-tests`
   build without warnings on Windows UCRT64 GCC, and `native-core-tests` also builds on
   hosted Linux.
2. [ ] **Window lifecycle:** the window presents before any disc read, indexing or
   extraction; that work runs on the staging worker (`src/player/setup_staging.c`).
   `mingw32-make display-smoke-player` exercises the launch path. No latency figure is
   claimed until one is measured.
3. [ ] **Multi-title invariant:** no `hst` or disc-ID logic in the UI; identity and launch
   resolve from the manifest and catalog (`src/core/nk_title_manifest.c`,
   `src/core/generated/nk_title_catalog.c`). Covered by
   `tests/native/test_launch_resolution.c`.
4. [ ] **LLE compliance:** no synthetic TTF substitution in the guest runtime, no fake
   `libfont` readiness flags, and no fake `psmf` return values. Missing firmware fonts
   fail closed with guidance.
5. [ ] **File dialog:** the native picker (`SDL_ShowOpenFileDialog` in
   `src/player/main.c`) opens without a terminal or console prompt.
6. [ ] **Error handling:** missing, corrupt or unsupported discs produce an actionable
   error card with a stable code (for example `ISO_CORRUPT`, `SOURCE_NOT_FOUND`,
   `STAGED_EXECUTABLE_INVALID`, `LIBRARY_WRITE_FAILED`, `RUNTIME_PACKAGE_NOT_READY`) and
   a recovery button. Covered by `tests/native/test_player_state.c`.
7. [ ] **Automated tests:** `python -m unittest tools.test_nk_core` and the
   `tests/native/` binaries run by `native-core-tests` pass.
8. [ ] **Typography:** the player loads a system UI font at run time through SDL3_ttf
   (for example DejaVu Sans on Linux) and falls back to SDL3 debug text; the repository
   and shipped binary bundle no fonts. Open fonts such as M PLUS Rounded 1c, Rubik,
   Inter and JetBrains Mono are design references only.
