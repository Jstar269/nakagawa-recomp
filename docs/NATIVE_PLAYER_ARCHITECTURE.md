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
- **Only Library and Title detail are built on it**, behind the opt-in `--ui=next` flag (or `NK_UI=next`), drawn with the design system in section 8. The default UI is still `src/player/ui_renderer.c` until the cut-over lane.
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

> This section is the design spec the player UI lanes build against. It is meant to move to its own
> page, `docs/PLAYER_UI_DESIGN.md`, once that path is admitted; until then this is its home.

The visual spec for the native player's launcher UI (`--ui=next`, lanes C5-C10). It is the reference every
screen lane builds against: Library and Title detail (C5, built), Settings (C6), first run (C7) and error dialogs
(C8). The tokens and components it names live in code:

| Layer | File | Holds |
| --- | --- | --- |
| Tokens | `src/ui/nk_ui_theme.h`, `src/ui/nk_ui_theme.c` | colours, spacing, radii, borders, control sizes, layout grid, motion, font slots, type scale |
| Components | `src/ui/nk_ui_widgets.h`, `src/ui/nk_ui_widgets.c` | button, game card, add card, hero, status pill, tag, badge, eyebrow, card, divider, top bar, prompt bar, vector art |
| Core | `src/ui/nk_ui.h`, `src/ui/nk_ui.c` | layout (Clay), text runs, the focus ring, frame text storage, custom elements |
| Screens | `src/player/ui/scr_*.c` | compose components; never pick raw colours or sizes |

It follows the owner's interactive prototype (version 8) and the "Player" boards of the design canvas, both private.
Where this page and the code disagree, the code is current and this page is updated in the same change.

### Principles

- **One dark theme.** Ground `#0B0F14`; every surface steps up in colour, not with shadows.
- **Controller first.** Every action is reachable with a D-pad and two buttons, focus is always visible, and the
  prompt bar names the buttons that work on this screen. Nothing needs a pointer or hover.
- **Status is a word and a shape, then a colour:** `●` Ready, `▲` needs setup, `◆` experimental, `?` untested,
  `✕` unsupported. Colour never carries meaning alone.
- **Honest copy.** Words describe what the build does today. Compatibility words come from data
  (`docs/COMPATIBILITY.md`), never from layout code.
- **Bring your own game.** No game data, art, music or fonts ship with the player; placeholders say where the
  user's own disc art will appear. Nothing is downloaded or uploaded.
- **No console trade dress.** No PSP system icons, sounds or names, no XMB-style horizontal category bar. The
  waves, the palette and the seasonal arc are the project's own.
