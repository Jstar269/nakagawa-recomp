# Web UI Inventory, Evaluation, and Native Migration Record

> **Status: CURRENT — maintained migration record.** The interface/ tree remains a developer-facing web prototype. The native player slice has landed, including the first-time setup wizard with its bounded ISO/XB staging pipeline (PR #202), but the module-decryption backend, full diagnostic parity, and web retirement are not built. References to future native behavior below are targets, not current capability.
>
> **Boundary:** ISO/PARAM.SFO inspection in the current native player is bounded, and the wizard's staged extraction writes only into its own staging area; no runtime guest reads over archives, module decryption, or end-user AOT pipeline is connected. See NATIVE_PLAYER_ARCHITECTURE.md and ISO_ONLY_GAP_ANALYSIS.md.

## 1. Inventory of Current Web UI Components

The existing prototype UI is located under [`interface/`](../interface/). The player flow lives in the native player; the web prototype has no player hero launcher today. Rows below enumerate representative and routed source components in the current `interface/` tree; omission from this summary does not imply that a function is a target or unimplemented.

| Subsystem | Location | Technologies | Function / Scope |
| :--- | :--- | :--- | :--- |
| **Frontend Shell** | `src/app/page.tsx`, `src/app/layout.tsx` | Next.js 16, React 19, Tailwind CSS 4 | Studio container with topbar, sidebar, and panel routing |
| **Studio Panel Router** | `src/app/page.tsx` | React | Routes Graphics, Controllers, Limitations, Patches, Assets, Progress, Porting, Troubleshooting, Build Health, and related studio panels |
| **ISO Loader** | `components/studio/iso-loader.tsx` | Client-side ISO9660 reader (`lib/recompiler/iso.ts`) | Inspects ISO9660 PVD/directory in-browser (read-only, no disk write) |
| **Build Pipeline Panel** | `components/studio/build-panel.tsx` | React, SSE | Triggers root `nk_manager.ps1` manager actions (build/test flows) over the manager API |
| **Process Manager** | `lib/recompiler/manager-process.ts` | Node.js `child_process.spawn`, `taskkill.exe` | Background process registry, log ring buffer, clean process cancellation |
| **Process API** | `app/api/recompiler/manager/route.ts` | Next.js API route | Streams stdout/stderr over SSE; validates `ManagerLaunchRequest` |
| **Preflight Doctor** | `app/api/recompiler/doctor/route.ts` | Node.js, `tools/nk_doctor.py` | Runs workspace diagnostics and returns structured JSON report |
| **Internals Panel** | `components/studio/internals-panel.tsx` | React | Streams manager logs, parses crash-register snapshots, and renders static pipeline, subsystem, function, and thread-map views; no live semaphore, module, or memory-partition inspector |
| **VRAM Viewer** | `components/studio/vram-viewer.tsx` | React canvas | Fetches VRAM buffers over the debug console API and renders them in-browser |
| **Performance/Profiler Panels** | `components/studio/performance-panel.tsx`, `components/studio/profiler-panel.tsx` | React, SVG visualizer | Performance configuration plus generated-function/basic-block counts, durations, and watchpoint statistics; no FPS/frame-time telemetry view |
| **Visual Regression** | `components/studio/visual-regression-panel.tsx` | Node.js comparison routes | Compares captured snapshots against same-named golden frames; the route does not establish the golden files' rendering provenance |
| **Fuzz Lab** | `components/studio/test-lab-panel.tsx` | Next.js API, Python harness | Triggers the VFPU differential fuzzer through the manager API; cosimulation gates remain separate Make/CI targets |
| **Execution Console** | `components/studio/execution-console.tsx` | React | Runtime diagnostics console for process status, pause/resume, register and memory inspection, opt-in writes, and crash traces |

---

## 2. Process Model & Dependencies

* **Process Architecture:**
  1. Developer opens terminal and runs `npm run dev`.
  2. Node.js process starts and binds a local HTTP server to `127.0.0.1:3000`.
  3. External web browser connects over HTTP.
  4. Server spawns `powershell.exe` / `pwsh.exe` subprocesses for manager tasks.
* **Dependency Footprint:**
  * Node.js 24 LTS runtime + npm 11.
  * ~378 KB `package-lock.json` with hundreds of transitive npm packages.
  * Generates ~400 MB of `node_modules`.

---

## 3. Critical Web UI Problems for Common Users

1. **The Localhost & Port Collision Problem**:
   Requiring a local web server creates port conflicts if port 3000 is occupied, triggers firewall / antivirus alerts, and requires browser security permissions.
2. **Browser Security Sandbox Impasse**:
   Because the UI runs inside a web browser, it **cannot directly access the local filesystem**. When a user selects an ISO in `iso-loader.tsx`, the browser can only parse sectors in memory—it cannot write to `place_game_here/`, cannot trigger native extractions, and cannot open native Windows file dialogs.
3. **Severe Developer Tooling Requirement**:
   An end-user who just wants to play a game cannot be expected to install Node.js, configure npm, and launch a web server from a terminal.
4. **Disjoint Application Lifecycle**:
   The launcher is a web page, while the game is a native Vulkan window (`hst.exe`). There is no unified application experience or seamless controller navigation.

---

## 4. Functions That Must Be Preserved During Migration

The web UI was created to give developers deep visibility into the recompiler. The following capabilities are the **parity target** for a native "Studio Tools" surface and headless CLI tools; this document does not claim that parity is complete. Only the game-input inspection part of item 1 has a native counterpart today, via the wizard's bounded ISO staging:

1. **Preflight Diagnostics:** Validating Vulkan drivers, system specs, and game inputs.
2. **Recompiler Task Execution:** Live build/compile triggers with streaming log feedback.
3. **Performance Profiling:** Telemetry graphs (FPS, vblank rate, CPU/GPU frame times).
4. **VRAM & Texture Inspection:** Visualizing active GE textures and framebuffer targets.
5. **Gamepad Calibration & Testing:** Live visual controller input verification.
6. **Visual Regression Comparator:** Comparing captured output against a designated reference set.
7. **LLE State Inspector:** Inspecting active guest threads, semaphores, loaded PRX modules, and memory partitions.

---

## 5. Phased Migration Plan

The plan below records the target sequence. The native SDL3 launcher window, native file picker, and bounded ISO/XB staging landed in PR #202; the in-engine overlay, full diagnostic parity, and web retirement remain unbuilt.

```mermaid
graph TD
    subgraph Phase 1: Core Decoupling
        P1A[Create portable nk_core API] --> P1B[Create Headless CLI nk_cli.py]
        P1B --> P1C[Connect Core to Native Launcher and Setup Wizard (LANDED)]
    end

    subgraph Phase 2: Native Shell Implementation
        P2A[SDL3 Native Launcher Window (LANDED)]
        P2B[Native OS File Picker (LANDED)]
        P2C[Bounded Transactional ISO/XB Staging (LANDED)]
        P2D[Integrate In-Engine Pause Overlay (TARGET)]
    end

    subgraph Phase 3: Parity & Retirement
        P3A[Port Developer Diagnostic Views to Native UI]
        P3B[Verify Feature Parity on Windows, Linux, macOS]
        P3C[Retire Node.js HTTP Server & Interface Directory]
    end

    Phase 1 --> Phase 2
    Phase 2 --> Phase 3
```

### Phase 1: Decoupling & Portable Core (PARTIAL / SOURCE-ANCHORED)

* The current source owns a native player and core slice for ISO inspection,
  title-catalog lookup, launch planning, and isolated child-process startup.
* The streamlined end-user flow runs in the native player's setup wizard
  (`VIEW_SETUP_WIZARD` in `src/player/player_state.h`), not in the web
  prototype; the web tree has no player panel today.
* Transactional staging for supported inputs landed with the wizard (PR #202);
  module decryption and end-user AOT generation remain unconnected and must
  not be inferred from this phase label.

### Phase 2: Native SDL3 Launcher & In-Engine Overlay (PARTIAL / TARGET)

* The native SDL3 Game Library window, platform file picker, and bounded transactional
  ISO/XB staging are implemented for supported inputs.
* Wire the eventual complete preparation core into the native launcher; retail
  decryption, generated-runtime provisioning, and arbitrary-ISO one-click play remain
  targets.
* Embed an in-engine overlay into the SDL3 Vulkan swapchain for the in-game pause menu.

### Phase 3: Feature Parity & Complete Retirement of Web Components (UNBUILT / TARGET)

* Once the native SDL3 launcher implements game selection, preparation, settings,
  and developer diagnostics with evidence, evaluate retiring the `interface/`
  directory and Node.js dependencies. No removal is authorized by this plan alone.

---

## 6. Open-Source UI Typography & Font Strategy

Selecting the right open-source fonts for nakagawa-recomp depends on whether you are styling the in-game arcade HUD, the native launcher/config menus, debug overlays, or Japanese text:

### In-Game UI & Arcade HUD (Clap Hanz / Sports Aesthetic)

* [M PLUS Rounded 1c](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) (SIL OFL): The closest stylistic match to the Hot Shots Tennis / Minna no Tennis aesthetic. Features rounded terminals, 7 weights, and full Japanese (Kanji/Kana) and Latin character sets.
* [Rubik](https://fonts.google.com/specimen/Rubik) (SIL OFL): A stout, slightly rounded sans-serif with subtle geometric corners. Great for match results, power gauges, and character selection cards.
* [Nunito](https://fonts.google.com/specimen/Nunito) (SIL OFL): A clean, rounded display sans that provides high legibility on lower rendering resolutions and handheld screens.
* [Teko](https://fonts.google.com/specimen/Teko) or [Bebas Neue](https://fonts.google.com/specimen/Bebas+Neue) (SIL OFL): Condensed, high-impact athletic display fonts suited for scoreboards, serve speed readouts, and game-point banners.

### Launcher & Settings UI (Dear ImGui / SDL3 Dialogs)

* [Inter](https://fonts.google.com/specimen/Inter) (SIL OFL): Optimized for screen interfaces and high pixel density. Remains sharp at small sizes (10px–14px) in configuration dialogs, graphics toggles, and controller binding menus.
* [Roboto Flex](https://fonts.google.com/specimen/Roboto+Flex) (SIL OFL): Flexible variable font that allows granular control over weight, width, and optical size across windowed or full-screen modes.

### Debug Overlays & Telemetry (FPS, Frame Times, Memory Viewers)

* [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (SIL OFL): Tall x-height and clear character spacing make it easy to scan rapid telemetry data and live recomp logs.
* [Intel One Mono](https://github.com/intel/intel-one-mono) (SIL OFL): Engineered for maximum distinction between similar glyphs (0/O, 1/l/I), making it ideal for memory address viewers, MIPS instruction dumps, and texture offset readouts.

### Japanese CJK Support

* [Kosugi Maru](https://fonts.google.com/specimen/Kosugi+Maru) (Apache 2.0): A rounded Japanese gothic typeface designed for interface readability, matching original PSP system fonts.
* [Zen Maru Gothic](https://fonts.google.com/specimen/Zen+Maru+Gothic) (SIL OFL): A smooth rounded Japanese font that scales well for translated dialogue boxes and Japanese audio/text toggles.
