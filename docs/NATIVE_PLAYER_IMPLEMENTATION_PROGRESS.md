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
   - `player_state.c` / `player_state.h`: Finite-state machine managing library games, inspection, preparation staging, settings, and structured recovery actions.
   - `ui_renderer.c` / `ui_renderer.h`: High-performance SDL3 renderer using the Dark Court palette, responsive card layouts, auto-scaled typography, and offscreen screenshot capabilities.
   - `main.c`: Interactive event loop with native file dialog (`SDL_ShowOpenFileDialog`), gamepad input, drag-and-drop ISO support, and headless test driver.
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
| Add Game | `native_02_add_game.png` | 1280×720 | Native file picker invocation state |
| ISO Inspecting | `native_03_iso_inspecting.png` | 1280×720 | Sector reading progress bar with cancel action |
| Recognized Title | `native_04_supported_game.png` | 1280×720 | `UCUS98701` verified; honest LLE font requirement note |
| Unsupported Title | `native_05_unsupported_game.png` | 1280×720 | Fail-closed boundary preventing unregistered execution |
| Preparation | `native_06_preparing.png` | 1280×720 | Transactional progress (18,450 / 56,672 items, 32.5%) |
| Ready Library | `native_07_ready_library.png` | 1280×720 | Hero game card with "PLAY NOW" & Quick Specs Rail |
| Settings Dialog | `native_08_settings.png` | 1280×720 | Resolution presets (1x..8x), 60 FPS, Audio, DualSense |
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
| 4 | Asset Extraction | External PowerShell script | Staged pipeline prototype in `nk_core` | **PARTIAL** (VFS integration pending) |
| 5 | Module Decryption | External toolchain | **NOT_IMPLEMENTED** (KIRK engine pending) | **NOT_IMPLEMENTED** (Requires pre-decrypted inputs) |
| 6 | Runtime Launch | Node child_process spawn | Native launch session & process spawn | **IN_PROGRESS** (Wiring real spawn) |
| 7 | Graphics Settings | Web localStorage | Native JSON configuration & CLI env | **PASS** (Verified serialization) |
| 8 | Gamepad Calibration | Web Gamepad API | SDL3 Gamepad Subsystem (DualSense/XInput) | **PASS** (Direct SDL3 controller API) |
| 9 | Preflight Checks | `hst_doctor.py` via HTTP | Integrated diagnostic rules | **PASS** (Portable rule engine) |
| 10 | Progress Feedback | Server-Sent Events (SSE) | Frame-accurate progress bar with item counts | **PASS** (Immediate-mode rendering) |
| 11 | Error Handling | HTML alert banner | Modal error dialog with recovery buttons | **PASS** (Structured recovery views) |
| 12 | Moved ISO Handling | Silent failure | Fail-closed detection + fallback lookup | **PASS** (Unit-tested recovery) |
| 13 | Multi-Title Support | Hardcoded HST strings | Data-driven manifest catalog | **IN_PROGRESS** (Unifying title contract) |

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
