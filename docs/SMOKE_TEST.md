# Release Smoke Test: Native Player

Status: CURRENT. This document defines the canonical 12-step pass/fail release
smoke test for the Nakagawa Recomp native desktop player (`build/nakagawa_player.exe`).
Every release candidate must pass this test on a clean Windows 11 system before
qualification.

Results may be submitted using the [Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).

## Prerequisites

- Windows 11 x64 with current Vulkan graphics drivers.
- The compiled native player binary (`mingw32-make player` -> `build/nakagawa_player.exe`).
- A synthetic fixture disc image or a showcase demo image when available ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).
- For Step 10, the compiled display smoke fixture (`mingw32-make display-smoke`).

---

## Numbered smoke test steps

### Step 1: Fresh profile (clean user-data folder)

1. Close any running instances of `nakagawa_player.exe`.
2. Back up and remove the user data and configuration directories at:
   - Configuration: `%LOCALAPPDATA%\Nakagawa\config`
   - Data & titles: `%LOCALAPPDATA%\Nakagawa\data`
   - Logs: `%LOCALAPPDATA%\Nakagawa\logs`
3. Verify the directory `%LOCALAPPDATA%\Nakagawa` is either absent or contains no pre-existing `settings.json`, `library.json`, or cached titles.

**Expected result:** Pass if the environment starts completely clean with no residual settings or library state.

### Step 2: Start the player

1. Launch `build/nakagawa_player.exe` from PowerShell or File Explorer.
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

### Step 7: Encrypted-executable message names the per-title folder

1. Inspect the game card for an encrypted disc image where no decrypted executable has yet been staged.
2. Read the checklist status and instructions displayed on the card.

**Expected result:** Pass if the checklist displays the fail-closed boundary notice explicitly naming the per-title folder:
`Encrypted executable: supply decrypted modules at <user data>\titles\<DISC_ID>\decrypted (#295). Automatic decryption is in the works.`
The path must resolve to `%LOCALAPPDATA%\Nakagawa\data\titles\<DISC_ID>\decrypted` and must not state that decryption succeeded.

### Step 8: Build package with progress, and cancel

1. On a title eligible for package building (such as a source-owned fixture or a showcase demo when available ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309))), click **Build Package**.
2. Observe the package build screen (`VIEW_BUILDING_PACKAGE`): the 4-stage chips (Preflight, Extract, Compile, Package), the indeterminate progress bar, elapsed time counter, and output log lines.
3. While building, click **CANCEL BUILD** (or press `Esc`).

**Expected result:** Pass if the build interface displays real-time progress across the 4 stages, and clicking Cancel stops the build and returns to the library without hanging. (Stopping every process the build started, such as the compiler, is being added under #296.)

### Step 9: Error card wording

1. Select an invalid or corrupted file (e.g. a non-ISO file renamed to `.iso`), or trigger an error state.
2. Observe the error dialog (`VIEW_ERROR`).

**Expected result:** Pass if the error dialog displays a red badge with the specific error code (e.g. `ISO_CORRUPT`), a clear human-readable title, the fail-closed boundary description naming what is missing and any tracking issue, the log file location (`LOG FILE: ...`), and a focused recovery button (e.g. "Try Another File" or "Return to Library") that restores navigation.

### Step 10: F1 overlay in a running package

1. Launch a running package (such as the source-owned display smoke fixture via `mingw32-make display-smoke-player`, or a showcase demo when available ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309))).
2. In the running game window, press the `F1` key.
3. Observe the performance HUD overlay.
4. Press `F1` again.

**Expected result:** Pass if pressing `F1` toggles an in-game HUD displaying live performance telemetry from `SR_PERF` (presented FPS, frame time in ms, VBlank rate, audio status), and pressing `F1` again dismisses the overlay without affecting game execution.

### Step 11: Quit

1. From the library or active window, press `Alt+F4`, click the window close button (`X`), or press `Esc` from the top-level view.
2. Verify process termination in Task Manager or PowerShell (`Get-Process nakagawa_player -ErrorAction SilentlyContinue`).

**Expected result:** Pass if the process exits cleanly with return code 0, all threads stop immediately, no zombie processes linger, and temporary locks are released.

### Step 12: Log and report locations to attach

1. Open the build log folder: `%LOCALAPPDATA%\Nakagawa\data\logs`.
2. If you started a build, collect:
   - the build progress log, `build_<DISC_ID>_progress.jsonl`;
   - the detailed build log, `build_<DISC_ID>.log`.
3. If you started the player from PowerShell, copy its console output. The player has no separate application log file yet.

**Expected result:** Pass if the build logs exist for any build you started. Before attaching them, check that they contain nothing private: no disc images, no game files, and no personal paths you don't want to share.
