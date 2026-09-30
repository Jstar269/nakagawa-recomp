# Native UI Regression Matrix & Capability Disposition

This document is the authoritative disposition of every capability the retired
localhost web dashboard used to provide, and of every diagnostic capability the
product still owes a consumer. It replaces the earlier parallel status tables: one
row per capability, one status vocabulary, one named owner.

The native player is the only UI in this repository. The browser dashboard and its
migration proposals are historical evidence only
([`archive/ui-baseline/README.md`](archive/ui-baseline/README.md)), removed in
[#522](https://github.com/Jstar269/nakagawa-recomp/pull/522).

The portable preparation engine in `tools/nk_core/` remains a standalone
prototype. The first-time setup wizard has a separate native,
standalone ISO/XB staging path: it copies only `EBOOT.BIN` and
`PSP_GAME/USRDIR/xbdata`, decodes validated `.xb` members into runtime-compatible
`<archive>.xb.d/` directories, records the
asset/audio/visual/layout census, and promotes the isolated staging tree atomically
into the actionable `PLAYER_VIEW_READY_LIBRARY` state. Anything the staging path
does not do is named as such below rather than implied by it.

## 1. Status vocabulary and the evidence rule

| Status | Meaning |
| :--- | :--- |
| **PASS** | A named repository test exercises the capability on source-owned input. |
| **PARTIAL** | The capability works for the named subset; the cell states exactly what is absent and why. |
| **UNBUILT** | Not implemented. Named as *in the works* with its tracking issue. |
| **DROPPED** | Deliberately retired. Named with the change that removed it. |

A **PASS** or **PARTIAL** row must name a backticked repository path that exists; an
**UNBUILT** or **DROPPED** row must name a tracking issue. `tools/lint_docs.py`
enforces both, fails closed on a missing or empty table, and rejects any status
outside this vocabulary.

**Surface** is `native player`, `headless nk_cli` (`tools/nk_cli.py`, `nk.ps1`), or
`none`. A capability with no surface is not a promise.

<!-- capability-disposition:begin -->

| ID | Capability | Status | Surface | Owning evidence |
| --- | :--- | :---: | :--- | :--- |
| `ISO_INSPECT_WORKS` | ISO9660 PVD and directory parsing from a raw disc image | **PASS** | native player, headless `nk_cli` | `tests/native/test_xb_parser.c`, `tools/test_nk_core.py` (`test_iso_inspection_success`) |
| `TITLE_ID_DETECTION_WORKS` | `DISC_ID` / `TITLE` from `PARAM.SFO`, matched against the qualified title catalog | **PASS** | native player, headless `nk_cli` | `tools/test_nk_core.py` (`test_title_registry_matching_and_normalization`), `tests/native/test_launch_resolution.c` |
| `PREPARED_FOLDER_VALIDATION_WORKS` | Transactional staging and validation of the game directory | **PASS** | native player | `tests/native/test_xb_parser.c`, `tests/native/test_launch_resolution.c` |
| `MODULE_DECRYPTION` | Local-key decryption of an encrypted executable and its PRX modules | **PASS** | native player, headless `nk_cli` | `tools/test_psp_decrypt.py`; `src/core/nk_psp_kirk.c` and `tools/nk_cli.py` `build-package`. Keys are never shipped and decrypted bytes stay in the private per-title folder ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)) |
| `PSP_ISO_ENV_HANDOFF_WORKS` | `PSP_ISO` handed to the runtime by the typed launch session | **PASS** | native player, headless `nk_cli` | `tools/test_nk_core.py` (`test_runtime_launcher_plan_construction`) |
| `RUNTIME_PROCESS_STARTS` | Host launcher spawns the runtime binary and the library exposes `PLAY NOW` or `RUNTIME REQUIRED` | **PASS** | native player, headless `nk_cli` | `tools/test_player_package_route.py` |
| `VULKAN_WINDOW_STARTS` | SDL3 window and Vulkan swapchain | **PARTIAL** | native player, native runtime | `tools/test_production_smoke.py` pins the offscreen host-sink route. A real window is created only by the developer display gate `mingw32-make display-smoke-player`, which hosted CI never runs, so no CI result covers visible pixels |
| `SAVES_PATH_VALID` | `SR_MEMSTICK` routed to a writable per-disc location: the catalog's `memory_stick_root` when the install is writable, otherwise the platform save directory (Windows `%LOCALAPPDATA%`, then `FOLDERID_LocalAppData` or `%APPDATA%`, at `Nakagawa\saves\<disc id>`; Linux `$XDG_DATA_HOME/nakagawa-recomp/saves/<disc id>`) | **PASS** | native player, headless `nk_cli` | `tests/native/test_launch_resolution.c`, run on Windows and Linux |
| `CONFIG_PERSISTENCE_VALID` | Settings serialize and deserialize without schema degradation | **PASS** | native player | `tests/native/test_player_state.c` |
| `LOGS_WRITTEN` | Per-title package-build log and progress journal | **PARTIAL** | native player | `tests/native/test_package_builder.c`, `tools/test_nk_cli_progress.py`. There is no general player log: the earlier `logs/native_player.log` never existed in source |
| `NO_C_NK_RUNTIME_DEPENDENCY_IN_PLAYER_PACKAGE` | No hardcoded workspace paths; relative and platform paths only | **PASS** | native player | `tools/test_workspace_paths.py` |
| `PLAYER_LANDING_CARD` | Home hero card, per-title quick specs, runtime readiness and boot status | **PASS** | native player | `tools/test_player_package_route.py` |
| `GRAPHICS_SETTINGS` | Resolution scale, frame cap, fullscreen, VSync and volume, persisted to `settings.json` | **PARTIAL** | native player | `tests/native/test_player_state.c`. No MSAA, aspect-ratio or CRT-shader control exists; the runtime presents `VK_SAMPLE_COUNT_1_BIT` with no user-facing toggle |
| `CONTROLLER_CALIBRATION` | Button mapping, deadzone and trigger calibration, live input monitor, guided resting/extreme wizard | **PASS** | native player | `tests/native/test_input_settings.c`, `tests/native/test_input_profile.c` ([#357](https://github.com/Jstar269/nakagawa-recomp/issues/357)) |
| `HOST_PREFLIGHT_DOCTOR` | Host, toolchain, private-input and publication-contract preflight | **PASS** | headless `nk_cli` (`nk.ps1 Doctor`) | `tools/test_hst_doctor.py`, `docs/WORKSPACE_DOCTOR.md` |
| `DISC_PREFLIGHT_CHECKS` | Per-disc compatibility checks surfaced in the wizard and library card | **PASS** | native player | `tests/native/test_player_state.c` |
| `STAGING_PROGRESS` | Bounded copy/unpack counts and percentages during staging | **PASS** | native player | `tests/native/test_player_state.c` (asserts the recorded percent, files and total) |
| `BUILD_PIPELINE_UI` | `BUILD PACKAGE` / `REBUILD PACKAGE` with live stage progress, recent output, elapsed time and cancel | **PASS** | native player, headless `nk_cli` | `tests/native/test_package_builder.c`, `tools/test_nk_cli_progress.py` |
| `ERROR_RECOVERY_UI` | Structured error card with a stable code and a recovery action | **PASS** | native player | `tests/native/test_player_state.c` |
| `MOVED_ISO_RECOVERY` | Fail-closed moved/missing disc detection with fallback lookup | **PASS** | native player, headless `nk_cli` | `tools/test_nk_core.py` |
| `COMMERCIAL_TITLE_COMPATIBILITY` | Any disc other than the source-owned fixtures | **PARTIAL** | native player, headless `nk_cli` | `tools/test_player_package_route.py` drives the whole consumer route on the source-owned display guest. A maintainer-held disc has reached its title menu ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487), [#358](https://github.com/Jstar269/nakagawa-recomp/issues/358)); that evidence needs private inputs, so no CI result and no consumer-run gate reproduces it |
| `LIVE_FRAME_OVERLAY` | F1 / `SR_HUD=1` in-game overlay: presented FPS, frame time, VBlank rate, audio active status | **PARTIAL** | native runtime | `tools/test_profile_zero_e2e.py` and `tools/test_player_package_route.py` regression-test the `SR_PERF` / `SR_PERF_CSV` telemetry the overlay renders. The overlay's own composition and the F1 toggle are covered only by the developer display gate, not by an automated assertion |
| `PERF_HOT_BLOCK_COUNTERS` | Per-PC call, block and duration counts from the AOT profiler, dumped at exit under `SR_PROFILE=1` | **PASS** | native runtime | `src/rt/profiler_selftest.c` (`mingw32-make profiler-selftest`) |
| `PERF_PROFILE_VIEW` | Interactive hot-block visualizer, flame chart or profile viewer | **UNBUILT** | none | In the works: [#314](https://github.com/Jstar269/nakagawa-recomp/issues/314) |
| `FRAMEBUFFER_SNAPSHOTS` | `SR_FBSNAP` PPM framebuffer captures with diffing and PNG conversion | **PASS** | native runtime, headless `nk_cli` | `tools/test_ppmdiff_coverage.py`, `docs/DEBUGGING.md` |
| `VRAM_VIEWER` | Interactive guest-VRAM inspection | **UNBUILT** | none | In the works: [#314](https://github.com/Jstar269/nakagawa-recomp/issues/314) |
| `FUNCTION_AND_CHUNK_CENSUS` | MIPS function count, generated chunk table and executable-span scope reports | **PASS** | headless `nk_cli` | `tools/test_build_graph_snapshot.py`, `tools/test_analyzer_span_scope.py` |
| `SPAN_INSPECTOR_VIEW` | Interactive executable-span or chunk viewer | **UNBUILT** | none | In the works: [#314](https://github.com/Jstar269/nakagawa-recomp/issues/314) |
| `LOCALHOST_DASHBOARD` | The browser dashboard itself: live local server, browser-sandboxed ISO drop, disjoint second window, all eight screens | **DROPPED** | none | Removed in [#522](https://github.com/Jstar269/nakagawa-recomp/pull/522); this table is its replacement inventory |

<!-- capability-disposition:end -->

Notes on what the table deliberately does not claim:

- Every automated row runs on source-owned input: the `display-smoke-v1`,
  `profile-zero` or synthetic fixture guests, or a synthetic ISO/XB tree. No row
  asserts commercial-title behaviour, PSP hardware timing, or audio playback.
- `PERF_HOT_BLOCK_COUNTERS` proves the counting table, not the readability of its
  text dump; no test parses the `PERF_PROFILE` output.
- `tools/mem_debug.py` is not a VRAM viewer: it is private flagship-only tooling
  ([#368](https://github.com/Jstar269/nakagawa-recomp/issues/368)) that is
  deliberately not advertised as a generic runtime facility.

## 2. Regression gate checklist for native slices

This is a per-slice authoring template, not an acceptance oracle: each gate must be
satisfied and cited before a slice is declared complete, and a completed slice must
have its capability row above at `PASS` or `PARTIAL` with the evidence that was run.
An unticked box records work not yet done; it never records a failure.

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
