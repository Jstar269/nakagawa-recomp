# UI Baseline: Visual and Functional State Inventory

> **Publication status:** The 20 PNG captures referenced by this inventory are
> retained internally and are not published with this landing. This README is
> the published text inventory; the capture files are illustrative evidence
> only.

## 1. Overview & Capture Environment

This directory records the visual and functional baseline of Nakagawa Recomp's user interfaces prior to native migration.

- **Baseline Commit / HEAD:** `312d1d5d844adce1cd252fb8fd1b6ff6a1553a7b` (branch `ux-investigation`)
- **Host OS:** Windows 11 (x86_64)
- **Node.js Runtime:** v26.5.1
- **Next.js Version:** 16.3.0 (Turbopack)
- **Resolution Evaluated:** 1280×720 (Desktop standard)

---

## 2. Screenshot Inventory & Functional Assessment

| Screenshot Filename | View / Section | Launch Command / URL | Visible Functionality | Working Status | Preservation Requirement in Native UI |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `01_web_player_mode.png` | Player Mode Landing / Hero | `http://localhost:3000` | Game hero banner, "Start Setup Wizard", quick settings cards (Resolution, Controller, Save status), Workspace Preflight badge, Boot Health summary, Native binary status card | **WORKING (UI Level)** | Must be preserved as the default home screen when a title is loaded; clean layout, controller and resolution indicators |
| `02_web_studio_iso_loader.png` | ISO Loader & Inspector | `http://localhost:3000?section=iso` | In-browser ISO file dropzone, sector read status, disc ID / title display | **PARTIALLY WORKING** (In-browser sector parse only; cannot write files to disk) | Must be replaced by native OS file picker (`IFileDialog`) with direct disk reading and transactional preparation |
| `03_web_studio_pipeline_build.png` | Build & Recompile Pipeline | `http://localhost:3000?section=build` | Full Build / Fast Build buttons, asset copy switches, live SSE stdout/stderr terminal streaming | **WORKING** (Triggers PowerShell runner when inputs present) | Orchestration logic preserved in `nk_core`; terminal logs accessible under developer drawer |
| `04_web_studio_graphics_config.png` | Graphics Settings | `http://localhost:3000?section=graphics` | Resolution presets (Native 480×272, 2x Vita 960×544, 4x 1080p, 4K), aspect ratio, MSAA, CRT shader toggle, FPS cap slider | **WORKING** (Serializes to `.recompiler-config.json`) | Core player settings (Resolution, Fullscreen, VSync, FPS cap) must be native sliders/toggles |
| `05_web_studio_controllers_config.png` | Controller Calibration | `http://localhost:3000?section=controllers` | Visual PSP gamepad layout, button bindings, deadzone sliders, active gamepad detection | **WORKING** (Detects connected pads via Web Gamepad API) | Native SDL3 Gamepad polling, button mapping screen, visual button feedback |
| `06_web_studio_troubleshooting_doctor.png` | Preflight Doctor | `http://localhost:3000?section=troubleshooting` | Diagnostic check table: Windows version, Python version, MSYS2 toolchain, Vulkan SDK discovery, Game input readiness | **WORKING** (Runs `tools/hst_doctor.py` and renders report) | Diagnostic rules preserved in `tools/hst_doctor_checks.py`, callable from native diagnostics menu |
| `07_web_studio_profiler.png` | Performance Profiler | `http://localhost:3000?section=profiler` | Instruction execution frequency, hot block visualizer, flame charts, vblank FPS counters | **WORKING (Studio Mode)** | Preserved in optional developer mode / in-game overlay |
| `08_web_studio_internals.png` | Pipeline & Chunks Inspector | `http://localhost:3000?section=internals` | MIPS function count, generated chunk table, executable span viewer | **WORKING (Studio Mode)** | Preserved as developer diagnostic inspection data |

---

---

## 3. Key Observations & Inherent Web Limitations

1. **Localhost Server Friction:** Running these views requires `npx next start -p 3000`, which binds a network socket, requires Node.js, and risks port collisions (`EADDRINUSE`).
2. **Filesystem Isolation:** The ISO Loader (`02_web_studio_iso_loader.png`) explicitly warns the user that dragging an ISO into a web browser does not extract or prepare the game on the host machine.
3. **Disjoint Application Window:** The browser dashboard has no direct connection to the native Vulkan rendering window (`hst.exe`), creating a dual-window disjointed experience.
4. **Conclusion:** The layout, styling, and status indicators of the Player Mode (`01_web_player_mode.png`) are modern and effective, but must be rendered directly by a native desktop executable without Node.js or browser sandboxing.

---

## 4. Native Player UI Screenshot Matrix (SDL3 Standalone Executable)

The native player (`build/nakagawa_player.exe`) has been implemented in clean-room C using SDL3. It runs headlessly or interactively with zero web dependencies.

The complete visual matrix was captured via `python tools/capture_native_screenshots.py`:

| Screenshot Filename | View State | Resolution | Description & Visual Proof |
| :--- | :--- | :--- | :--- |
| `native_01_empty_library.png` | `VIEW_LIBRARY` (empty) | 1280×720 | Clean landing card with "+ ADD PSP GAME ISO" CTA, disc support indicators |
| `native_02_add_game.png` | `VIEW_LIBRARY` (empty) | 1280×720 | Add Game entrypoint |
| `native_03_iso_inspecting.png` | `VIEW_INSPECTING` | 1280×720 | PVD & PARAM.SFO inspection progress card with cancel button |
| `native_04_supported_game.png` | `VIEW_SUPPORTED_TITLE` | 1280×720 | Verified North American retail release recognized (`UCUS98701`), LLE font disclosure |
| `native_05_unsupported_game.png` | `VIEW_UNSUPPORTED_TITLE` | 1280×720 | Clear error indication for unregistered games with fail-closed return to library |
| `native_06_preparing.png` | `VIEW_PREPARING` | 1280×720 | Real transactional extraction progress bar, item counts (18450 / 56672), staging path |
| `native_07_ready_library.png` | `VIEW_LIBRARY` (ready) | 1280×720 | Hero game card with "PLAY NOW", specs rail (1080p Vulkan, DualSense, Memory Stick Slot 1), installed titles strip |
| `native_08_settings.png` | `VIEW_SETTINGS` | 1280×720 | Responsive settings: resolution scales (1x, 2x, 4x, 8x), FPS cap, audio stream, gamepad, savedata path |
| `native_09_missing_source_error.png` | `VIEW_ERROR` | 1280×720 | Missing ISO error dialog with structured recovery action |
| `native_10_library_1080p.png` | `VIEW_LIBRARY` (ready) | 1920×1080 | Full HD 1080p responsive layout verification |
| `native_11_settings_1080p.png` | `VIEW_SETTINGS` | 1920×1080 | Full HD 1080p settings modal scaling verification |