- **No key-file UI anywhere.** The player does not decrypt games (#1187). No screen, step, setting, dialog or path
  row asks for, finds or names a key file. A disc whose program is encrypted gets the plain explanation the host
  already supplies ("Nakagawa does not decrypt games; it uses decrypted files from your own copy").

### Tokens

#### Colour

| Token (`NkUiTheme`) | Value | Use |
| --- | --- | --- |
| `surface` | `#0B0F14` | window ground, add card |
| `surface_raised` | `#121821` | cards, panels, prompt bar |
| `surface_high` | `#18202B` | chips, focused secondary button, focused row |
| `line` | `#1C232D` | hairlines, card edges |
| `line_strong` | `#2E3846` | control and chip borders, focused card edge |
| `paper` | `#F5F7FA` | primary text |
| `muted` | `#9AA4B2` | secondary text (7.6:1 on the ground) |
| `dim` | `#6B7684` | disabled text and footnotes only (4.2:1, not for body text) |
| `accent_dark` | `#D7F24A` | focus ring, primary button, active tab rule, toggles, sliders, progress |
| `ink` | `#10151C` | text on an accent fill (14.6:1) |
| `accent_pressed` | `#B4D134` | pressed primary button |
| `pressed` / `pressed_danger` | `#222C39` / `#2A1A1C` | pressed secondary / danger button |
| `ready` | `#2DD4BF` | Ready; never the accent |
| `warning` | `#DBB024` | needs setup |
| `info` | `#6CB6FF` | experimental, untested, informational notes |
| `error` | `#FF7B72` | unsupported, failed, remove |
| `scrim` | ground at 78% | behind dialogs and the in-game overlay |
| `art_scrim` | ground at 72% | under a badge on title art |
| `art_label` | `#C9D6E3` | captions over title art |
| `placeholder` | `#3A4552` | art placeholder outline, inactive glyph dots |
| `capture` | `#1F2613` | a row waiting for a button press (C6 controls) |
| `accent_light` | `#6E9600` | accent on a light surface (reserved; the UI has none) |

#### Spacing, radii, borders

- Spacing is a 4 px scale: `NK_UI_SPACE_1` 4, `_2` 8, `_3` 12, `_4` 16, `_5` 20, `_6` 24, `_7` 32, `_8` 48, `_9` 64.
- Radii: chip 4 (pills, badges), control 8 (buttons, rows), tile 12 (cards, panels, hero), dialog 16, pill (full).
- Borders are 1 px hairlines (`NK_UI_BORDER_HAIRLINE`). Rounded borders draw straight runs plus antialiased quarter
  rings; every edge rounds to whole device pixels at every UI scale.
- Elevation is a colour step plus an edge: 0 page; 1 card (`surface_raised` + `line`); 2 control or focused row
  (`surface_high` + `line_strong`); 3 dialog (`surface_raised` + `line_strong` over the scrim).

#### Type

Fonts are SIL OFL 1.1 files in `assets/fonts/ui/`, registered in eight slots (`NK_UI_FONT_*`); every face falls
back to Geist and then Noto Sans JP. Sizes are logical px; tracking is CSS letter-spacing (it follows every
character).

| Role (`NK_UI_TYPE_*`) | Face | Size / line | Tracking | Use |
| --- | --- | --- | --- | --- |
| `DISPLAY_XL` | Chakra Petch Bold, caps | 40 / 44 | 0.04em | first-run headline |
| `DISPLAY` | Chakra Petch Bold, caps | 30 / 32 | 0.04em | screen names (LIBRARY, SETTINGS) |
| `WORDMARK` | Chakra Petch Bold, caps | 18 / 20 | 0.06em | NAKAGAWA RECOMP in the top bar |
| `TITLE_XL` | Geist SemiBold | 30 / 34 | 0 | a game's name on its page |
| `TITLE` | Geist SemiBold | 22 / 28 | 0 | dialog and panel titles |
| `HEADING` | Geist SemiBold | 17 / 24 | 0 | card headings |
| `TILE_TITLE` | Geist SemiBold | 15 / 20 | 0 | game card names (two lines, then an ellipsis) |
| `BODY_L` | Geist | 17 / 26 | 0 | lead paragraphs |
| `BODY` | Geist | 15 / 22 | 0 | running text |
| `BODY_S` | Geist | 14 / 21 | 0 | card paragraphs |
| `CAPTION` | Geist | 13 / 19 | 0 | hints, counts, notes |
| `FOOTNOTE` | Geist, `dim` | 12 / 16 | 0 | sources and fine print |
| `LABEL` | Geist Mono Medium, caps | 12 / 16 | 0.08em | buttons, prompts, tabs |
| `LABEL_L` | Geist Mono Medium, caps | 14 / 18 | 0.12em | the large primary button |
| `LABEL_S` | Geist Mono Medium, caps | 11 / 14 | 0.06em | chips, card meta rows |
| `EYEBROW` | Geist Mono Medium, caps, `muted` | 11 / 14 | 0.12em | section headers |
| `MICRO` | Geist Mono Medium, caps | 10 / 12 | 0.08em | badges over art |
| `DATA` | Geist Mono | 12 / 16 | 0 | disc IDs, figures, paths |

Labels, chips and eyebrows are drawn in caps (`nk_ui_caps`); the words stay sentence case in data.

#### Focus

- The ring is drawn by the core outside the focused element, so nothing moves when focus moves: 3 px
  `accent_dark`, 3 px gap, radius = the element's radius + 6.
- High-contrast focus (`NkUiPrefs.high_contrast_focus`): a 4 px white ring between 2 px black keylines.
- A focusable control calls `nk_ui_register_button` and, when focused, `nk_ui_focus_ring(ui, id, radius)`. A focused
  secondary button also fills with `surface_high`; a focused card's edge becomes `line_strong`.

#### Layout grid

| Measure | Value |
| --- | --- |
| Top bar / prompt bar | 64 / 56 px, separated from the body by a `line` hairline |
| Page gutter | 32 px; 16 px below 900 px wide (`NK_UI_GUTTER`, `NK_UI_GUTTER_NARROW`) |
| Body padding | 24 px top, 16 px bottom; 16 px between blocks |
| Library grid | columns = how many 236 px cards fit with 20 px gaps (1 to 6); rows = how many fit; 16 px row gap |
| Game card | art panel 0.53 x width, body 90 px |
| Title detail | 520 px aside (at most 45% of the body) and a growing column, 32 px apart; hero at 480:272 |
| Minimum target | 44 px high (`NK_UI_CONTROL_HEIGHT`); large 52, small 34 |
| Safe area | nothing closer than 16 px to a window edge |

Layout is logical px; the UI scale (Auto follows the display, or 100/125/150/200%) times the text size gives the
device density, and the logical width decides the columns. Clay clips at most ten containers per layout: shorten
long text with `nk_ui_text_clamp` instead of clipping it.

#### Motion

Tokens are in seconds (`NK_UI_MOTION_*`): fast 0.12 (focus, press), base 0.20 (toast, tab rule), slow 0.50 (art
crossfade); art appears after a 0.6 s rest, preview music after 1.1 s, fading in over 1.0 s and out over 0.45 s.
The waves step at 30 Hz. Reduce motion (the setting or the system's) makes every duration a cut and holds the
waves on one frame. Nothing ever needs motion to be understood.

#### Background waves

Four project-authored ribbons drift behind every launcher screen (`nk_ui_waves_element`, drawn first and outside
the layout), never in game. Their combined alpha is capped so every text colour keeps WCAG AA over the brightest
point. The SDL_Renderer backend draws them through the CPU reference at a third of the window size and keeps the
frame until the command changes; the Vulkan backend will use the shader.

### Components

| Component | Call | Spec |
| --- | --- | --- |
| Button | `nk_ui_button(ui, id, label, flags)` | label centred, `LABEL` caps; 44 px (`_LARGE` 52 with `LABEL_L`, `_SMALL` 34 with `LABEL_S`); fits its label (`_GROW` fills); primary = accent fill, ink text; secondary = clear fill, `line_strong` edge, paper text; danger = `error` text; disabled = `dim` text, `line` edge (primary keeps a `surface_high` fill) |
| Game card | `nk_ui_tile(ui, id, &spec)` | `surface_raised`, `line` edge, radius 12; art panel flush with the top (tint per title, badge top-left, picture or "ART FROM YOUR DISC" placeholder); name in `TILE_TITLE` (two lines); meta row: marker + status word in the status colour, disc ID right in `dim` |
| Add card | `nk_ui_add_tile` | ground fill, `line_strong` edge; 44 px outlined plus in accent, centred name and one-line note |
| Hero | `nk_ui_hero` | the title's art panel at 480:272 with a badge caption and the size note |
| Status pill | `nk_ui_status_pill` | 26 px, `surface_high`, `line_strong`, radius 4, `LABEL_S` in the status colour, marker first |
| Tag | `nk_ui_tag` | as the pill, `muted` text (YOUR DISC, SHOWCASE DEMO) |
| Badge | `nk_ui_badge` | 20 px on `art_scrim`, radius 4, `MICRO` |
| Eyebrow | `nk_ui_eyebrow` | `EYEBROW`; names a section or a card |
| Card | `nk_ui_card_begin` | `surface_raised`, `line`, radius 12, 16/20 px padding |
| Top bar | `nk_ui_top_bar` | N1 mark (28 px) and wordmark; section tabs in `LABEL` with a 3 px accent rule under the active one; input chip on the right (dot in `ready` for a controller) |
| Prompt bar | `nk_ui_prompt_bar` | `surface_raised`; hint in `CAPTION` `muted` on the left; glyph (24 px, current prompt style) + `LABEL` per action on the right, 18 px apart |
| Vector art | `nk_ui_shapes` | rects, circles and convex polygons in a view box, fitted and centred; used for the N1 mark, the showcase demos' pictures and icons |

Prompt glyphs come from `nk_ui_prompt_glyph`: neutral position dots, letters (A south or A east), or keycaps, as
the last input device reports. Only actions the screen handles appear in its prompt bar.

### Screens

#### Library (C5, built)

Top bar; `LIBRARY` in `DISPLAY` with the count in `CAPTION` beside it ("7 titles · 2 showcase demos"); a pager
(small PREVIOUS and NEXT buttons around "PAGE 1 OF 2") on the right of that row when the grid spans pages; the card
grid, ending with the add card; prompt bar "Bring your own game. Nothing is downloaded." with MOVE and OPEN. Empty
library: one centred 640 px card with a disc icon, `TITLE` "Your library is empty", a `BODY` line and the primary
ADD A PSP DISC IMAGE button.

#### Title detail (C5, built)

Aside: the hero, then a PACKAGE card (eyebrow, status pill, the status detail in `BODY_S` `muted`, and any input
note in an inset `info` box). Main column: eyebrow LIBRARY › TITLE, the name in `TITLE_XL`, the disc ID in
`DATA`, the status pill and tag, the large primary action with OPEN SAVE FOLDER beside it (both 52 px), BACK TO
LIBRARY below, and the COMPATIBILITY card (eyebrow, the word as a pill coloured by the word: Works `ready`,
Partially works and Experimental `info`, Boots to a named boundary `warning`, others `muted`; the detail; a
`FOOTNOTE` naming `docs/COMPATIBILITY.md`). Prompt bar: MOVE, SELECT, BACK.

#### Settings (C6)

- Top bar with SETTINGS active; `SETTINGS` in `DISPLAY`; a tab row (`LABEL`, 10/12 px padding, 3 px accent rule
  under the active tab, `line` underneath): DISPLAY, AUDIO, CONTROLS, PATHS AND STORAGE, ACCESSIBILITY, ABOUT.
  L and R (Q and E) switch tabs.
- Body: a column of rows and, from 1100 px wide, a 340 px help panel (`card`, a 20 px Geist SemiBold heading,
  `BODY_S` `muted` text) that explains the focused row and names its `settings.json` key.
- Row: at least 56 px, 8/16 px padding, radius 8; label in Geist Medium 15 with a 12 px `muted` description; the
  control on the right. A focused row fills with `surface_high` and the ring surrounds the row.
- Segmented control: 3 px padding in a ground-filled, `line_strong`, radius 9 track; options 30 px, 13 px `muted`;
  the selected one `surface_high` with paper text, and a 2 px inset accent edge while its row has focus.
  Left and right change it.
- Toggle: 46 x 26 track (off `line_strong`, on accent) with a 20 px knob (off `muted`, on ink) and ON / OFF in
  `LABEL_S` before it.
- Slider: 280 px track 6 px high (accent fill, `line_strong` rest), 18 px paper thumb, the value in mono 13 right.
- Paths rows: name, the path in `DATA` (middle-truncated), status line, small actions (OPEN, REMOVE). No key-file
  row.
- Controls tab: a 30 px mapping header in `EYEBROW` and 38 px mapping rows; a row waiting for a press fills with
  `capture` and the prompt bar shows only CANCEL.

#### First run (C7)

- Left rail 260 px (32/24 px padding, `line` edge on the right) listing the steps: Welcome, Disc image, System
  fonts, Build, Play. There is no key-file step. Each step is 44 px with a 26 px numbered circle (`line_strong`
  edge); current: accent fill and ink number, name in SemiBold; done: `ready` edge and number; failed: `error`.
  Below 900 px the rail hides and the step shows as an eyebrow.
- Content: 36/56 px padding; `DISPLAY_XL` headline; `BODY_L` `muted` lead, at most 720 px wide; choice cards
  (`card`, 18 px padding, tag chip, `HEADING`, `BODY_S`); check rows (38 px, `line` underneath: name 170 px,
  code in `DATA` 140 px, status word 120 px in its colour, detail `CAPTION`).
- Build: stage rows 50 px with a 26 px circle (running: accent fill), name 170 px, and a 6 px progress bar (accent
  on `line_strong`); a log box (`#080B0F`, `line`, radius 10, mono 12/1.6 `muted`).
- One primary action per step at the bottom left (CONTINUE, BUILD, PLAY), secondary actions after it.

#### Error dialogs (C8)

- Over the scrim, centred, 16 px from the edges; at most 660 px wide (760 for comparisons); `surface_raised`,
  `line_strong` edge, radius 16, 26/28/22 px padding, 14 px gap.
- Order: a status eyebrow in its colour with marker ("▲ CONTROLLER DISCONNECTED · GAME PAUSED"), a `TITLE` that says
  what happened in plain words, a `BODY` `muted` paragraph that says what to do, an optional detail box (ground
  fill, `line`, radius 10, `DATA` lines for paths or codes), then the button row: the recommended action first as
  primary, the way out last ("NOT NOW", "CANCEL"); a destructive action is a danger button and is never the
  default focus.
- Focus is trapped in the dialog and starts on the first button; BACK closes it with the safe choice.
- Cases to cover: unreadable or unsupported disc image (a store package is not a disc image), a different release of
  a known game (add as Experimental), build failure (stage named, log path shown), controller disconnected, quit to
  library. A disc whose program is encrypted gets the "does not decrypt games" explanation, never a request for a
  key file.

### Accessibility

- Text contrast is at least 4.5:1 for everything except `dim`, which is reserved for disabled controls and
  footnotes; over the brightest wave point muted and error text stay at 4.6:1.
- Focus ring 15.3:1 on the ground and at least 9.2:1 over the waves; high-contrast focus for any background.
- Targets are at least 44 px high; text size (90 to 150%) scales the whole layout through the density.
- Reduce motion holds the waves and makes transitions cuts.

### Building and checking a screen

1. Compose the screen from the components above; give every element a stable id (sub-parts use
   `nk_ui_child_id`).
2. `mingw32-make nk-ui-player-screens-selftest` compares the draw-list dumps in `fixtures/nk_ui/*.golden` and runs
   the layout checks (ring outside the focused card, centred labels, grid gaps). After an intended change, run
   `build/test_player_screens --write-goldens` and say in the pull request why each golden changed.
3. `mingw32-make nk-ui-player-screens-capture NK_UI_CAPTURE_DIR=<dir>` saves offscreen captures of each state at
   1280 x 800, 1920 x 1200 at 150% and 960 x 600, from synthetic fixture titles only. Compare them with the
   design boards before review.
