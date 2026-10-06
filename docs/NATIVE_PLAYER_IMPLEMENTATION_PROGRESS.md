# Native Player Implementation Progress & Working-State Verification

## 1. Executive Summary

Nakagawa Recomp has integrated a native SDL3 player; it is the only UI in the public repository.

The core product design target remains:
$$\text{NAKAGAWA PROGRAM} + \text{USER'S GAME ISO} \longrightarrow \text{AUTHENTIC RECOMPILED PLAY}$$

This milestone was executed under the strict **LLE / Original Guest Execution** doctrine:
$$\text{ORIGINAL\_GUEST\_EXECUTION} \succ \text{LLE/GENERIC PSP BEHAVIOR} \succ \text{GENERIC HLE} \succ \text{TITLE PATCHES (TOWARD ZERO)}$$

### Key Accomplishments

1. **Historical UI Baseline Retained**: `docs/archive/ui-baseline/` marks the retired browser screens as historical and records the native player baseline.
2. **Standalone Native Player Implemented (`src/player/`)**:
   - `iso_reader.c` / `iso_reader.h`: Pure C ISO9660 PVD reader and `PARAM.SFO` parser identifying `DISC_ID`, `TITLE`, and matching against qualified title registries.
   - `player_state.c` / `player_state.h`: Finite-state machine managing library games, inspection, asynchronous extraction metrics, settings, and structured recovery actions.
   - `setup_staging.c` / `setup_staging.h`: Native worker-facing staging boundary that keeps cancellation/progress separate from SDL and invokes the source-owned ISO/XB layers.
   - The optional local bridge discovers named plain support PRXs the user has already placed in a recognized local folder and stages them under `EXTRACTED/decrypted/`. Separately, the built-in decryption boundary (`src/core/nk_psp_container.c`) unwraps the disc's encrypted executable and its encrypted PRX modules, but only when the user's own key file is present (`<user data>/keys/psp-keyfile.json` or `NAKAGAWA_PSP_KEY_FILE`) and only into the private per-title folder. The player holds no key material, a user-supplied plain module still wins, and a missing key entry fails closed naming the entry
     ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).
   - `ui_renderer.c` / `ui_renderer.h`: High-performance SDL3 renderer using the Dark Court palette, responsive card layouts, auto-scaled typography, and offscreen screenshot capabilities.
   - `main.c`: Interactive event loop with native file dialog (`SDL_ShowOpenFileDialog`), reactive SDL worker notifications, gamepad detection and d-pad/shoulder library navigation, arrow-key and scroll-wheel selection across the whole library, drag-and-drop ISO support, and a headless test driver. Demo fixtures are opt-in (`--demo`, or any `--view=` capture run) and are never written to the user's library file.
   - `nk_xb.c` / `nk_xb.h`: Project-authored bounded XB FST parser and native LZS/Huffman/nested tag-0 decoder; no `third_party/libxb` dependency.
   - `nk_iso_extract_game`: ISO directory-record walk that stages `EBOOT.BIN` and `USRDIR/xbdata` without shelling out or requiring Python.
3. **Build System Integration**: Integrated `player` target into `Makefile` (`mingw32-make player`), compiling alongside runtime objects without MSVC dependencies.
4. **Portable Core Expansion (`tools/nk_core/`)**: Added persistent `GameLibrary`, moved-ISO detection, fallback resolution, space-tolerant paths, and full Unicode/CJK path support.
5. **Continuous Working-State Preservation**: Existing playable HST route, test suites (`test_nk_core.py`, `test_build_truth.py`, and full tools test suite) remain 100% green.

### Runtime/setup progression (2026-09-13)

- Staging now returns a bounded census of XB members plus `.sgd`/`.sgh`/`.sgb`
  audio, `.gim` visual, and `data/menu/` layout members. The counts are kept
  with the library entry as staging facts.
