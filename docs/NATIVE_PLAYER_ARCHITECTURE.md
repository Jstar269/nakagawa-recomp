# Native Cross-Platform Player UI Architecture

> **Status: CURRENT — maintained architecture record.** The native SDL3 player,
> launch/session core, bounded ISO/XB staging pipeline, and the library's
> build-package / validate / launch route exist in the public source
> ([#465](https://github.com/Jstar269/nakagawa-recomp/pull/465),
> [#487](https://github.com/Jstar269/nakagawa-recomp/pull/487)). Module
> decryption, retail-disc hash validation, progress reporting for every
> preparation route, and end-user productization without the developer
> toolchain remain unbuilt and are marked as targets below.
>
> **Current build boundary:** The native player connects a bounded ISO/XB
> staging pipeline for the first-time setup wizard. It records the extracted
> asset/audio/visual/layout counts, promotes the transaction, registers the
> title in the library, and resolves the promoted `EBOOT.BIN`, `xbdata`, and
> per-title Memory Stick root for launch preparation. Plain staged ELF32/MIPS
> images receive program-header, entry, load-range, and BSS geometry checks;
> retail `~PSP` containers are recognized but their encrypted inner ELF remains
> a separate decryption capability. The library card drives the same route
> without scripts: **BUILD PACKAGE** runs `tools/nk_cli.py build-package` with
> live stage progress, and **PLAY NOW** starts a validated package (or a
> non-experimental catalog title's developer runtime,
> [#483](https://github.com/Jstar269/nakagawa-recomp/pull/483)). The legacy
> `VIEW_PREPARING` screen is still an honest unavailable state, reachable today
> only as a captured view.

## 1. Executive Vision: The Honest "Program + ISO" Contract

The ultimate productization goal of Nakagawa Recomp is to deliver a seamless, modern, zero-terminal gaming experience without sacrificing low-level emulation (LLE) correctness or provenance integrity:

```text
Download / Install Nakagawa Recomp
   ↓
Double-click standalone executable (nakagawa.exe)
   ↓
Select a PSP game ISO you own
   ↓
Nakagawa identifies supported title from PARAM.SFO
   ↓
Native ISO/XB staging stage (connected in the setup wizard)
   ↓
Reactive bounded progress reporting (wizard Step 3)
   ↓
Launch into genuine recompiled guest execution with Vulkan & SDL3
   ↓
Next time: Double-click and PLAY
```

### 1.1 The Common-User Principle

A gamer should only ever have to understand:

1. *"I own this PSP game."*
2. *"Here is my ISO file."*
3. *"Play."*

The player must never be forced to install MSYS2, Python, PowerShell 7, Git, or C compilers; nor should they be asked to manually extract archives or run developer scripts.

### 1.2 Truthful Prerequisite Discipline

Where genuine LLE fidelity requires resources not found on the game disc (specifically, Sony firmware PGF fonts from `flash0:/font/`), the UI must **never fake success or silently substitute inferior host approximations**.
Instead, the player UI honestly informs the user:
> *"Authentic font rendering requires PSP firmware font assets. Place your `jpn0.pgf` in `%LOCALAPPDATA%\Nakagawa\fonts\` or click 'Continue with Synthetic Preview Font'."*

---

## 2. Evaluation of Native UI Technologies

The native player uses the existing SDL3 and Vulkan runtime to render its in-window controls. Candidate desktop UI frameworks were evaluated across sixteen engineering criteria:

| Criterion | SDL3 + In-Engine UI (Chosen) | Qt 6 / QML | Slint (C++/Rust) | Webview2 / Tauri v2 |
| :--- | :--- | :--- | :--- | :--- |
| **Existing Integration** | **Native** (Runtime already uses SDL3 + Vulkan) | None (Requires new binding layer) | None | Separate webview process |
| **Executable Footprint** | **~2 MB overhead** (Single `.exe`) | 50–100 MB of shared DLLs | ~15 MB | ~10–15 MB |
| **Runtime Dependencies** | **None** (Self-contained) | Extensive shared libraries | Rust runtime | OS Webview (WebKitGTK on Linux) |
| **Couch / Controller Navigation** | **First-Class** (SDL3 Gamepad API) | Complex focus management | Moderate | Web Gamepad API limits |
| **Unified Game Window** | **Target** (launcher and title currently run as separate processes) | No (Separate launcher & render window) | No | No |
| **In-Game Overlay Capable** | **Target** (overlay parity is not built) | No | No | No |
| **Linux / Steam Deck Parity** | **Target** (SDL3/Vulkan; platform acceptance not run) | Good | Good | WebKitGTK packaging fragmentation |
| **macOS (Metal/MoltenVK)** | **Target** (framework path; project acceptance not run) | Framework capability; project acceptance not run | Framework capability; project acceptance not run | Framework capability; project acceptance not run |
| **Build-System Burden** | **Target: native Makefile path** (developer toolchain still required today) | Heavy (CMake + MOC + UIC) | Heavy (Requires Cargo / Rustc) | Heavy (Node + Rust toolchains) |
| **Startup Latency** | **Target estimate; not acceptance evidence** | ~300–600 ms | ~100 ms | ~400–800 ms |

### Selection Rationale

**SDL3 with an in-engine retained/immediate UI layer** was decisively chosen because:

1. **Unified Installation Experience (Target)**: The product goal is one install with a coherent launcher and game experience. The current source starts a prepared title as a separate child through `nk_launch_start`; it does not make the launcher and game a single executable.
2. **Future Outside/Inside Presence**: A shared UI system is intended to drive the launcher outside the game and an eventual in-game settings pause overlay (`F1` / Gamepad `Guide`). In-game overlay parity is not implemented in the current native slice.
3. **No Web / Server Overhead**: Eliminates Node.js, localhost HTTP listeners, port collisions, browser sandbox restrictions, and security boundary headaches.

### Layout library and language decision (2026-10-21)

The owner decided on 2026-10-21 that the native player's UI is written in C on SDL3 and Vulkan, and that its layout uses **Clay**, a single-header C layout library ([github.com/nicbarker/clay](https://github.com/nicbarker/clay)). Upstream v0.14's `LICENSE.md` identifies the license as zlib/libpng (`Zlib`); the pinned header, license, and provenance are recorded under `third_party/clay/`. The decision has three parts:

1. **One UI system (target)**: the launcher, the in-game overlay and the settings screens are planned to share one UI system. It is drawn at the window's real pixel size (the UI half of [#1129](https://github.com/Jstar269/nakagawa-recomp/issues/1129)) and is controller-first.
2. **Rust is not adopted for the UI**: Rust may be used later, behind a C ABI, only for isolated parsers of untrusted input (ISO, SFO, PGF), and only if fuzzing justifies it.
3. **Reasons**: one interface serves both the overlay and the launcher; SDL3 already supports Android, a platform the roadmap names; and a Rust UI framework would add hundreds of crates to track under the project's provenance rules, while Clay is one C header.

Not done yet:

- **Clay is vendored at v0.14 and its new paths have trusted provenance admission.** `src/ui/` (nk_ui) wraps it with the theme, text, focus, input and SDL_Renderer layers.
- **The nk_ui launcher is the default.** It draws the Library, Title detail, About, the setup wizard, package build progress, the build-tool download and the error card with the design system in section 8. Settings, the game-found view and the controller screens are still drawn by `src/player/ui_renderer.c`, letterboxed inside it. `--ui=legacy` (or `NK_UI=legacy`) keeps the existing renderer for one release; the player also falls back to it when the UI fonts cannot load.
- Settings, first run, error dialogs and the in-game overlay on nk_ui are targets, specified in section 8.

---

## 3. Architecture & Separation of Concerns

The diagram below is the target separation. The implemented public slice currently reaches
title lookup, bounded ISO inspection and ISO/XB staging, library package build and
validation, launch-session validation, and child-process lifecycle. Nakagawa does not
decrypt games (built-in decryption was removed, #295); retail-disc hash
validation and overlay parity remain unbuilt.

```text
┌────────────────────────────────────────────────────────┐
│               PRESENTATION LAYER                       │
│  ┌─────────────────────────┐ ┌──────────────────────┐  │
│  │   SDL3 Native Window    │ │ In-Game Pause Overlay│  │
│  │ (Library, Setup, Config)│ │ (Shaders, Res, Saves)│  │
│  └────────────┬────────────┘ └──────────┬───────────┘  │
└───────────────┼─────────────────────────┼──────────────┘
                │ Native Events           │ Render Hooks
┌───────────────▼─────────────────────────▼──────────────┐
│                  PORTABLE CORE API                     │
│ ┌────────────────┐ ┌────────────────┐ ┌──────────────┐ │
│ │ Title Registry │ │  ISO Inspector │ │ Prep Contract│ │
│ └───────┬────────┘ └────────┬───────┘ └───────┬──────┘ │
│ ┌───────▼────────┐ ┌────────▼───────┐ ┌───────▼──────┐ │
│ │ Progress Contract│ │ Task Cancel   │ │ Launch Plan  │ │
│ └────────────────┘ └────────────────┘ └──────────────┘ │
└───────────────────────────────┬────────────────────────┘
                                │ Filesystem / Process
┌───────────────────────────────▼────────────────────────┐
│                   OPERATING SYSTEM                     │
│   Windows (LOCALAPPDATA / FOLDERID_LocalAppData / APPDATA) │
│   Linux (XDG) · macOS (Application Support)            │
└────────────────────────────────────────────────────────┘
```

The presentation layer contains **zero business logic**. Title identification,
ISO inspection, and the bounded setup transaction are wired through the native
player/core boundary; the renderer only presents the resulting state. The
runtime consumes the promoted `xbdata` tree through its existing `SR_DATAROOT`
asset index, while proprietary audio/GIM decoding remains in the runtime/tool
capability layers rather than being reimplemented in the launcher.

---

## 4. First-Run & Onboarding Flow

The flow below is the complete productization target, not a description of the current
executable. Today the player opens a native SDL file picker, inspects a selected ISO, runs
bounded ISO/XB staging for supported inputs, builds a runtime package from the library with
live stage progress, and launches a validated package ([#487](https://github.com/Jstar269/nakagawa-recomp/pull/487)). The player and CLI use decrypted
files from your own copy and do not decrypt games ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)); retail-disc hash validation
and one-click play from an arbitrary ISO without the developer toolchain remain unbuilt
([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).

1. **Immediate Window Appearance**: The SDL3 window initializes and presents the UI in under 100 milliseconds.
2. **Game Library View**: Displays supported games. If no game is configured, the prominent hero card invites the player: *"Select a PSP ISO you own"*.
3. **Native File Selection**: Clicking *"Add Game"* invokes SDL3's native file-dialog API; the platform backend supplies the operating-system picker behavior.
4. **Instant ISO Qualification**: The inspector reads the ISO9660 PVD and `PARAM.SFO` in memory, extracting `DISC_ID` (e.g. `UCUS98701`), Title, and Region.
5. **Transactional Preparation**: one staging transaction (`player_stage_title`
   in `src/player/setup_staging.c`) serves every route that sets up a disc's files:
   the wizard's worker (also started by the supported-title card's ADD TO LIBRARY
   when the title takes its data from the disc), `--stage-only`, `--launch-now`
   when the files are not in place yet, and `nk_cli prepare` and `nk_cli bringup`
   for discs whose data ships in archives (they run the player's `--stage-only`).
   - A per-title lock (`games/.staging_<disc_id>.lock`) refuses a second staging of
     the same title with `[STAGE_BUSY]`; the operating system releases it when its
     holder exits.
   - Whatever an interrupted run left behind (an unpromoted `.staging_<disc_id>/`
     tree, or a replaced tree not yet removed) is cleared and never promoted.
   - An existing `games/<disc_id>/` tree is reused when its staging record
     (`EXTRACTED/staging-record.txt`: disc ID, disc version, image size, data root,
     loose-content roots, asset counts) matches and every required folder is
     present; an older or incomplete tree is replaced only after its replacement
     is complete.
   - Staging directory created under `.staging_<disc_id>/` in the per-user data
     root (`--user-data-root` overrides it for the whole player process).
   - `EBOOT.BIN` and the title's configured loose-content roots below
     `PSP_GAME/USRDIR` are copied by the native ISO reader. A data root that lies
     under those roots (for example `xbdata`) must be present, or the disc is
     refused with `[STAGE_DATA_FOLDER_MISSING]` before anything is promoted.
   - Native project-authored XB parsing validates FST spans, names, bounds, and nested LZS/Huffman payloads before writing members under the runtime-compatible `<archive>.xb.d/` directories.
   - If present, named plain support PRXs that the user has already placed in a recognized local folder are copied into `EXTRACTED/decrypted/`. This staging step does not decrypt files, and neither does any other part of the player ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).
   - Atomic directory promotion occurs only after the worker completes; failed/cancelled staging is discarded.
   - The promoted root is registered with a persistent asset census and becomes
     the launch session's preferred `SR_DATAROOT`/`SR_MEMSTICK` source.
   - An encrypted `~PSP`/`~SCE` executable without a user-supplied `EBOOT.elf` stops as
     `ENCRYPTED_EXECUTABLE` ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). Without a supported plain or decrypted
     executable, a missing recompiled child is reported instead of fabricated.
6. **One-Click Play**: The hero card exposes *"PLAY NOW"* when a validating
   package is present or, for a non-experimental catalog title, when the
   launcher resolves that title's developer runtime ([#483](https://github.com/Jstar269/nakagawa-recomp/pull/483)).
   Otherwise it shows *"REBUILD PACKAGE"* for a stale package, *"BUILD
   PACKAGE"* for a missing one, a `PACKAGE INCOMPATIBLE` status for an
   incompatible one, `RUNTIME REQUIRED` for a staged entry with no runnable
   runtime, and `PREPARATION UNAVAILABLE` when nothing is staged. A package
   status worker that cannot start shows `PACKAGE CHECK FAILED` and offers
   `RETRY PACKAGE CHECK`; ordinary background rescans retry that failure only
   after a bounded backoff.

---

## 5. Progress, Cancellation & Error Architecture

### Progress Contracts

The native staging worker emits progress notifications through SDL user events; it does
not use a timer or poll the worker. For the connected bounded ISO/XB path, each progress
update carries only:

- **Metrics**: Percent, completed file count, total file count, and the current processing path.
- **Transport**: The SDL event identifies progress or completion and carries the worker
  context; the payload above is read from that context on the UI thread.
- **Completion**: The asset/audio/visual/layout census is recorded by the completed
  staging transaction, not emitted as per-event progress data.
- **Truthfulness**: Percentages are derived from the bounded ISO byte/file walk and XB
  entry completion. Stage labels such as `DECRYPTING_MODULES`, elapsed time, and richer
  preparation-route metrics are a future contract, not fields populated by this worker.

### Transactional Integrity

- The extraction path uses temporary staging directories and promotes only on
  complete success.
- Cancellation and decode failure remove the newly created staging tree while
  leaving previous installations untouched.

### User-Friendly Error Experience

Instead of exposing raw exception stack traces or compiler lines, errors provide a stable code, human explanation, and actionable remediation:

- `ISO_UNSUPPORTED_TITLE`: *"This PSP title (DISC_ID) is not currently supported by Nakagawa."*
- `ISO_UNREADABLE`: *"The selected file could not be read. Ensure the image is a valid ISO9660 disc."*
- `STAGED_EXECUTABLE_INVALID`: *"The staged executable does not match the selected title's ELF/load contract."*
- `LIBRARY_WRITE_FAILED`: *"The game was staged, but the library record could not be saved."*
- `MISSING_FIRMWARE_FONT`: *"Authentic typography requires jpn0.pgf in %LOCALAPPDATA%\Nakagawa\fonts\."*

---

## 6. Controller & High-DPI Navigation

- **Gamepad First**: D-Pad and mapped buttons provide navigation; Cross (A) selects and Circle (B) goes back. Left-stick navigation remains a target until axis events are handled.
- **High-DPI Scaling**: DPI-aware raster text uses density-specific SDL_ttf size buckets. Vector/SDF typography remains a target.
- **Theme**: Modern dark court palette with crisp contrast, soft glass panels, and clear visual hierarchy.

### 6.1 Controller Settings

The native player provides a dedicated Controller Settings screen (`VIEW_CONTROLLER_SETTINGS`), reachable from the main Settings menu via the "CONTROLLER SETTINGS" action:

- **Interactive Remapping**: Allows rebinding each of the 14 standard PSP digital controls (D-Pad, action buttons, L/R shoulders, Start, Select, Home, Hold) and analog stick axes. Activating a binding button begins a 5-second capture window; pressing any host gamepad button assigns the new mapping. Captures can be cancelled at any time with keyboard Escape.
- **Conflict Visibility**: When the same host button is mapped to multiple PSP controls, a prominent conflict warning banner identifies the conflicting controls without silently dropping or unbinding either one.
- **Deadzone & Trigger Calibration**: Dedicated steppers adjust stick inner deadzone (range 0–32766) and trigger threshold (range 0–32767) in bounded increments.
- **Live Input Monitor**: A real-time visualizer panel displays active host button presses, numerical stick coordinates, and an interactive 2D deadzone box so users can immediately observe calibration effects.
- **Safe Navigation & Defaults**: Keyboard Escape always backs out or cancels capture, preventing navigation lockouts. "RESET DEFAULTS" restores the baseline profile.
- **Per-Title Mapping Choice**: the screen states which mapping it is editing, and a "USE GLOBAL MAPPING" / "USE THIS GAME MAPPING" control switches scope for the selected library disc ([#520](https://github.com/Jstar269/nakagawa-recomp/issues/520)). A disc's own mapping is seeded from the global one, so the first edit in that scope changes only what the user changed; choosing the global mapping again drops that disc's entry. The library card carries the same choice as a "MAPPING: GLOBAL" / "MAPPING: THIS GAME" button.
- **Unified Atomic Persistence**: "SAVE PROFILE" atomically writes the whole profile document (the global mapping plus the per-disc entries, profile schema 2) via a temporary file and rename to `<config>/input_profile.json`, shared with the runtime. An interrupted write leaves the previous file intact, and an invalid entry is refused before anything is written.
- **Guided Stick & Trigger Calibration**: A multi-stage calibration wizard ("CALIBRATE STICK & TRIGGERS") guides the user through:
  1. *Resting state*: sampling resting analog stick and trigger axes for 1000 ms to establish neutral/center points.
  2. *Extremes state*: interactive live sampling where the user presses each trigger fully and rotates analog sticks in circles to capture maximum physical excursions.
  3. *Review & Accept*: visual summary of captured resting and extreme values with Accept or Cancel actions.
  Values are stored in the unified `input_profile.json` (schema 2, schema 1 files still load) and applied during analog and trigger transform evaluations. Keyboard Escape or gamepad East button safely cancels at any point.
- **In the Works**: In-game pause overlay controller configuration remains in the works. Its guest-clock contract is tracked by [issue #1171](https://github.com/Jstar269/nakagawa-recomp/issues/1171) and the [Clocks section](ARCHITECTURE.md#clocks); the scheduler/audio half lands before the presenter and input-claim half. Focus loss and window minimization do not auto-pause in v0.0.1.

---

## 7. Open-Source UI Typography & Font Strategy

Selecting the right open-source fonts for nakagawa-recomp depends on whether you are styling the in-game arcade HUD, the native launcher/config menus, debug overlays, or Japanese text.

### In-Game UI & Arcade HUD (Rounded Sports Style)

- [M PLUS Rounded 1c](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) (SIL OFL): A rounded, friendly sports-style face for the project's own UI. Features rounded terminals, 7 weights, and full Japanese (Kanji/Kana) and Latin character sets.
- [Rubik](https://fonts.google.com/specimen/Rubik) (SIL OFL): A stout, slightly rounded sans-serif with subtle geometric corners. Great for match results, power gauges, and character selection cards.
- [Nunito](https://fonts.google.com/specimen/Nunito) (SIL OFL): A clean, rounded display sans that provides high legibility on lower rendering resolutions and handheld screens.
- [Teko](https://fonts.google.com/specimen/Teko) or [Bebas Neue](https://fonts.google.com/specimen/Bebas+Neue) (SIL OFL): Condensed, high-impact athletic display fonts suited for scoreboards, serve speed readouts, and game-point banners.

### Launcher, Overlay & Settings UI (Clay on SDL3, planned)

The player ships Chakra Petch, Geist, Geist Mono and Noto Sans JP (`assets/fonts/ui/`, section 8); the faces below
were the initial survey.

- [Inter](https://fonts.google.com/specimen/Inter) (SIL OFL): Optimized for screen interfaces and high pixel density. Remains sharp at small sizes (10px–14px) in configuration dialogs, graphics toggles, and controller binding menus.
- [Roboto Flex](https://fonts.google.com/specimen/Roboto+Flex) (SIL OFL): Flexible variable font that allows granular control over weight, width, and optical size across windowed or full-screen modes.

### Debug Overlays & Telemetry (FPS, Frame Times, Memory Viewers)

- [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (SIL OFL): Tall x-height and clear character spacing make it easy to scan rapid telemetry data and live recomp logs.
- [Intel One Mono](https://github.com/intel/intel-one-mono) (SIL OFL): Engineered for maximum distinction between similar glyphs (0/O, 1/l/I), making it ideal for memory address viewers, MIPS instruction dumps, and texture offset readouts.

### Japanese CJK Support

- [Kosugi Maru](https://fonts.google.com/specimen/Kosugi+Maru) (Apache 2.0): A rounded Japanese gothic typeface designed for interface readability, matching original PSP system fonts.
- [Zen Maru Gothic](https://fonts.google.com/specimen/Zen+Maru+Gothic) (SIL OFL): A smooth rounded Japanese font that scales well for translated dialogue boxes and Japanese audio/text toggles.

## 8. Player UI design system

The visual spec the player UI lanes (C5-C10) build against, with its tokens, components and the screen specs for
Settings, first run and error dialogs, is [`PLAYER_UI_DESIGN.md`](PLAYER_UI_DESIGN.md).

## 9. System check (planned)

Maintainer decision of 2026-10-23: a **System check** page verifies that this PC can run every part of Nakagawa. It
is a button in Settings, and the setup wizard's last step offers it ("Check this PC, about 2 minutes", skippable). Each
row shows a pass or a named failure with the fix, in plain words:

| Check | How |
| --- | --- |
| Graphics | Vulkan starts and the GPU and driver are named; the software fallback is reported when Vulkan does not start |
| Display and fonts | The UI fonts load and text draws at the display's scaling |
| Audio | The output device opens and a short tone plays, which the user confirms |
| Input | A controller is detected, and a "press each button" test fills in a picture |
| Storage | The data folders are writable and there is room for a typical package |
| Build tools | Python, the compiler and make are found with their versions, or the consented download is offered |
| Full pipeline | The project's own showcase demo is analysed, recompiled, built and launched for a few frames, proving the whole route works on this PC without any game |
| Online | Only with online mode on: the host answers and the update notice's signature verifies |
| Nakagawa Link | Later, only when the user has it: the PSP is found over USB |

It never claims that a particular game works: game compatibility stays on each title's card. It reuses the
prerequisite preflight, the showcase fixtures (project-made, no game data), the developer log from developer mode
([`ONLINE_FEATURES.md`](ONLINE_FEATURES.md#developer-mode)), and, once online mode exists, the report flow with its
preview ("Send this result").
