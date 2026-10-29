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

A **PASS** or **PARTIAL** row must name a backticked repository path that exists, and a
**PASS** row must name at least one such path that is a test file (`test_*`, a
`*selftest*` source, or a file under `tests/`). An **UNBUILT** or **DROPPED** row must
name a tracking issue. `tools/lint_docs.py` enforces the path, vocabulary and issue
rules and fails closed on a missing or empty table. `tools/test_lint_docs.py` enforces
the PASS test-file rule and the section 2 rules, and it fails on any open font family,
any font claim the shipped player cannot back, and any unmeasured latency threshold in
this document.

**Surface** is `native player`, `headless nk_cli` (`tools/nk_cli.py`, `nk.ps1`), or
`none`. A capability with no surface is not a promise.

<!-- capability-disposition:begin -->

| ID | Capability | Status | Surface | Owning evidence |
| --- | :--- | :---: | :--- | :--- |
| `ISO_INSPECT_WORKS` | ISO9660 PVD and directory parsing from a raw disc image | **PASS** | native player, headless `nk_cli` | `tests/native/test_xb_parser.c`, `tools/test_nk_core.py` (`test_iso_inspection_success`) |
| `ISO_ART_TERMINAL_OUTCOMES` | Per-image loaded, absent, failed, and unsupported outcomes with bounded recovery and joined worker shutdown | **PARTIAL** | native player | `tests/native/test_player_ui.py` runs the art workers in the old renderer, which only its regression build keeps (`--ui=legacy`); the default launcher (the next UI) shows an art placeholder and does not draw disc art yet |
| `LAUNCHER_UI_DEFAULT` | The nk_ui launcher (`src/player/ui/`) is the only launcher: it draws every `PlayerView`, the shipped player has no switch to the old renderer, and a launcher that cannot start (UI fonts missing) gets a message box and exit code 4, never the old renderer | **PASS** | native player | `tests/native/test_player_ui.py` (`test_next_ui_is_the_only_launcher`, `test_every_view_is_drawn_by_the_launcher`, `LegacyRendererSourceTests`, `test_next_ui_mouse_and_keyboard_reach_the_add_card`) |
| `TITLE_ID_DETECTION_WORKS` | `DISC_ID` / `TITLE` from `PARAM.SFO`, matched against the qualified title catalog | **PASS** | native player, headless `nk_cli` | `tools/test_nk_core.py` (`test_title_registry_matching_and_normalization`), `tests/native/test_launch_resolution.c` |
| `PREPARED_FOLDER_VALIDATION_WORKS` | Transactional staging and validation of the game directory | **PASS** | native player | `tests/native/test_xb_parser.c`, `tests/native/test_launch_resolution.c` |
| `MODULE_DECRYPTION` | Built-in decryption of encrypted executables and PRX modules | **PASS** | native player | Restored with built-in tables ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)): the disc check decrypts EBOOT.BIN and encrypted modules of a UMD image into the title's folder; one it can't decrypt fails closed as `ENCRYPTED_EXECUTABLE` or `ENCRYPTED_MODULE` with a named result; `tools/test_psp_decrypt.py` (`tests/native/test_player_state.c`, `tools/test_iso_parity.py`) |
| `PSP_ISO_ENV_HANDOFF_WORKS` | `PSP_ISO` handed to the runtime by the typed launch session | **PASS** | native player, headless `nk_cli` | `tools/test_nk_core.py` (`test_runtime_launcher_plan_construction`) |
| `RUNTIME_PROCESS_STARTS` | Host launcher spawns the runtime binary and the library exposes `PLAY` (`PLAY NOW` in the headless log) or `RUNTIME REQUIRED` | **PASS** | native player, headless `nk_cli` | `tools/test_player_package_route.py` |
| `VULKAN_WINDOW_STARTS` | SDL3 window and Vulkan swapchain | **PARTIAL** | native player, native runtime | `tools/test_production_smoke.py` pins the offscreen host-sink route. A real window is created only by the developer display gate `mingw32-make display-smoke-player`, which hosted CI never runs, so no CI result covers visible pixels |
| `SAVES_PATH_VALID` | `SR_MEMSTICK` routed to a writable per-disc location: the catalog's `memory_stick_root` when the install is writable, otherwise the platform save directory (Windows `%LOCALAPPDATA%`, then `FOLDERID_LocalAppData` or `%APPDATA%`, at `Nakagawa\saves\<disc id>`; Linux `$XDG_DATA_HOME/nakagawa-recomp/saves/<disc id>`) | **PASS** | native player, headless `nk_cli` | `tests/native/test_launch_resolution.c`, run on Windows and Linux |
| `CONFIG_PERSISTENCE_VALID` | Settings serialize and deserialize without schema degradation | **PASS** | native player | `tests/native/test_player_state.c` |
| `LOGS_WRITTEN` | Per-title package-build log and progress journal | **PARTIAL** | native player | `tests/native/test_package_builder.c`, `tools/test_nk_cli_progress.py`. There is no general player log: the earlier `logs/native_player.log` never existed in source |
| `NO_C_NK_RUNTIME_DEPENDENCY_IN_PLAYER_PACKAGE` | No hardcoded workspace paths; relative and platform paths only | **PASS** | native player | `tools/test_workspace_paths.py` |
| `PLAYER_LANDING_CARD` | Home hero card, per-title quick specs, runtime readiness and boot status | **PASS** | native player | `tools/test_player_package_route.py` |
| `GRAPHICS_SETTINGS` | Resolution scale, game-frame scaling (sharp, integer or bilinear, [#1129](https://github.com/Jstar269/nakagawa-recomp/issues/1129)), frame cap, fullscreen, VSync and volume, persisted to `settings.json` | **PARTIAL** | native player | `tests/native/test_player_state.c`. No MSAA, aspect-ratio or CRT-shader control exists; the runtime presents `VK_SAMPLE_COUNT_1_BIT` with no user-facing toggle |
| `CONTROLLER_CALIBRATION` | Button mapping, deadzone and trigger calibration, live input monitor, guided resting/extreme wizard | **PASS** | native player | `tests/native/test_input_settings.c`, `tests/native/test_input_profile.c` ([#357](https://github.com/Jstar269/nakagawa-recomp/issues/357)) |
| `HOST_PREFLIGHT_DOCTOR` | Host, toolchain, private-input and publication-contract preflight | **PASS** | headless `nk_cli` (`nk.ps1 Doctor`) | `tools/test_hst_doctor.py`, `docs/WORKSPACE_DOCTOR.md` |
| `DISC_PREFLIGHT_CHECKS` | Per-disc compatibility checks surfaced in the wizard and library card | **PASS** | native player | `tests/native/test_player_state.c` |
| `STAGING_PROGRESS` | Bounded copy/unpack counts and percentages during staging | **PASS** | native player | `tests/native/test_player_state.c` (asserts the recorded percent and file count) |
| `BUILD_PIPELINE_UI` | `BUILD PACKAGE` / `REBUILD PACKAGE` with live stage progress, recent output, elapsed time and cancel | **PASS** | native player, headless `nk_cli` | `tests/native/test_package_builder.c`, `tools/test_nk_cli_progress.py` |
| `PACKAGE_CHECK_FAILURE_RECOVERY` | Worker-start failure shows `PACKAGE CHECK FAILED`; `RETRY PACKAGE CHECK` and bounded background backoff can recover without losing a completed build's prepared state | **PASS** | native player | `tests/native/test_player_ui.py` |
| `ERROR_RECOVERY_UI` | Structured error card with a stable code and a recovery action | **PASS** | native player | `tests/native/test_player_state.c` |
| `MOVED_ISO_RECOVERY` | Fail-closed moved/missing disc detection with fallback lookup | **PASS** | native player, headless `nk_cli` | `tools/test_nk_core.py` |
| `COMMERCIAL_TITLE_COMPATIBILITY` | Any disc other than the source-owned fixtures | **PARTIAL** | native player, headless `nk_cli` | `tools/test_player_package_route.py` drives the whole consumer route on the source-owned display guest. A maintainer-held disc has reached its title menu ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487), [#358](https://github.com/Jstar269/nakagawa-recomp/issues/358)); that evidence needs private inputs, so no CI result and no consumer-run gate reproduces it |
| `LIVE_FRAME_OVERLAY` | F3 / `SR_HUD=1` in-game overlay: game FPS (new frames), VBlanks per frame, presents per second, VBlank rate, audio active status | **PARTIAL** | native runtime | `tools/test_profile_zero_e2e.py` and `tools/test_player_package_route.py` regression-test the `SR_PERF` / `SR_PERF_CSV` telemetry the overlay renders, and `profiler-selftest` pins the new-frame count against re-presents. The F3 mode cycle (off, HUD, then the profile HUD in developer mode) is asserted by `mingw32-make gpu-capture-selftest`; the overlay's own composition is covered only by the developer display gate |
| `IN_GAME_PAUSE` | `Esc`, `F1`, Guide or a lost controller pauses the guest; the overlay holds all input and a button held across resume is not a fresh press | **PARTIAL** | native runtime | `src/rt/sched_selftest.c` (`mingw32-make sched-selftest`), `tests/native/test_input_profile.c`, and the pause input and overlay cases in `mingw32-make gpu-capture-selftest`. The overlay's pages ([#1159](https://github.com/Jstar269/nakagawa-recomp/issues/1159)): `mingw32-make nk-ui-overlay-screens-selftest` (flow, focus, fit, `fixtures/nk_ui/overlay.golden`, the controls description) and `mingw32-make nk-ui-overlay-host-selftest` (the runtime host on SDL3_ttf and real key events; SKIP without SDL3_ttf). Not covered: a real title paused with the pages up, which needs a private route |
| `PERF_HOT_BLOCK_COUNTERS` | Per-PC call, block and duration counts from the AOT profiler, dumped at exit under `SR_PROFILE=1` | **PASS** | native runtime | `src/rt/profiler_selftest.c` (`mingw32-make profiler-selftest`) |
| `PERF_PROFILE_VIEW` | Live view of the `nakagawa-perf-v1` counters in developer mode: F3's profile HUD (frame, guest/host split, AOT/interpreter residency and transitions, scheduler residency, GE CPU, Vulkan submit/wait/pipeline/readback, audio underruns, the hottest transition PC, one bar per second) and the pause menu's Profile page | **PARTIAL** | native runtime | `mingw32-make profiler-selftest` (the history ring, the row formatter and the top transition PCs), `mingw32-make nk-ui-overlay-screens-selftest` (the page, its developer-mode gate and `fixtures/nk_ui/overlay.golden`), `mingw32-make gpu-capture-selftest` (the F3 mode cycle). Not yet: the view's own overhead measured against the `SR_PERF` bounds of [#995](https://github.com/Jstar269/nakagawa-recomp/pull/995), and a flame chart or hot-block view ([#314](https://github.com/Jstar269/nakagawa-recomp/issues/314)) |
| `FRAMEBUFFER_SNAPSHOTS` | `SR_FBSNAP` PPM framebuffer captures with diffing and PNG conversion | **PARTIAL** | native runtime | `tools/test_ppmdiff_coverage.py` covers the diffing in `tools/ppmdiff.py`; see `docs/DEBUGGING.md`. PNG conversion (`tools/ppm2png.py`) has no named test |
| `VRAM_VIEWER` | Developer mode's VRAM viewer on the pause menu: the display and draw framebuffers, the depth buffer and the bound texture, or any window of the 2 MiB by address, stride and format (5650, 5551, 4444, 8888, DEPTH16), read-only | **PARTIAL** | native runtime | `mingw32-make ge-cpu-profile-validate-selftest` (the surface and bound-texture decoders, nothing written), `mingw32-make nk-ui-overlay-screens-selftest` (window clamping and stepping, the page's flow, its developer-mode gate, `fixtures/nk_ui/overlay.golden`). Not yet: frame identity with the page open versus closed asserted by a test, and stepping between `SR_VRAMDUMP` captures ([#314](https://github.com/Jstar269/nakagawa-recomp/issues/314)) |
| `VRAM_CAPTURE_EXPORT` | Present-aligned capture of guest VRAM and GE state (`SR_VRAMDUMP`), exported as linear, swizzled, CLUT, DXT and depth PNGs | **PASS** | native runtime, headless `nk_cli` | `fixtures/display_smoke/generate.py` (`mingw32-make display-smoke-run`: decoded-pixel parity and capture-on/off frame identity), `tools/test_nk_cli_progress.py` (`NkCliVramTests`), `docs/DEBUGGING.md` |
| `FUNCTION_AND_CHUNK_CENSUS` | MIPS function count, generated chunk table and executable-span scope reports | **PARTIAL** | none | `tools/test_analyzer_span_scope.py` (span scope and chunk generation), `tools/test_build_graph_snapshot.py` (generated chunk count). No named test asserts the MIPS function count, and no `nk_cli` subcommand exposes a census |
| `SPAN_INSPECTOR_VIEW` | Interactive executable-span or chunk viewer | **UNBUILT** | none | In the works: [#314](https://github.com/Jstar269/nakagawa-recomp/issues/314) |
| `LOCALHOST_DASHBOARD` | The browser dashboard itself: live local server, browser-sandboxed ISO drop, disjoint second window, all eight screens | **DROPPED** | none | Removed in [#522](https://github.com/Jstar269/nakagawa-recomp/pull/522); this table is its replacement inventory |

<!-- capability-disposition:end -->

Notes on what the table deliberately does not claim:

- Since the next UI became the only launcher, the tests that pin the old
  renderer's layout, hit boxes, art workers and package badges run with
  `--ui=legacy`, which only the regression build accepts; `LEGACY_RENDERER_TESTS` in `tests/native/test_player_ui.py` lists
  them. The game-found card, Settings, the controller settings screen and the
  developer log are next-UI screens
  (`test_next_ui_game_found_cards_use_plain_words_and_offer_add_honestly`,
  `test_next_ui_settings_changes_are_saved_and_done_returns`,
  `test_next_ui_controller_rebind_calibrate_save_and_back`,
  `test_next_ui_developer_log_buttons`).

- Every automated row runs on source-owned input: the `display-smoke-v1`,
  `profile-zero` or synthetic fixture guests, or a synthetic ISO/XB tree. No row
  asserts commercial-title behaviour, PSP hardware timing, or audio playback.
- `PERF_HOT_BLOCK_COUNTERS` proves the counting table, not the readability of its
  text dump; no test parses the `PERF_PROFILE` output.
- `tools/mem_debug.py` is not a VRAM viewer: it is private flagship-only tooling
  ([#368](https://github.com/Jstar269/nakagawa-recomp/issues/368)) that is
  deliberately not advertised as a generic runtime facility.

## 2. Regression gate checklist for native slices

Each gate carries exactly one status and the evidence that backs it. **PASS** means a
named repository test runs the gate on source-owned input, and the item cites that test
file. **PARTIAL** means the cited files back the named subset, and the item states what
is not verified. **UNBUILT** means not implemented, and the item names its tracking
issue. A completed slice must also have its capability row above at `PASS` or `PARTIAL`.
The checkboxes were replaced by statuses, because an unticked box cannot say whether a
gate failed or was never run. `tools/test_lint_docs.py` rejects an item with no status, a
PASS item with no existing test file, a PARTIAL item with no existing path, and any named
path that does not exist.

1. **PARTIAL** **Binary compilation:** hosted CI builds `player` on Windows (`windows_runtime`)
   and `native-core-tests` on Windows and Linux (`native_tools`). The player compiles with
   `-Wall -Wextra` and no `-Werror`, so a warning does not fail the build. Evidence:
   `.github/workflows/ci.yml`.
2. **PARTIAL** **Window lifecycle:** the window presents before any disc read, indexing or
   extraction, and that work runs on the staging worker (`src/player/setup_staging.c`).
   Hosted CI covers only the offscreen route (`tools/test_production_smoke.py`). The
   visible window and its ordering run only in the developer gate
   `mingw32-make display-smoke-player`, which CI does not run. Window latency is
   UNMEASURED, and no figure is claimed.
3. **PARTIAL** **Multi-title invariant:** identity and launch resolve from the manifest and
   catalog (`src/core/nk_title_manifest.c`, `src/core/generated/nk_title_catalog.c`), which
   `tests/native/test_launch_resolution.c` covers. No named test asserts that the UI has no
   `hst` or disc-ID logic.
4. **PARTIAL** **LLE compliance:** `tools/test_hle_title_config_behavior.py` covers the
   `LIBFONT_STARTUP_UNAVAILABLE` naming, the `SR_REAL_MODULE_START=0` kill switch and the
   refused host readiness binding. No named test asserts the absence of a synthetic TTF
   substitution or of fake `psmf` return values, and none in this item covers the
   fail-closed firmware-font refusal.
5. **PARTIAL** **File dialog:** `src/player/main.c` opens the native picker with
   `SDL_ShowOpenFileDialog`, and `tests/native/test_player_state.c` asserts that the wizard
   requests the picker. No automated test checks that the picker opens without a terminal
   or console prompt.
6. **PASS** **Error handling:** an actionable error card with a stable code and a recovery
   action is asserted by `tests/native/test_player_state.c` (`RUNTIME_PACKAGE_NOT_READY`
   and its recovery label) and by `tests/native/test_player_ui.py` (`ISO_CORRUPT`,
   `SOURCE_NOT_FOUND`, `STAGED_EXECUTABLE_INVALID`, `LIBRARY_WRITE_FAILED`,
   `RUNTIME_PACKAGE_NOT_READY`).
7. **PASS** **Automated tests:** `tools/test_nk_core.py` backs the Python gate
   `python -m unittest tools.test_nk_core`, and `native-core-tests` runs the `tests/native/`
   binaries (for example `tests/native/test_xb_parser.c`).
8. **PARTIAL** **Typography:** the player loads a system UI font at run time through SDL3_ttf
   (for example DejaVu Sans on Linux) and falls back to SDL3 debug text when that font is
   unavailable or `NK_UI_NO_TTF=1` is set. `tests/native/test_player_ui.py` forces the
   fallback and asserts its reason. The player ships no font of its own: the in-game PGF
   fonts come from the user's own PSP import or the project `font/` folder
   (`docs/SETUP.md`). No named test checks that no font ships, so that half stays PARTIAL.
   Evidence: `src/player/ui_renderer.c`, `tests/native/test_player_ui.py`.