- A completed worker promotion registers the inspected title in `app.games` and
  enters `PLAYER_VIEW_READY_LIBRARY`. The card shows the disc ID and staged
  asset census, with `PLAY NOW` for a validating package or a resolvable
  non-experimental catalog runtime ([#483](https://github.com/Jstar269/nakagawa-recomp/pull/483)),
  `BUILD PACKAGE`/`REBUILD PACKAGE` when the package is missing or stale, and
  a `RUNTIME REQUIRED` status for a staged entry with no runnable runtime.
  A failed package-status worker shows `PACKAGE CHECK FAILED` with the
  `RETRY PACKAGE CHECK` action; background retries use bounded backoff.
- `--iso=<path> --stage-only` is a headless path through the same native staging,
  atomic promotion, registration, and state transition. `--stage` starts the
  same transaction from the interactive wizard.
- `nk_launch.c` resolves staged `EBOOT.BIN`, prefers the promoted `xbdata`
  root (whose archives unpack as `<archive>.xb.d/` for the runtime asset index),
  creates a per-title `memstick` root, and validates
  plain ELF32/MIPS program headers against catalog base/entry metadata. A
  `~PSP` file is recognized as an encrypted source container only; no inner ELF
  or private retail acceptance is claimed.

---

## 2. Visual Baseline vs. Native Player Results

### Baseline Web Interface (Archived in `docs/archive/ui-baseline/`)

- `01_web_player_mode.png`: Web Player Landing
- `02_web_studio_iso_loader.png`: ISO Loader (Browser drag-and-drop, sandbox-constrained)
- `03_web_studio_pipeline_build.png`: Build & Recompile Pipeline
- `04_web_studio_graphics_config.png`: Graphics Settings
- `05_web_studio_controllers_config.png`: Controller Calibration
- `06_web_studio_troubleshooting_doctor.png`: Preflight Doctor
- `07_web_studio_profiler.png`: Performance Profiler
- `08_web_studio_internals.png`: Chunks & Span Inspector

### Native Player Screenshot Matrix (Captured via `python tools/capture_native_screenshots.py`)

| View State | Native Artifact | Resolution | Provenance & Validation |
| :--- | :--- | :--- | :--- |
| Empty Library | `native_01_empty_library.png` | 1280×720 | Clean CTA for ISO selection; title support disclosure |
| Add Game | `native_02_add_game.png` | 1280×720 | The empty-library CTA, captured with the same `--view=empty --empty` arguments as `native_01`. The host file dialog is opened by the button at runtime and is not part of this capture |
| ISO Inspecting | `native_03_iso_inspecting.png` | 1280×720 | Captured with no `--iso=`, so the inspector renders "No disc image selected" and a cancel action. With a disc it shows an indeterminate indicator: this build's inspector reports no percentage |
| Recognized Title | `native_04_supported_game.png` | 1280×720 | The synthetic fixture `TEST00001`, which is what `--view=supported` populates — no retail disc is involved; honest LLE font requirement note |
| Unsupported Title | `native_05_unsupported_game.png` | 1280×720 | Fail-closed boundary preventing unregistered execution |
| Legacy Preparation View | `native_06_preparing.png` | 1280×720 | "No preparation pipeline is connected in this build" remains an honest unavailable state; it is reachable today only as the captured `--view=preparing` input, while wizard Step 3 has a separate bounded staging progress view |
| Ready Library | `native_07_ready_library.png` | 1280×720 | Hero game card with "PLAY NOW" & Quick Specs Rail |
| Settings Dialog | `native_08_settings.png` | 1280×720 | Live resolution/FPS presets, display toggles, reduce-motion switch and volume stepper (persisted to settings.json); keyboard/gamepad focus on every control; single-column flow on narrow windows |
| Visual Craft | n/a | all | Runtime system-font typography (SDL3_ttf dlopen, zero bundled fonts, DebugText fallback), rounded cards/buttons/pills with shadows, disc ICON0.PNG runtime texture extraction with monogram fallback, dimmed PIC1.PNG hero backdrop, density-aware raster |
| Missing Source Error | `native_09_missing_source_error.png` | 1280×720 | Structured error recovery for moved or missing ISOs |
| 1080p Library | `native_10_library_1080p.png` | 1920×1080 | Verified responsive scaling on Full HD displays |
| 1080p Settings | `native_11_settings_1080p.png` | 1920×1080 | Verified responsive settings modal on Full HD displays |

---

## 3. Native UI Capability Status

Per-capability status is not maintained here. The single authoritative disposition
of every former web-dashboard and diagnostic capability — status, surface, owning
test path, and the tracking issue for anything unbuilt — lives in
[`NATIVE_UI_REGRESSION_MATRIX.md`](NATIVE_UI_REGRESSION_MATRIX.md), which
`tools/lint_docs.py` enforces. This file keeps the narrative: what was built, what
the launch path proves, and what it does not.

---

## 3a. The launch path, and the one title that exercises it

Until `display-smoke-v1` existed, no public title in this tree could be launched,
and the reason was not the spawn code. `src/core/nk_launch.c` resolves a runtime
by probing `build/<title_id>/<title_id>[.exe]` and its sibling
`<...>_image.bin`, and takes the load addresses from the generated title catalog.
Every fixture was built under a different directory and stem, so that probe never
matched anything and the entire resolution path was unreachable. The spawn was
unit-tested; the thing it was meant to spawn had no discoverable location.

`fixtures/display_smoke/generate.py` emits a source-owned PSP guest that fills
the framebuffer and flips it through `sceDisplaySetFrameBuf` once per frame, and
`mingw32-make display-smoke` builds it **under its own title id** so the existing
probe resolves it. `assets/titles/display-smoke.json` gives it a catalog entry
with the real load addresses. That makes it the first public-scope artifact the
player can resolve, start, and show.

What this establishes, exactly:

- the two-phase pipeline, the loader, the import/NID path, the scheduler's vblank
  delivery, `sceDisplaySetFrameBuf`, the display latch and `gui_present` all work
  together on a guest built from committed source, with no external toolchain,
  no retail disc and no private input;
- `display-smoke-run` first asserts the guest-visible framebuffer word with the
  headless `--sched` route, then runs the normal `--sched --gui` scheduler route
  with `SR_VIDEO=offscreen`. The second run uses the explicit host-memory sink,
  checks that `frame_present` precedes `HOST_PRESENT_SUBMITTED`, and never creates
  a window. The gate pins all four modern and legacy SDL video/audio selector
  names to `dummy`, and sets the offscreen selector itself, so inherited
  interactive selectors cannot change the evidence. Sanitized bring-up reports
  retain the accepted presenter's backend identity; this route reports
  `backend=offscreen`.
  This proves host-sink acceptance for the source-owned fixture; it does not
  prove visible pixels, live input, audio playback, private-title compatibility,
  or hardware/PSP acceptance.

The native-player path is exercised separately by
`mingw32-make display-smoke-player`. It runs the player with
`--demo --runtime-root=<repository> --launch-index=1`, so the entry must first
resolve through the same predicate the launch uses — a validating package or,
for a non-experimental catalog title, the developer runtime under the
repository build tree ([#483](https://github.com/Jstar269/nakagawa-recomp/pull/483));
the driver then asserts the generated `--gui` argument and the child runtime's
`window_ready`/`first_frame` boot events. This is a display-dependent developer
gate, not a retail-title claim.

The same player-owned launch runs headlessly with
`--launch-index=N --headless-launch`. The child is spawned without `--gui`, and
the player waits for it under a bounded timeout, reporting the child's own exit
status and a distinct failure status if the bound expires; the headless child
still emits the same `SR_BOOT_EVENT_FILE` startup milestones and consumes
vblanks, so the guest-visible frame checkpoint is readable without a display.
`tools/test_player_package_route.py` drives the whole consumer route on the
source-owned display guest: the BUILD PACKAGE action
(`package_builder_start` → `tools/nk_cli.py build-package`, real analysis,
codegen and compilation), the player's own package validator, the headless
launch, two named missing-prerequisite boundaries ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)
decrypted inputs, [#296](https://github.com/Jstar269/nakagawa-recomp/issues/296)
`--psp-header`), and a before/after check that the route changed no
repository-tracked file.

What it does not establish: any commercial-title compatibility, PSP timing or
rendering correctness, GE/graphics-pipeline behaviour (this guest writes the
framebuffer directly and submits no display list), audio, or that any other
title in the catalog can be launched by this test — the showcase demos are
built and packaged under their own title IDs and launch through the same
package validator ([#477](https://github.com/Jstar269/nakagawa-recomp/pull/477)),
but this driver does not exercise them, and a commercial title still needs a
plain executable plus a built package.

---

## 3b. The ISO art worker contract

The library card's `ICON0.PNG` icon and `PIC1.PNG` backdrop are read from the
ISO on a worker thread, so a disc image is never parsed on the UI thread.
`src/player/ui_renderer.c` keeps one cache entry per title (64 slots), and each
image carries its own attempt count and terminal result: loaded, absent, failed,
or unsupported by a build without a PNG decoder.

| Terminal state | Set when | Retries |
| :--- | :--- | :--- |
| `UI_ART_TERMINAL_LOADED` | a texture was created for that image | terminal |
| `UI_ART_TERMINAL_ABSENT` | the ISO or image entry was missing on the last read after `GAME_ART_MAX_ATTEMPTS` (3) | terminal |
| `UI_ART_TERMINAL_FAILED` | corrupt/oversized data or a transient decoder/renderer/worker failure persisted through the bounded retry budget | terminal |
| `UI_ART_TERMINAL_UNSUPPORTED` | the image was read off the disc intact (`NK_ICON_OK` with bytes), but this build has no PNG decoder (SDL < 3.4) | terminal on the **first** attempt |

- Even without a PNG decoder, an absent or unreadable ISO entry remains a
  retryable read failure; `unsupported` applies only after intact bytes arrive.
- A *successful* decode whose texture creation fails keeps the retry budget:
  that is a VRAM/renderer condition, not a decoder verdict.
- Retries are spaced by `GAME_ART_RETRY_COOLDOWN_NS` (1 s) shifted by
  `min(attempt - 1, 2)`, so the delay grows 1 s → 2 s → 4 s and then stops.
- A retry asks only for the image that is still missing, so a disc without
  `PIC1.PNG` never re-reads its `ICON0.PNG`.
- When a completion event cannot be queued, the worker marks the handoff
  failed, the UI thread reclaims that job, and the entry retries instead of
  staying pending for the session.
- `ui_font_shutdown()` joins every in-flight art worker and releases its decoded
  bytes before the renderer and the SDL event queue go away; the regression
  harness waits for a worker to enter its test-only delay before quitting, then
  reports both `art_jobs_active_at_shutdown` and `art_jobs_drained`.

The states are observable from the outside: a build with
`NK_PLAYER_UI_REGRESSION_TEST` prints `art_attempt index=… icon_loaded=…
pic1_loaded=… wanted=… icon_state=… pic1_state=…` for every completion, which is
what `tests/native/test_player_ui.py` reads to distinguish a missing image
(`absent` after three attempts), corrupt or persistent operational failure
(`failed` after three attempts), and readable bytes on a build without PNG
support (`unsupported` on the first attempt). A decoder rejection on a build
with PNG support is a bounded `failed` result.

The shutdown regression waits for the worker to confirm it is inside a test-only
delay, quits while that known job is active, and asserts the job is joined before
renderer/window teardown.

What this does not claim: the suite decodes no retail disc, `unsupported` says
the build lacks a PNG decoder rather than making a statement about the file, and
nothing here speaks to the correctness of the artwork pixels themselves.

---

## 4. Architectural Boundaries & LLE Compliance

1. **Rejection of Premature HLE**:
   - No wholesale substitution of `libfont.prx` with stb_truetype.
   - Authentic Sony PGF fonts (`jpn0.pgf`) and guest module execution remain the primary path.
   - PGD / console key material remains isolated from public checkouts.
2. **Separation of Concerns**:
   - `src/player/`: Native SDL3 player application.
   - `tools/nk_core/`: Portable Python core orchestration library.
3. **Fail-Closed Dispatch**:
   - Unsupported titles are clearly identified and prevented from running to avoid undefined crashes.
   - Missing or moved ISO files trigger structured error dialogs rather than silent halts.

---

## 5. Verification Commands

### Native Compilation

```powershell
mingw32-make player

mingw32-make native-core-tests

build/nakagawa_player.exe --view=wizard-staging --screenshot=build/wizard_staging.bmp
```

### Visual Verification

```powershell
python tools/capture_native_screenshots.py
```

### Test Suite Execution

```powershell
python -m unittest discover -s tools -p "test_nk_core.py" -v
python -m unittest tools/test_build_truth.py -v
```
