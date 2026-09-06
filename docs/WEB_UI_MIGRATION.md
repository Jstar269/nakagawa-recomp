# Web UI Inventory, Evaluation, and Native Migration Plan

## 1. Inventory of Current Web UI Components

The existing prototype UI is located under [`interface/`](file:///C:/nk/worktrees/ux-investigation/interface/):

| Subsystem | Location | Technologies | Function / Scope |
| :--- | :--- | :--- | :--- |
| **Frontend Shell** | `src/app/page.tsx`, `layout.tsx` | Next.js 16, React 19, Tailwind CSS 4 | Studio and Player container with topbar, sidebar, and dynamic panel routing |
| **Player Hero Launcher** | `components/studio/launcher-panel.tsx` | React 19, Lucide icons | Streamlined hero launcher, quick settings, and 1-click ISO dropzone |
| **ISO Loader** | `components/studio/iso-loader.tsx` | Client-side ISO9660 reader (`lib/recompiler/iso.ts`) | Inspects ISO9660 PVD/directory in-browser (read-only, no disk write) |
| **Pipeline Panel** | `components/studio/pipeline-panel.tsx` | React, SSE | Triggers `tools/recompile.ps1` / `hst_manager.ps1` (`BuildFull`, `BuildFast`) |
| **Process Manager** | `lib/recompiler/manager-process.ts` | Node.js `child_process.spawn`, `taskkill.exe` | Background process registry, log ring buffer, clean process cancellation |
| **Process API** | `app/api/recompiler/manager/route.ts` | Next.js API route | Streams stdout/stderr over SSE; validates `ManagerLaunchRequest` |
| **Preflight Doctor**| `app/api/recompiler/doctor/route.ts` | Node.js, `tools/hst_doctor.py` | Runs workspace diagnostics and returns structured JSON report |
| **Inspector Panel** | `components/studio/inspector-panel.tsx` | React, JSON editor | Memory watchpoint management and interactive memory inspector |
| **VRAM Viewer** | `components/studio/vram-panel.tsx` | React canvas | Renders PSP VRAM buffers, GE textures, and framebuffers |
| **Benchmarks Panel**| `components/studio/benchmarks-panel.tsx` | React, SVG visualizer | Visualizes generated-PC call counts, block hotspots, and performance data |
| **Shader Regression**| `components/studio/shader-panel.tsx` | Node.js, SPIR-V probes | Compares GPU shader pipelines against software reference rasters |
| **Fuzz Lab** | `components/studio/fuzz-panel.tsx` | Next.js API, Python harness | Triggers instruction fuzzing and cosimulation suites |
| **Terminal Drawer** | `components/studio/terminal-drawer.tsx` | React, ANSI parser | Live streaming terminal drawer for build and engine logs |

---

## 2. Process Model & Dependencies

* **Process Architecture:**
  1. Developer opens terminal and runs `npm run dev`.
  2. Node.js process starts and binds a local HTTP server to `127.0.0.1:3000`.
  3. External web browser connects over HTTP.
  4. Server spawns `powershell.exe` / `pwsh.exe` subprocesses for manager tasks.
* **Dependency Footprint:**
  - Node.js 24 LTS runtime + npm 11.
  - ~378 KB `package-lock.json` with hundreds of transitive npm packages.
  - Generates ~400 MB of `node_modules`.

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

The web UI was created to give developers deep visibility into the recompiler. The following capabilities must **not be lost**; they are migrated to the native player UI (under an accessible **"Studio Tools"** menu) and headless CLI tools:

1. **Preflight Diagnostics:** Validating Vulkan drivers, system specs, and game inputs.
2. **Recompiler Task Execution:** Live build/compile triggers with streaming log feedback.
3. **Performance Profiling:** Telemetry graphs (FPS, vblank rate, CPU/GPU frame times).
4. **VRAM & Texture Inspection:** Visualizing active GE textures and framebuffer targets.
5. **Gamepad Calibration & Testing:** Live visual controller input verification.
6. **Visual Regression Comparator:** Validating GPU render parity against software ground truth.
7. **LLE State Inspector:** Inspecting active guest threads, semaphores, loaded PRX modules, and memory partitions.

---

## 5. Phased Migration Plan

```mermaid
graph TD
    subgraph Phase 1: Core Decoupling
        P1A[Create portable nk_core API] --> P1B[Create Headless CLI nk_cli.py]
        P1B --> P1C[Wire Launcher Mode into Interface]
    end

    subgraph Phase 2: Native Shell Implementation
        P2A[Implement SDL3 Native Launcher Window]
        P2B[Add Native OS File Picker]
        P2C[Wire Transactional Prep Engine]
        P2D[Integrate In-Engine Pause Overlay]
    end

    subgraph Phase 3: Parity & Retirement
        P3A[Port Developer Diagnostic Views to Native UI]
        P3B[Verify Feature Parity on Windows, Linux, macOS]
        P3C[Retire Node.js HTTP Server & Interface Directory]
    end

    Phase 1 --> Phase 2
    Phase 2 --> Phase 3
```

### Phase 1: Decoupling & Portable Core (Completed in Current Worktree)
* Business logic (ISO inspection, title registry, transactional preparation, launch planning) has been fully decoupled from the web UI and implemented in the standalone, portable `tools/nk_core/` library and `tools/nk_cli.py`.
* A dedicated **Player Mode** (`launcher-panel.tsx`) with a top-level mode toggle was added to `interface/` to prototype the streamlined end-user flow.

### Phase 2: Native SDL3 Launcher & In-Engine Overlay
* Build the native SDL3 Game Library window with `IFileDialog` / native platform file picker.
* Wire `nk_core` into the native launcher.
* Embed an in-engine overlay into the SDL3 Vulkan swapchain for the in-game pause menu.

### Phase 3: Feature Parity & Complete Retirement of Web Components
* Once the native SDL3 launcher implements game selection, preparation, settings, and developer diagnostics, completely retire and remove the `interface/` directory and Node.js dependencies from the repository.
