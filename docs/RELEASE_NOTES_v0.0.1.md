# Nakagawa Recomp v0.0.1 Release Notes

Status: DRAFT. Not released yet.
Release qualification authority: [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278).
Audience: players and contributors trying the first public build.

## What v0.0.1 is

Nakagawa Recomp is an experimental static recompiler that translates user-supplied PlayStation Portable (PSP) executables into C, links them with a native C runtime, and runs the resulting binary on Windows via SDL3 and Vulkan. Encrypted executables on a disc you own are decrypted locally by the player before recompilation. v0.0.1 is the first public build, featuring a standalone desktop player (`build/nakagawa_player.exe`), ahead-of-time (AOT) package building, synthetic verification suites, and a fail-closed runtime architecture where unsupported, missing, or unimplemented behavior halts at a named semantic boundary with a clear message.

## What you can do

- **Identify discs directly from `PARAM.SFO`**: Drag and drop or select a PSP `.iso` image in the player; the player reads `PSP_GAME/PARAM.SFO` directly in C to extract the title and Disc ID, checking it against supported title profiles without using an update SFO or volume ID ([#426](https://github.com/Jstar269/nakagawa-recomp/pull/426)).
- **Play source-owned showcase demos**: Run project-authored PSPDEV demos (a lit, textured 3D scene and Breakout with paddle input, sound effects, and savedata high score) built from source into deterministic plaintext ISOs and packages, discovered automatically with authentic icons in the player ([#477](https://github.com/Jstar269/nakagawa-recomp/pull/477)).
- **Configure and calibrate controllers in the player**: Open Controller Settings (`VIEW_CONTROLLER_SETTINGS`) from Settings to remap 14 digital PSP controls and the analog stick with conflict detection, deadzone and trigger threshold calibration, live input monitoring, and atomic profile persistence to `input_profile.json` ([#455](https://github.com/Jstar269/nakagawa-recomp/pull/455)).
- **Build runtime packages with live progress and cancel**: Click **BUILD PACKAGE** (or **REBUILD PACKAGE**) in the player to recompile a title via `tools/nk_cli.py build-package`, with real-time 4-stage progress (Preflight, Extract, Compile, Package), elapsed time display, in-app cancellation ending the child process tree, and an in-game performance HUD overlay toggled via `F3` ([#465](https://github.com/Jstar269/nakagawa-recomp/pull/465), [#469](https://github.com/Jstar269/nakagawa-recomp/pull/469)).
- **Install missing build prerequisites after one consent**: On a clean `PATH`, the player offers one consent card for the pinned tools needed by **BUILD PACKAGE**, shows each tool's version, host, size, licence and the total, then downloads and verifies the declared artifacts before resuming the build ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)). You can also install Python, GNU Make and the MSYS2 UCRT64 compiler yourself.
- **Display real disc icons and persist settings**: Library cards show the disc's authentic `ICON0.PNG` and dimmed `PIC1.PNG` background loaded safely at runtime from the ISO with fallback to monogram badges; settings (resolution scale, frame cap, VSync, reduce motion) persist atomically to `settings.json` across restarts, alongside truthful status text ([#458](https://github.com/Jstar269/nakagawa-recomp/pull/458)).
- **Encrypted discs decrypt locally**: the player decrypts your own disc's encrypted `EBOOT.BIN` and modules on your PC, the way PPSSPP does, into `<user data>/titles/<DISC_ID>/decrypted/` ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). It never downloads games and never decrypts firmware. A file it can't decrypt fails closed with a named reason, and a plain copy from your own disc placed there is used instead ([#428](https://github.com/Jstar269/nakagawa-recomp/pull/428)).
- **Serve game assets directly through archive-backed VFS**: The runtime serves game data directly from validated `.xb` archives using a read-only archive-backed virtual filesystem (`sceIoOpen`, `sceIoGetstat`, `sceIoDopen`), eliminating the need to pre-extract loose files before running ([#475](https://github.com/Jstar269/nakagawa-recomp/pull/475)).
- **Import PSP system fonts into a validated local cache**: Import the PSP system fonts copied from your own PSP (`jpn0.pgf`, `ltn0.pgf`–`ltn15.pgf`, `kr0.pgf`) into `<user data>/fonts/v2/` using `python tools/nk_cli.py fonts import <folder>`, with structural header validation, atomic manifest generation, and the project-authored public PGF reader ([#441](https://github.com/Jstar269/nakagawa-recomp/pull/441), [#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)).

## Public verification you can reproduce

The public, game-input-free verification suite and the media subsystem are the parts of v0.0.1 you can
confirm yourself, with no retail ISO, PRX, save, key, or decrypted module:

- **Native player application:** `mingw32-make player` compiles `build/nakagawa_player.exe` using SDL3. It supports drag-and-drop and file-dialog ISO loading, parses the ISO9660 PVD and `PARAM.SFO` metadata in C, performs asset census staging, and handles gamepad navigation.
- **Display smoke verification:** `mingw32-make display-smoke` proves the two-phase pipeline, loader, NID imports, vblank delivery, display latch, and the off-screen GUI check. The player-launch variant, `mingw32-make display-smoke-player`, does not yet pass on `main`.
- **Production smoke:** `mingw32-make production-smoke` generates a synthetic PSP PRX, statically recompiles it MIPS-to-C-to-native, and verifies and runs the AOT output.
- **Fail-closed dispatch:** `mingw32-make production-smoke-gap` proves the fail-closed dispatch path when a synthetic function is intentionally omitted from AOT emission.
- **Differential cosimulation:** `mingw32-make cosim-selftest` compares AOT and interpreter traces, writes, memory, and architectural state over synthetic cases.
- **Platform ladder:** `mingw32-make platform-ladder` exercises relocations, scheduler threading, scalar FPU, and filesystem semantics across synthetic workloads.
- **Core runtime selftests:** `selftest`, `sched-selftest`, `hle-thread-selftest`, and `public-safe-verify` pass on the verified Windows host.
- **Media subsystem:** PSMF demuxing and H.264 video decoding (through Windows Media Foundation) process video cutscenes.

From a clean Windows checkout with the supported toolchain, the four headline commands are:

```powershell
mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

The commands generate their synthetic inputs beneath the ignored `build/` tree.

## What v0.0.1 is not

- **No bundled games**: This repository does not include game binaries, proprietary game assets, firmware modules, decryption keys, or private oracle traces. Nakagawa Recomp never downloads games or game data. With your consent, it downloads only the build tools declared in the prerequisite manifest. Users must supply their own PSP game disc image, of a game they own, in uncompressed standard `.iso` format ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Encrypted executables are decrypted from your own disc**: Nakagawa Recomp is an experimental static recompiler, not a finished consumer emulator. No commercial PSP game is claimed playable end-to-end from an ISO alone. Most retail PSP executables (`EBOOT.BIN` and `.prx` modules) are encrypted; the player decrypts them from a UMD disc image you own, locally, with built-in tables ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). One it can't decrypt is named and fails closed. PSN `EBOOT.PBP` packages are not decrypted yet, and PGD-protected content is unsupported; broader ISO-to-Play support is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Experimental titles**: A valid PSP disc that has no profile yet is imported as **Experimental**. The card notes that compatibility is unknown, checklist items indicate what is missing, and Play stays unavailable until a matching runtime package exists. PAC-MAN Championship Edition reaches its menu and plays a stage on the maintainer's copy through generic fixes with no title-specific branch ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)); full-game compatibility and other title candidates are in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). Non-PSP images are refused.
- **Fonts need the user's own firmware fonts**: Authentic in-game typography requires Sony firmware font files (`jpn0.pgf`, `ltn0.pgf`) dumped from a real PSP console (`flash0:/font/`). The project does not include or distribute these proprietary files; you provide them from your own PSP and import them into the validated local cache via `python tools/nk_cli.py fonts import <folder>` ([#300](https://github.com/Jstar269/nakagawa-recomp/issues/300)). Public builds read these fonts with the project-authored PGF reader ([#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)). The project's own built-in fonts are generated from open font files by the project-authored converter in `tools/ttf2pgf.py` ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).
- **Windows is the supported player platform and Linux is development-only**: Only Windows 11 x64 is supported today for the desktop player runtime. Linux host runtime/player support is in progress after backend seams are portable ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)), followed by macOS (Apple Silicon) ([#329](https://github.com/Jstar269/nakagawa-recomp/issues/329)) and Android ARM64 ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)). Linux/WSL currently serves as a development platform for building the player and portable core and running native tests ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306), [#471](https://github.com/Jstar269/nakagawa-recomp/pull/471)).
- **Audio output requires a connected audio device**: Public builds drive one SDL3 audio stream per `sceAudio` channel ([#301](https://github.com/Jstar269/nakagawa-recomp/issues/301), [#411](https://github.com/Jstar269/nakagawa-recomp/pull/411)); with no audio device the game runs silently after one message. The SAS hardware conformance group is planned ([#311](https://github.com/Jstar269/nakagawa-recomp/issues/311)).

## Checking your download

v0.0.1 is not code-signed. The first time you run `nakagawa_player.exe`, Windows SmartScreen may say
"Windows protected your PC" because the file has no publisher signature yet. Before you choose
**More info** and then **Run anyway**, check that the download is the one this project built:

1. Compare the ZIP's SHA-256 with `nakagawa-recomp-0.0.1-windows-x64.zip.sha256`, published next to it
   on the release page:

   ```powershell
   (Get-FileHash .\nakagawa-recomp-0.0.1-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   ```

2. After extracting, `SHA256SUMS.txt` lists the SHA-256 of every file in the package.
3. With the [GitHub CLI](https://cli.github.com/), check that each executable and DLL under `bin\` was
   built by this repository's release workflow:

   ```powershell
   gh attestation verify .\bin\nakagawa_player.exe --repo Jstar269/nakagawa-recomp
   ```

If any check fails, don't run the file; report it as described under [How to report](#how-to-report).

## Requirements when running the packaged player

The v0.0.1 package layout (defined in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md)) is
`bin/nakagawa_player.exe` beside the public source export under `source/`, which
includes `tools/nk_cli.py`. The package includes the resolved SDL3 and SDL3_ttf DLL
closures with generated third-party notices and relinking material. Its exact DLL
inventory and licences are recorded in `THIRD_PARTY_NOTICES` and the SBOM. The Vulkan
loader and a Vulkan-capable graphics driver remain host-provided:

- **Running the player:** launch `nakagawa_player.exe` with its adjacent packaged
  `SDL3.dll` and a Vulkan-capable GPU driver/runtime. The player itself runs without a
  source checkout; browsing the library and settings works.
- **Typography:** the package stages `SDL3_ttf.dll` and its resolved runtime closure
  beside the executable when available, so the readable UI font loads from the package
  without a toolchain `PATH`. The notices and SBOM list each shipped library.
- **BUILD PACKAGE:** recompiling a title needs the full developer toolchain, which the
  player can install after one consent on a clean `PATH`. The consent lists each pinned
  prerequisite and downloads from the manifest's declared official hosts ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)). The
  player looks for `tools/nk_cli.py` in this order: the `NK_INSTALL_ROOT` environment
  variable (a folder containing `tools/`), beside the executable,
  `<exe dir>/../tools`, `<exe dir>/../source/tools` (this release's layout), the
  working directory, and its parent. When you provide your own toolchain, it needs
  `python`, `gcc`, and `mingw32-make` on `PATH`; the supported install is MSYS2 UCRT64
  in [`SETUP.md`](SETUP.md). Missing pieces are named, never silent: `CLI_NOT_FOUND`
  lists every searched location and the `NK_INSTALL_ROOT` fix,
  `BUILD_TOOLCHAIN_MISSING` names the missing tool, and `PYTHON_NOT_FOUND` names the
  interpreter. With the package, these lead to the consent path; prerequisite installation
  landed in #547. In an unpackaged run, set `NK_INSTALL_ROOT` to the unzipped
  `source/` folder so the player uses this package's CLI.

The exact package command and its fail-closed scan are in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md).

**Hardware:** Windows 11 x64 with a Vulkan GPU, 8 GB RAM (16 GB recommended) and 5 GB of free
disk space (10 GB on an SSD recommended). Building a game is the heavy part: one compiler process
peaked at about 3 GB. The measured numbers are in
[`SETUP.md`](SETUP.md#system-requirements).

## Title-specific compatibility patches

Hot Shots Tennis runs with 11 title-specific compatibility patches: the forced branches, success stubs and scheduler or display surrogates that `python tools/compat_overrides.py --debt-census` lists. They fire only for that title. The recompiler emits its patches only under that title's profile, and the runtime applies the rest only through that title's validated configuration, so generic runtime code outside the configuration layer carries no title address. They are not fail-closed, so a missing behaviour can look like success on that route. Each run's log names every patch that fired, or says that none did, in its `COMPAT_PATCH_REPORT` lines. Retiring them is v0.0.2 work, tracked in [#363](https://github.com/Jstar269/nakagawa-recomp/issues/363).

## Known issues

- **Second-title scope**: PAC-MAN Championship Edition reaches its menu and plays a stage on the maintainer's copy, through generic fixes with no title-specific branches ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)). This is not full-game compatibility; other candidates stop at named unsupported imports and further title bring-up is in the works under [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).
- **GPU vertex transform, lighting, and skinning**: In-game rendering currently uses software transformation with Vulkan presentation; moving GE transform, lighting, and skinning to Vulkan waits for GE correctness and CPU cost measurements behind software-reference differential proof ([#318](https://github.com/Jstar269/nakagawa-recomp/issues/318)).
- **Networking APIs unimplemented**: All networking APIs (`sceNet`, `sceNetAdhoc`, `sceNetApctl`) are unimplemented. Network module loading (`sceUtilityLoadNetModule`) returns controlled refusal (`0x80110001` `SCE_ERROR_UTILITY_INVALID_STATUS`); sockets and infrastructure return unsupported with no fake success or phantom connections ([#1139](https://github.com/Jstar269/nakagawa-recomp/issues/1139), [#307](https://github.com/Jstar269/nakagawa-recomp/issues/307)).
- **ISO-to-Play still has title boundaries**: The player can install pinned build prerequisites after one consent, then build and validate a package for a supported title ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)). It does not make every ISO playable: unsupported profiles/imports and PGD-protected data fail closed with named guidance; broader title intake is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Controller remapping is global by default**: Controller settings save host input configuration and calibration to `input_profile.json`. A custom mapping for one disc is optional. Controller setup from a running game's pause overlay is not available yet.
- **In-game Latin text can drop, clip or overflow**: The public build serves one Latin font file for all Latin font indices, so some in-game Latin dialog text loses letters, clips, or overflows its box until each index has its own file ([#1142](https://github.com/Jstar269/nakagawa-recomp/issues/1142)).
- **Project-font dashes and a full-width YES/NO band**: A title that runs its own `sceFont` can draw the project's PGF fonts as one-pixel dashes, and its YES/NO confirmation prompt can draw as a full-width band ([#1055](https://github.com/Jstar269/nakagawa-recomp/issues/1055), [#821](https://github.com/Jstar269/nakagawa-recomp/issues/821)).
- **Launcher UI is soft at most window sizes**: The player draws its UI on a fixed 1280x720 canvas, so the UI looks blurry at any window size other than the starting one, and worst in fullscreen. Game frames are sharp at any size ([#1131](https://github.com/Jstar269/nakagawa-recomp/pull/1131)). The UI part is tracked in [#1129](https://github.com/Jstar269/nakagawa-recomp/issues/1129).
- **H.264 video is Windows-only for now**: H.264 decoding runs through Windows Media Foundation. The portable decoder backend in [#283](https://github.com/Jstar269/nakagawa-recomp/issues/283) is not merged (PR [#888](https://github.com/Jstar269/nakagawa-recomp/pull/888) is open), so video decoding is Windows-only for now.
- **CPU reads of GPU-rendered VRAM**: A CPU read of depth data through a VRAM mirror is still wrong for some depth-buffer settings ([#1049](https://github.com/Jstar269/nakagawa-recomp/issues/1049)).
- **ATRAC3 sound effects**: A private replay still reports ATRAC3 decode failures for some sound effects, and the decode contract is not yet matched to PSP hardware ([#1096](https://github.com/Jstar269/nakagawa-recomp/issues/1096)).
- **Video cutscene timing**: The decoded-picture count and the audio offset are not yet fully accounted for ([#279](https://github.com/Jstar269/nakagawa-recomp/issues/279)).
- **Not code-signed yet**: Windows SmartScreen warns on first run until releases are signed; see [Checking your download](#checking-your-download) for how to confirm a download before you run it.
- **Console window**: The player is a console-subsystem app, so opening it by double-clicking also opens a console window beside it ([#1146](https://github.com/Jstar269/nakagawa-recomp/issues/1146)).
- **Resolved: one-frame skinned-character corruption**: The corruption at the title-to-main-menu transition was traced to a VFPU3 compare-decode error, fixed in [#822](https://github.com/Jstar269/nakagawa-recomp/pull/822). The maintainer confirmed on 2026-10-19 that it no longer appears on the GPU presenter, and [#69](https://github.com/Jstar269/nakagawa-recomp/issues/69) closed on 2026-10-20. Reopen #69 if it returns.

## How to report

If you encounter issues while running the native desktop player (`build/nakagawa_player.exe`), follow the pass/fail verification procedure in [`SMOKE_TEST.md`](SMOKE_TEST.md).

Submit smoke test results and bug reports using the [Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).

When reporting a problem:

1. Note the specific step in [`docs/SMOKE_TEST.md`](SMOKE_TEST.md) that failed and the exact error code or message displayed on the card (e.g. `ISO_CORRUPT`).
2. Attach build logs from `%LOCALAPPDATA%\Nakagawa\data\logs` (`build_<DISC_ID>_progress.jsonl` and `build_<DISC_ID>.log`) and console output from the terminal.
   For a problem during play, attach `game-<DISC_ID>-last.txt` from the player's logs folder (`%LOCALAPPDATA%\Nakagawa\logs`): the last game session's output, kept after the game exits. The run before it is `game-<DISC_ID>-prev.txt`. **Copy details** on the error card names the file.
3. Check all logs before attaching to ensure they contain nothing private: no disc images, no game files, no decryption keys, and no personal paths you do not want to share.

## Thanks

Thank you to the people outside the maintainer team who contributed to this release:

- [@gufernandess](https://github.com/gufernandess): Windows CMake linking for the player targets
  ([#869](https://github.com/Jstar269/nakagawa-recomp/pull/869)), and the original portable FFmpeg/libavcodec H.264
  backend ([#888](https://github.com/Jstar269/nakagawa-recomp/pull/888)), which inspired the maintained backend that
  landed in [#1087](https://github.com/Jstar269/nakagawa-recomp/pull/1087).
- [@Apurvbkt](https://github.com/Apurvbkt): closing out a stale entry in `ISSUES.md`
  ([#1162](https://github.com/Jstar269/nakagawa-recomp/pull/1162)).
