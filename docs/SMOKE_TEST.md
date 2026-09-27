# Release Smoke Test: Native Player

Status: CURRENT. This document defines the canonical 13-step pass/fail release
smoke test for the Nakagawa Recomp native desktop player (`build/nakagawa_player.exe`
from a source checkout or `bin/nakagawa_player.exe` from the v0.0.1 package).
Every release candidate must pass this test on a clean Windows 11 system before
qualification.

Results may be submitted using the [Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).

## Prerequisites

- Windows 11 x64 with current Vulkan graphics drivers.
- The compiled native player binary (`mingw32-make player` -> `build/nakagawa_player.exe`), or the packaged `bin/nakagawa_player.exe` with its adjacent `SDL3.dll`.
- A source-owned synthetic fixture ISO or a showcase demo ISO ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).
- For **BUILD PACKAGE**, either the package's consent-based prerequisite fetcher and an internet connection, or Python, GCC, and GNU Make already on `PATH`. Step 8 starts with a clean `PATH` and exercises the consent path ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)).
- For Step 11, the compiled display smoke fixture (`mingw32-make display-smoke`).

---

## Numbered smoke test steps

### Step 1: Fresh isolated profile

1. Close any running instances of `nakagawa_player.exe`.
2. Use a disposable Windows profile or launch the player with `LOCALAPPDATA` set to a new temporary directory. Do not remove an existing `%LOCALAPPDATA%\Nakagawa` tree.
3. Verify the isolated `%LOCALAPPDATA%\Nakagawa` directory has no existing settings, library, cached titles, or logs.

**Expected result:** Pass if the isolated profile starts empty and the user's existing player data remains untouched.

### Step 2: Start the player

1. Launch `build/nakagawa_player.exe` from a source checkout, or `bin/nakagawa_player.exe` from the extracted release package, using PowerShell or File Explorer.
2. Observe window creation and initialization.

**Expected result:** Pass if the player window opens smoothly at 1280x720, titled "Nakagawa Recomp", with the dark background (`#0c0f12`), without crashing or emitting console assertions.

### Step 3: Empty-library first run

1. Inspect the initial view rendered on first launch with no games added.
2. Confirm the presence of the first-run call-to-action banner and navigation elements.

**Expected result:** Pass if the empty-library card shows "PSP GAME LIBRARY", the text "No games currently loaded in library.", and a **START SETUP WIZARD** button (the `O` shortcut adds an ISO directly).

### Step 4: Settings persist across a restart

1. Open Settings by clicking the **Settings** gear button or pressing `S`.
2. Change a configuration setting (for example, change **Resolution Scale** to `2x (960x544)`, set **FPS Cap** to `60 FPS`, or toggle **Reduce Motion**).
3. Close the Settings modal.
4. Exit the player (press `Esc` or close the window).
5. Relaunch `build/nakagawa_player.exe` and reopen Settings.

**Expected result:** Pass if `%LOCALAPPDATA%\Nakagawa\config\settings.json` was written atomically and your modified settings are accurately restored on relaunch.

### Step 5: Controller settings open

1. In the Settings view, click **Controller Settings** (or navigate to `VIEW_CONTROLLER_SETTINGS`).
2. Verify the layout of the controller configuration screen.

**Expected result:** Pass if the Controller Settings screen opens displaying connected gamepad status, the 14-button digital PSP mapping diagram plus analog stick, deadzone and trigger threshold calibration controls, and the live input monitor.

### Step 6: Add a disc image (synthetic or showcase image)

1. Return to the main library view.
2. Drag and drop a valid PSP `.iso` file into the window, or use **ADD ANOTHER ISO** (on an empty library, **START SETUP WIZARD** or the `O` shortcut) to pick it in the Windows file picker.
   - Use a synthetic test fixture image or a showcase demo image when available ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).

**Expected result:** Pass if the player transitions through `VIEW_INSPECTING`, parses `PSP_GAME/PARAM.SFO` directly in C without external tools, extracts the Title and Disc ID, and displays the corresponding game card.

### Step 7: Built-in decryption uses only the user's own key

