# Native Cross-Platform Player UI Architecture

> **Current build boundary:** The native player connects a bounded ISO/XB
> staging pipeline for the first-time setup wizard. It records the extracted
> asset/audio/visual/layout counts, promotes the transaction, registers the
> title in the library, and resolves the promoted `EBOOT.BIN`, `xbdata`, and
> per-title Memory Stick root for launch preparation. Plain staged ELF32/MIPS
> images receive program-header, entry, load-range, and BSS geometry checks;
> retail `~PSP` containers are recognized but their encrypted inner ELF remains
> a separate decryption capability. The native `VIEW_PREPARING` screen is still
> an honest unavailable state for legacy/library preparation requests.

## 1. Executive Vision: The Honest "Program + ISO" Contract

The ultimate productization goal of Nakagawa Recomp is to deliver a seamless, modern, zero-terminal gaming experience without sacrificing low-level emulation (LLE) correctness or provenance integrity:

```text
Download / Install Nakagawa Recomp
   ↓
Double-click standalone executable (nakagawa.exe)
   ↓
Select lawfully obtained PSP game ISO
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
> *"Authentic font rendering requires PSP firmware font assets. Place your `jpn0.pgf` in `%LOCALAPPDATA%/nakagawa/system/font/` or click 'Continue with Synthetic Preview Font'."*

---

## 2. Evaluation of Native UI Technologies

To replace the prototype localhost web dashboard (`interface/`), candidate desktop GUI frameworks were evaluated across sixteen engineering criteria:

| Criterion | SDL3 + In-Engine UI (Chosen) | Qt 6 / QML | Slint (C++/Rust) | Webview2 / Tauri v2 |
| :--- | :--- | :--- | :--- | :--- |
| **Existing Integration** | **Native** (Runtime already uses SDL3 + Vulkan) | None (Requires new binding layer) | None | Separate webview process |
| **Executable Footprint** | **~2 MB overhead** (Single `.exe`) | 50–100 MB of shared DLLs | ~15 MB | ~10–15 MB |
| **Runtime Dependencies** | **None** (Self-contained) | Extensive shared libraries | Rust runtime | OS Webview (WebKitGTK on Linux) |
| **Couch / Controller Navigation** | **First-Class** (SDL3 Gamepad API) | Complex focus management | Moderate | Web Gamepad API limits |
| **Unified Game Window** | **Yes** (Launcher & Game in 1 window) | No (Separate launcher & render window) | No | No |
| **In-Game Overlay Capable** | **Yes** (Draws over Vulkan swapchain) | No | No | No |
| **Linux / Steam Deck Parity** | **Flawless** (Standard SDL3/Vulkan) | Good | Good | WebKitGTK packaging fragmentation |
| **macOS (Metal/MoltenVK)** | **Supported** | Supported | Supported | Supported |
| **Build-System Burden** | **Zero** (Compiles cleanly with Makefile) | Heavy (CMake + MOC + UIC) | Heavy (Requires Cargo / Rustc) | Heavy (Node + Rust toolchains) |
| **Startup Latency** | **< 50 milliseconds** | ~300–600 ms | ~100 ms | ~400–800 ms |

### Selection Rationale

**SDL3 with an in-engine retained/immediate UI layer** was decisively chosen because:

1. **Single Unified Binary**: The exact same executable (`nakagawa.exe`) serves as the Game Library/Launcher upon startup and seamlessly transitions into the recompiled Vulkan game upon clicking "Play".
2. **Dual Outside/Inside Presence**: The same UI system drives the launcher outside the game and renders the in-game settings pause overlay (`F1` / Gamepad `Guide`) over the running game.
3. **No Web / Server Overhead**: Eliminates Node.js, localhost HTTP listeners, port collisions, browser sandbox restrictions, and security boundary headaches.

---

## 3. Architecture & Separation of Concerns

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
│   Windows (%LOCALAPPDATA%) · Linux (XDG) · macOS       │
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

1. **Immediate Window Appearance**: The SDL3 window initializes and presents the UI in under 100 milliseconds.
2. **Game Library View**: Displays supported games. If no game is configured, the prominent hero card invites the player: *"Select your legally obtained PSP ISO"*.
3. **Native File Selection**: Clicking *"Add Game"* invokes the native platform file picker (`IFileDialog` on Windows, native portal/Zenity on Linux).
4. **Instant ISO Qualification**: The inspector reads the ISO9660 PVD and `PARAM.SFO` in memory, extracting `DISC_ID` (e.g. `UCUS98701`), Title, and Region.
5. **Transactional Preparation**:
   - Staging directory created under `.staging_<disc_id>/` in local application data.
   - `EBOOT.BIN` and `PSP_GAME/USRDIR/xbdata` are copied by the native ISO reader.
   - Native clean-room XB parsing validates FST spans, names, bounds, and nested LZS/Huffman payloads before writing members under the runtime-compatible `<archive>.xb.d/` directories.
   - If present, named already-decrypted support PRXs are copied from standard PPSSPP dump locations into `EXTRACTED/decrypted/`.
   - Atomic directory promotion occurs only after the worker completes; failed/cancelled staging is discarded.
   - The promoted root is registered with a persistent asset census and becomes
     the launch session's preferred `SR_DATAROOT`/`SR_MEMSTICK` source.
   - KIRK decryption and validation of an encrypted `~PSP` inner ELF remain later
     capabilities; a missing recompiled child is reported instead of fabricated.
6. **One-Click Play**: The hero card exposes *"PLAY NOW"* when a runtime is
   available and *"LAUNCH PREPARED"* when assets are staged but runtime
   preparation is still pending.

---

## 5. Progress, Cancellation & Error Architecture

### Progress Contracts

The native staging worker emits structured progress events through SDL user
events; it does not use a timer or poll the worker:

- **Stage**: `INSPECTING_ISO`, `EXTRACTING_CONTAINERS`, `DECRYPTING_MODULES`, `VALIDATING_ELFS`, `PREPARING_VFS`, `READY`.
- **Metrics**: Completed count, total count, elapsed time in milliseconds, current processing item, and the final asset/audio/visual/layout census.
- **Truthfulness**: Percentages are derived from the bounded ISO byte/file
  walk and XB entry completion. Decryption and other unimplemented phases do
  not fabricate readiness.

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
- `MISSING_FIRMWARE_FONT`: *"Authentic typography requires jpn0.pgf in %LOCALAPPDATA%/nakagawa/system/font/."*

---

## 6. Controller & High-DPI Navigation

- **Gamepad First**: Full navigation via D-Pad, Left Stick, Cross (A) to select, Circle (B) to go back.
- **High-DPI Scaling**: Vector/SDF typography automatically scales with monitor DPI (100%, 150%, 200%).
- **Theme**: Modern dark court palette with crisp contrast, soft glass panels, and clear visual hierarchy.

---

## 7. Open-Source UI Typography & Font Strategy

Selecting the right open-source fonts for nakagawa-recomp depends on whether you are styling the in-game arcade HUD, the native launcher/config menus, debug overlays, or Japanese text.

### In-Game UI & Arcade HUD (Clap Hanz / Sports Aesthetic)

- [M PLUS Rounded 1c](https://fonts.google.com/specimen/M+PLUS+Rounded+1c) (SIL OFL): The closest stylistic match to the Hot Shots Tennis / Minna no Tennis aesthetic. Features rounded terminals, 7 weights, and full Japanese (Kanji/Kana) and Latin character sets.
- [Rubik](https://fonts.google.com/specimen/Rubik) (SIL OFL): A stout, slightly rounded sans-serif with subtle geometric corners. Great for match results, power gauges, and character selection cards.
- [Nunito](https://fonts.google.com/specimen/Nunito) (SIL OFL): A clean, rounded display sans that provides high legibility on lower rendering resolutions and handheld screens.
- [Teko](https://fonts.google.com/specimen/Teko) or [Bebas Neue](https://fonts.google.com/specimen/Bebas+Neue) (SIL OFL): Condensed, high-impact athletic display fonts suited for scoreboards, serve speed readouts, and game-point banners.

### Launcher & Settings UI (Dear ImGui / SDL3 Dialogs)

- [Inter](https://fonts.google.com/specimen/Inter) (SIL OFL): Optimized for screen interfaces and high pixel density. Remains sharp at small sizes (10px–14px) in configuration dialogs, graphics toggles, and controller binding menus.
- [Roboto Flex](https://fonts.google.com/specimen/Roboto+Flex) (SIL OFL): Flexible variable font that allows granular control over weight, width, and optical size across windowed or full-screen modes.

### Debug Overlays & Telemetry (FPS, Frame Times, Memory Viewers)

- [JetBrains Mono](https://fonts.google.com/specimen/JetBrains+Mono) (SIL OFL): Tall x-height and clear character spacing make it easy to scan rapid telemetry data and live recomp logs.
- [Intel One Mono](https://github.com/intel/intel-one-mono) (SIL OFL): Engineered for maximum distinction between similar glyphs (0/O, 1/l/I), making it ideal for memory address viewers, MIPS instruction dumps, and texture offset readouts.

### Japanese CJK Support

- [Kosugi Maru](https://fonts.google.com/specimen/Kosugi+Maru) (Apache 2.0): A rounded Japanese gothic typeface designed for interface readability, matching original PSP system fonts.
- [Zen Maru Gothic](https://fonts.google.com/specimen/Zen+Maru+Gothic) (SIL OFL): A smooth rounded Japanese font that scales well for translated dialogue boxes and Japanese audio/text toggles.
