# Native Player Implementation Progress & Working-State Verification

## 1. Executive Summary

Nakagawa Recomp has successfully developed and integrated a native desktop player application (`build/nakagawa_player.exe`) designed to replace browser-based frontend friction with a standalone, cross-platform SDL3 experience.

The core product design target remains:
$$\text{NAKAGAWA PROGRAM} + \text{USER'S GAME ISO} \longrightarrow \text{AUTHENTIC RECOMPILED PLAY}$$

This milestone was executed under the strict **LLE / Original Guest Execution** doctrine:
$$\text{ORIGINAL\_GUEST\_EXECUTION} \succ \text{LLE/GENERIC PSP BEHAVIOR} \succ \text{GENERIC HLE} \succ \text{TITLE PATCHES (TOWARD ZERO)}$$

### Key Accomplishments

1. **Visual & Functional Baseline Captured**: All 8 developer studio panels and player mode landing screens archived in `docs/ui-baseline/`.
2. **Clean-Room Native Player Implemented (`src/player/`)**:
   - `iso_reader.c` / `iso_reader.h`: Pure C ISO9660 PVD reader and `PARAM.SFO` parser identifying `DISC_ID`, `TITLE`, and matching against qualified title registries.
   - `player_state.c` / `player_state.h`: Finite-state machine managing library games, inspection, preparation-unavailable state, settings, and structured recovery actions.
   - `ui_renderer.c` / `ui_renderer.h`: High-performance SDL3 renderer using the Dark Court palette, responsive card layouts, auto-scaled typography, and offscreen screenshot capabilities.
   - `main.c`: Interactive event loop with native file dialog (`SDL_ShowOpenFileDialog`), gamepad detection and d-pad/shoulder library navigation, arrow-key and scroll-wheel selection across the whole library, drag-and-drop ISO support, and a headless test driver. Demo fixtures are opt-in (`--demo`, or any `--view=` capture run) and are never written to the user's library file.
3. **Build System Integration**: Integrated `player` target into `Makefile` (`mingw32-make player`), compiling cleanly alongside runtime objects without MSVC or Node.js dependencies.
4. **Portable Core Expansion (`tools/nk_core/`)**: Added persistent `GameLibrary`, moved-ISO detection, fallback resolution, space-tolerant paths, and full Unicode/CJK path support.
5. **Continuous Working-State Preservation**: Existing playable HST route, test suites (`test_nk_core.py`, `test_build_truth.py`, and full tools test suite) remain 100% green.

---

## 2. Visual Baseline vs. Native Player Results

### Baseline Web Interface (Archived in `docs/ui-baseline/`)

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
| Preparation | `native_06_preparing.png` | 1280×720 | "No preparation pipeline is connected in this build" with an indeterminate indicator. The view deliberately claims no item counts or percentage because this build has no preparation backend |
| Ready Library | `native_07_ready_library.png` | 1280×720 | Hero game card with "PLAY NOW" & Quick Specs Rail |
| Settings Dialog | `native_08_settings.png` | 1280×720 | Resolution presets (1x..8x), 60 FPS, savedata path, and truthful audio/controller state |
| Missing Source Error | `native_09_missing_source_error.png` | 1280×720 | Structured error recovery for moved or missing ISOs |
| 1080p Library | `native_10_library_1080p.png` | 1920×1080 | Verified responsive scaling on Full HD displays |
| 1080p Settings | `native_11_settings_1080p.png` | 1920×1080 | Verified responsive settings modal on Full HD displays |

---

## 3. Native UI Regression Matrix Status (Audit & Evidence Strength Alignment)

The matrix distinguishes between architectural staging, implementation completeness, and verified execution:

- `PIPELINE_STAGE_EXISTS`: A state/step is declared in UI/data structures, but backend execution is not yet integrated.
- `NOT_IMPLEMENTED`: Underlying engine functionality (e.g. clean-room KIRK decryption) does not yet exist.
- `PLAN_VERIFIED`: Launch session data/environment parameters construct correctly in unit tests.
- `EXECUTED_VERIFIED`: Real process spawned, child landmarks observed on host.

| # | Capability | Web Studio Baseline | Native Player Status | Real Evidence Tier |
| :- | :--- | :--- | :--- | :--- |
| 1 | ISO Drag & Drop | Sandbox only | Full native filesystem read | **PASS** (Direct OS path handoff) |
| 2 | Disc Identification | Web Worker sector parse | Direct C ISO9660 PVD + SFO parse | **PASS** (Clean-room C PVD parser) |
| 3 | Title Qualification | Profile match in JS | Single authoritative manifest catalog | **PASS** (Derived from `assets/titles`) |
| 4 | Asset Extraction | External PowerShell script | Staged pipeline prototype in `nk_core` | **PARTIAL** (native player is not connected; VFS integration pending) |
| 5 | Module Decryption | External toolchain | **NOT_IMPLEMENTED** (KIRK engine pending) | **NOT_IMPLEMENTED** (Requires pre-decrypted inputs) |
| 6 | Runtime Launch | Node child_process spawn | Native launch session & process spawn | **EXECUTED_VERIFIED** for `display-smoke-v1` only (see below); `PLAN_VERIFIED` for every other title |
| 7 | Graphics Settings | Web localStorage | Native JSON configuration & CLI env | **PASS** (Verified serialization) |
| 8 | Gamepad Calibration | Web Gamepad API | SDL3 gamepad detection and library navigation | **PARTIAL** — a pad is opened, named and drives d-pad/shoulder selection, and the badge reports the real state. There is no calibration, binding or deadzone UI; the web baseline's calibration screen has no native counterpart |
| 9 | Preflight Checks | `hst_doctor.py` via HTTP | Integrated diagnostic rules | **PASS** (Portable rule engine) |
| 10 | Progress Feedback | Server-Sent Events (SSE) | Immediate-mode indeterminate preparation-unavailable state | **PARTIAL** — no native preparation pipeline supplies item counts or percentages in this build; the renderer reports that limitation instead of drawing a progress claim |
| 11 | Error Handling | HTML alert banner | Modal error dialog with recovery buttons | **PASS** (Structured recovery views) |
| 12 | Moved ISO Handling | Silent failure | Fail-closed detection + fallback lookup | **PASS** (Unit-tested recovery) |
| 13 | Multi-Title Support | Hardcoded HST strings | Data-driven manifest catalog | **IN_PROGRESS** (Unifying title contract) |

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
- `display-smoke-run` asserts the guest-visible framebuffer word headlessly, so
  the presentation path is gated in CI without a display.

The native-player path is exercised separately by
`mingw32-make display-smoke-player`. It runs the player with
`--demo --runtime-root=<repository> --launch-index=1`, so the entry must first
be marked `Prepared` by the same resolver the launch uses; the driver then
asserts the generated `--gui` argument and the child runtime's
`window_ready`/`first_frame` boot events. This is a display-dependent developer
gate, not a retail-title claim.

What it does not establish: any commercial-title compatibility, PSP timing or
rendering correctness, GE/graphics-pipeline behaviour (this guest writes the
framebuffer directly and submits no display list), audio, or that any other
title in the catalog can be launched — none of them are built under the layout
the launcher resolves.

---

## 4. Architectural Boundaries & LLE Compliance

1. **Rejection of Premature HLE**:
   - No wholesale substitution of `libfont.prx` with stb_truetype.
   - Authentic Sony PGF fonts (`jpn0.pgf`) and guest module execution remain the primary path.
   - PGD / console key material remains isolated from public checkouts.
2. **Separation of Concerns**:
   - `src/player/`: Native SDL3 player application.
   - `tools/nk_core/`: Portable Python core orchestration library.
   - `interface/`: Preserved as developer diagnostic tooling (Nakagawa Studio).
3. **Fail-Closed Dispatch**:
   - Unsupported titles are clearly identified and prevented from running to avoid undefined crashes.
   - Missing or moved ISO files trigger structured error dialogs rather than silent halts.

---

## 5. Verification Commands

### Native Compilation

```powershell
mingw32-make player
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