1. Use a source-owned synthetic encrypted fixture and its test key if one is available. Do not use a retail ISO or retail key for this public smoke test.
2. Verify supported executable and module files decrypt to `%LOCALAPPDATA%\Nakagawa\data\titles\<DISC_ID>\decrypted` only when that local key is supplied.
3. If the source-owned showcase fixture is the only available image, record this step as **NOT RUN** for decryption: its plaintext executable does not exercise the encrypted path.

**Expected result:** Pass only for the source-owned encrypted fixture when the local key decrypts supported files and missing module key entries fail closed by name ([#548](https://github.com/Jstar269/nakagawa-recomp/pull/548), [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550)). No key is bundled. A plaintext showcase ISO is not evidence for retail decryption.

### Step 8: BUILD PACKAGE and prerequisite consent

1. With `PATH` limited to Windows system directories, select the source-owned showcase title and click **BUILD PACKAGE**.
2. Verify that one consent card lists every missing prerequisite, its version, source host, download size, licence, and total before anything is downloaded ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)).
3. Select **DOWNLOAD** and observe per-item progress. The player downloads only pinned manifest entries from their declared official HTTPS hosts, checks each declared size and SHA-256, installs into the isolated user profile, then resumes the package build.
4. Observe the 4-stage chips (Preflight, Extract, Compile, Package), elapsed time, output log, and successful package validation. Cancel one build with **CANCEL BUILD** (or `Esc`), verify return to the library, then rebuild and allow the package to finish.

**Expected result:** The clean-PATH route shows the complete consent list before network access; after consent, each pinned artifact verifies, installation reports progress, and BUILD PACKAGE resumes and validates. The cancel/rebuild pass also returns to the library and completes successfully. Any unsupported title or build prerequisite stops at a named card; unsupported generic title intake is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).

### Step 9: Play the generated package

1. From the showcase title card, click **PLAY** after the package validates.
2. Observe the Nakagawa game window and verify the source-owned guest reaches its expected screen and responds to its documented controls.

**Expected result:** Pass if the player launches the validated per-user runtime package, the guest presents frames, and unsupported features stop at named boundaries rather than hanging or reporting false success. This proves the synthetic route only; it does not qualify a commercial title ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).

### Step 10: Error card wording

1. Select an invalid or corrupted file (e.g. a non-ISO file renamed to `.iso`), or trigger an error state.
2. Observe the error dialog (`VIEW_ERROR`).

**Expected result:** Pass if the error dialog displays a red badge with the specific error code (e.g. `ISO_CORRUPT`), a clear human-readable title, the fail-closed boundary description naming what is missing and any tracking issue, the log file location (`LOG FILE: ...`), and a focused recovery button (e.g. "Try Another File" or "Return to Library") that restores navigation.

### Step 11: F1 overlay in a running package

1. Launch a running package (such as the source-owned display smoke fixture via `mingw32-make display-smoke-player` from a source checkout, or a showcase demo when available ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309))). The release archive does not contain generated fixture packages.
2. In the running game window, press the `F1` key.
3. Observe the performance HUD overlay.
4. Press `F1` again.

**Expected result:** Pass if pressing `F1` toggles an in-game HUD displaying live performance telemetry from `SR_PERF` (presented FPS, frame time in ms, VBlank rate, audio status), and pressing `F1` again dismisses the overlay without affecting game execution.

### Step 12: Quit

1. From the library or active window, press `Alt+F4`, click the window close button (`X`), or press `Esc` from the top-level view.
2. Verify process termination in Task Manager or PowerShell (`Get-Process nakagawa_player -ErrorAction SilentlyContinue`).

**Expected result:** Pass if the process exits cleanly with return code 0, all threads stop immediately, no zombie processes linger, and temporary locks are released.

### Step 13: Log and report locations to attach

1. Open the build log folder: `%LOCALAPPDATA%\Nakagawa\data\logs`.
2. If you started a build, collect:
   - the build progress log, `build_<DISC_ID>_progress.jsonl`;
   - the detailed build log, `build_<DISC_ID>.log`.
3. If you started the player from PowerShell, copy its console output. The player has no separate application log file yet.

**Expected result:** Pass if the build logs exist for any build you started. Before attaching them, check that they contain nothing private: no disc images, no game files, and no personal paths you don't want to share.
