# Native Cross-Platform Player UI Architecture

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
Automated LLE preparation: local KIRK decryption & PRX validation
   ↓
Truthful progress reporting with stage names, item counts & times
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
│ │ Title Registry │ │  ISO Inspector │ │ Prep Engine  │ │
│ └───────┬────────┘ └────────┬───────┘ └───────┬──────┘ │
│ ┌───────▼────────┐ ┌────────▼───────┐ ┌───────▼──────┐ │
│ │ Progress Engine│ │  Task Cancel   │ │ Launch Plan  │ │
│ └────────────────┘ └────────────────┘ └──────────────┘ │
└───────────────────────────────┬────────────────────────┘
                                │ Filesystem / Process
┌───────────────────────────────▼────────────────────────┐
│                   OPERATING SYSTEM                     │
│   Windows (%LOCALAPPDATA%) · Linux (XDG) · macOS       │
└────────────────────────────────────────────────────────┘
```

The presentation layer contains **zero business logic**. All title identification, ISO inspection, archive extraction, checksum verification, and session manifest creation live inside the portable core (`nk_core`).

---

## 4. First-Run & Onboarding Flow

1. **Immediate Window Appearance**: The SDL3 window initializes and presents the UI in under 100 milliseconds.
2. **Game Library View**: Displays supported games. If no game is configured, the prominent hero card invites the player: *"Select your legally obtained PSP ISO"*.
3. **Native File Selection**: Clicking *"Add Game"* invokes the native platform file picker (`IFileDialog` on Windows, native portal/Zenity on Linux).
4. **Instant ISO Qualification**: The inspector reads the ISO9660 PVD and `PARAM.SFO` in memory, extracting `DISC_ID` (e.g. `UCUS98701`), Title, and Region.
5. **Transactional Preparation**:
   - Staging directory created under `.staging_<disc_id>_<timestamp>/`.
   - Untouched `EBOOT.BIN` and PRX files decrypted locally via clean-room KIRK routines.
   - ELF envelopes and section headers validated.
   - Checksums and manifests validated.
   - Atomic directory promotion to permanent game path.
6. **One-Click Play**: The hero button changes to glowing emerald *"PLAY GAME"*.

---

## 5. Progress, Cancellation & Error Architecture

### Progress Contracts

The core emits structured progress events at every milestone:

- **Stage**: `INSPECTING_ISO`, `EXTRACTING_CONTAINERS`, `DECRYPTING_MODULES`, `VALIDATING_ELFS`, `PREPARING_VFS`, `READY`.
- **Metrics**: Completed count, total count, elapsed time in milliseconds, and current processing item.
- **Truthfulness**: Progress never displays fabricated percentages; operations with indeterminate totals show smooth activity pulses.

### Transactional Integrity

- All extraction and generation occur in temporary staging directories.
- If the user clicks **Cancel** or the process is interrupted, the staging directory is cleaned up immediately. Previous working installations remain untouched.

### User-Friendly Error Experience

Instead of exposing raw exception stack traces or compiler lines, errors provide a stable code, human explanation, and actionable remediation:

- `ISO_UNSUPPORTED_TITLE`: *"This PSP title (DISC_ID) is not currently supported by Nakagawa."*
- `ISO_UNREADABLE`: *"The selected file could not be read. Ensure the image is a valid ISO9660 disc."*
- `MISSING_FIRMWARE_FONT`: *"Authentic typography requires jpn0.pgf in %LOCALAPPDATA%/nakagawa/system/font/."*

---

## 6. Controller & High-DPI Navigation

- **Gamepad First**: Full navigation via D-Pad, Left Stick, Cross (A) to select, Circle (B) to go back.
- **High-DPI Scaling**: Vector/SDF typography automatically scales with monitor DPI (100%, 150%, 200%).
- **Theme**: Modern dark court palette with crisp contrast, soft glass panels, and clear visual hierarchy.
