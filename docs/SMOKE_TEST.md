# Release Smoke Test: Native Player

Status: CURRENT. This is the 13-step pass/fail smoke test for the Nakagawa Recomp desktop player: `build/nakagawa_player.exe`
from a source checkout, or `bin/nakagawa_player.exe` from the v0.0.1 package. Every release candidate must pass it on
a clean Windows 11 system before it can qualify.

The test covers the player and the project's own test discs only. It doesn't set or change the status of any retail game; only [`COMPATIBILITY.md`](COMPATIBILITY.md) does that (see the status rule in [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md#status-of-retail-games)). Button and label names below are the player's own; buttons show their labels in capitals.

You can send your results with the [Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).

## Prerequisites

- Windows 11 x64 with current Vulkan graphics drivers.
- The player: build it with `mingw32-make player` (which makes `build/nakagawa_player.exe`), or use the packaged `bin/nakagawa_player.exe` with the `SDL3.dll` beside it.
- One of the project's synthetic test ISOs or a showcase demo ISO ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).
- For **BUILD PACKAGE**, either the package's prerequisite consent and download, with an internet connection, or Python, GCC and GNU Make already on `PATH`. Step 8 starts with a clean `PATH` and goes through the consent route ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)).
- For Step 11, the built display smoke fixture (`mingw32-make display-smoke`).

---

## Numbered smoke test steps

### Step 1: Fresh isolated profile

1. Close any running copies of `nakagawa_player.exe`.
2. Use a throwaway Windows profile, or start the player with `LOCALAPPDATA` set to a new empty folder. Don't delete an existing `%LOCALAPPDATA%\Nakagawa` folder.
3. Check that the isolated `%LOCALAPPDATA%\Nakagawa` folder has no settings, library, cached games or logs yet.

The player reads `%LOCALAPPDATA%` first, which is what makes this isolation work. If it is unset,
the player asks Windows for `FOLDERID_LocalAppData` and then uses `%APPDATA%`;
it never writes straight into `%USERPROFILE%`. If an old
`%USERPROFILE%\Nakagawa\data` exists, the player reports it so you can move it yourself, and never
moves or deletes it.

**Expected result:** Pass if the isolated profile starts empty and your existing player data is untouched.

### Step 2: Start the player

1. Start `build/nakagawa_player.exe` from a source checkout, or `bin/nakagawa_player.exe` from the unzipped release package, from PowerShell or File Explorer.
2. Watch the window open.

**Expected result:** Pass if the window opens with the title "Nakagawa Recomp" on a dark background (`#0B0F14`), at 1280x720 (larger when the Windows display scale is above 100%, and never larger than the screen), without crashing or printing assertion errors in the console.

### Step 3: Empty-library first run

1. Look at the first screen, with no games added.
2. Check the empty-library card and the tabs at the top.

**Expected result:** Pass if the **LIBRARY** heading says "No games yet", and the card says "Add your first game" with a **START SETUP WIZARD** button, a **CHOOSE A DISC IMAGE** button and the note "Or drop an .iso onto this window." (The `O` key also adds a disc image.)

### Step 4: Settings persist across a restart

1. Open Settings: click the **SETTINGS** tab, or press `S`.
2. Change a setting, for example **Resolution**, **Frame pacing** or **Reduce motion**.
3. Leave Settings with **DONE**.
4. Quit the player (press `Esc` in the library, or close the window).
5. Start `build/nakagawa_player.exe` again and reopen Settings.

**Expected result:** Pass if `%LOCALAPPDATA%\Nakagawa\config\settings.json` was written and the settings you changed come back after the restart.

### Step 5: Controller settings open

1. In Settings, click **CONTROLLER SETTINGS**.
2. Look over the controller screen.

**Expected result:** Pass if the Controller Settings screen opens and shows whether a controller is connected, the mapping for all 14 PSP buttons, the analog stick dead zone and trigger threshold with **CALIBRATE STICK**, and the live input view.

### Step 6: Add a disc image (synthetic or showcase image)

1. Go back to the library.
2. Drag a valid PSP `.iso` file into the window, or pick it in the Windows file picker: on an empty library use **CHOOSE A DISC IMAGE**, **START SETUP WIZARD** or the `O` key; once you have games, use the **Add a disc image you own** tile.
   - Use a synthetic test image or a showcase demo image when you have one ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).

**Expected result:** Pass if the player moves through its checking step (`VIEW_INSPECTING`), reads `PSP_GAME/PARAM.SFO` itself in C with no outside tools, finds the title and disc ID, and shows the game's card.

### Step 7: Encrypted executables are refused by name

1. Use one of the project's synthetic encrypted test files if you have one. Don't use a retail ISO for this public smoke test.
2. Check that the game card and checklist report `ENCRYPTED_EXECUTABLE` (or `ENCRYPTED_MODULE`) and name `%LOCALAPPDATA%\Nakagawa\data\titles\<DISC_ID>\decrypted`, and that nothing is written to that folder.
3. If the showcase image is the only one you have, record this step as **NOT RUN**: its plain program file doesn't go through the encrypted path.

**Expected result:** Pass when the encrypted test file stops with the named message. Nakagawa does not decrypt games; built-in decryption was removed ([#548](https://github.com/Jstar269/nakagawa-recomp/pull/548), [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550)).

### Step 8: BUILD PACKAGE and prerequisite consent

1. With `PATH` limited to the Windows system folders, select the showcase game and click **BUILD PACKAGE**.
2. Check that one consent card lists every missing prerequisite, with its version, source, download size and licence, and the total, before anything is downloaded ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)).
3. Choose **DOWNLOAD** and watch the progress for each item. The player downloads only the pinned entries in its manifest, from their official HTTPS sources, checks each one's size and SHA-256, installs them into the isolated user profile, and then carries on with the build.
4. Watch the four stages (Preflight, Extract, Compile, Package), the time taken, the output log and the final package check. Cancel one build with **CANCEL BUILD** (or `Esc`), check that you are back in the library, then build again and let it finish.

**Expected result:** On a clean `PATH`, the full consent list appears before anything is downloaded. After you agree, each pinned download checks out, the install shows its progress, and BUILD PACKAGE carries on and passes its package check. Cancelling returns you to the library, and the second build finishes. A game or prerequisite that isn't supported stops on a card that names it; support for games in general is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).

### Step 9: Play the generated package

1. On the showcase game's page, click **PLAY** once the package has passed its check.
2. Watch the game window and check that the demo reaches its first screen and answers its documented controls.

**Expected result:** Pass if the player starts the checked package from your user data, the demo shows frames, and anything unsupported stops with a named message instead of hanging or pretending to work. This proves the synthetic route only; it doesn't qualify any commercial game ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309)).

### Step 10: Error card wording

1. Pick an invalid or damaged file (for example a file that isn't an ISO, renamed to `.iso`), or cause another error.
2. Look at the error screen (`VIEW_ERROR`).

**Expected result:** Pass if the error screen has a plain title and a sentence that say what went wrong and what is missing (with any tracking issue), shows where the log file is (`Log file: ...`) when there is one, and puts focus on a recovery button. **COPY DETAILS** copies a short report that includes the error code (for example `ISO_CORRUPT`), and `Esc` takes you back to the library.

### Step 11: F3 overlay and pause in a running package

1. Start a package (such as the display smoke fixture, with `mingw32-make display-smoke-player` from a source checkout, or a showcase demo if you have one ([#309](https://github.com/Jstar269/nakagawa-recomp/issues/309))). The release archive doesn't include generated test packages.
2. In the running game window, press `F3`.
3. Look at the performance overlay.
4. Press `F3` again.
5. Press `Esc` (or `F1`, or the controller's Guide button), wait a few seconds, then hold a button the game reacts to and press `Esc` again while still holding it.

**Expected result:** Pass if `F3` shows an in-game overlay with live performance numbers from `SR_PERF` (frames per second shown, frame time in ms, VBlank rate, audio status), and pressing `F3` again hides it without affecting the game. The pause passes if the game stops on its last frame under a dark layer with its sound held, the held button does nothing in the game until you let go and press it again, and the game then carries on where it stopped.

### Step 12: Quit

1. From the library or the game window, press `Alt+F4`, click the window's close button (`X`), or press `Esc` in the library.
2. Check that the process has ended, in Task Manager or PowerShell (`Get-Process nakagawa_player -ErrorAction SilentlyContinue`).

**Expected result:** Pass if the process exits cleanly with return code 0, all its threads stop straight away, no stray processes are left behind, and any temporary locks are released.

### Step 13: Log and report locations to attach

1. Open the build log folder: `%LOCALAPPDATA%\Nakagawa\data\logs`.
2. If you started a build, collect:
   - the build progress log, `build_<DISC_ID>_progress.jsonl`;
   - the detailed build log, `build_<DISC_ID>.log`.
3. If you played a game, collect `game-<DISC_ID>-last.txt` from the player's logs folder, `%LOCALAPPDATA%\Nakagawa\logs`: the output of the last game session, kept after the game exits.
4. If you started the player from PowerShell, copy its console output too.

**Expected result:** Pass if the build logs exist for any build you started. Before you attach them, check that they contain nothing private: no disc images, no game files, and no personal paths you don't want to share.
