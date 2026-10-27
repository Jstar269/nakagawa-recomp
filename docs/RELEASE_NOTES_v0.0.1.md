# Nakagawa Recomp v0.0.1 Release Notes

Status: CURRENT. These are the notes of the v0.0.1 release; its release page names the exact commit.
Release qualification authority: [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278).
Audience: players and contributors trying the first public build.

## What v0.0.1 is

Nakagawa Recomp is an experimental static recompiler. It takes the program code of a PlayStation Portable (PSP) game
you own, translates it into C, links it with a native C runtime, and runs the result as a Windows program through
SDL3 and Vulkan. Most PSP games keep their program files encrypted; the player decrypts them from your own disc image,
on your PC, the way the PPSSPP emulator does ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)).

v0.0.1 is the first public build. It brings a standalone desktop player (`bin\nakagawa_player.exe` in the ZIP), ahead-of-time
(AOT) package building, test suites you can run yourself, and a runtime that never fakes what it can't do: when a game
needs something unsupported, missing or not built yet, it stops and tells you clearly what is missing.

## What you can do

- **Play Hot Shots Tennis at full speed.** Hot Shots Tennis (UCUS98701) is rated **Playable**. It is a game that
  draws 30 frames per second, and this release runs it at that rate in menus and in matches on the reference PC
  named below ([#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)). You need your own copy of the
  game.
- **Identify a disc from its `PARAM.SFO`.** Drag a PSP `.iso` into the player, or pick one. The player reads
  `PSP_GAME/PARAM.SFO` itself to find the title and disc ID, and checks it against the supported title profiles. It
  never uses an update SFO or the volume ID ([#426](https://github.com/Jstar269/nakagawa-recomp/pull/426)).
- **Play the showcase demos.** Two PSP demos the project wrote itself, a lit, textured 3D scene and a Breakout game
  with paddle input, sound effects and a saved high score, are built from source into the same plaintext ISOs and
  packages every time. The library lists them in their own **Test this PC** row, apart from your games, so you can
  check that graphics, sound and controls work before you add a game
  ([#477](https://github.com/Jstar269/nakagawa-recomp/pull/477), [#1534](https://github.com/Jstar269/nakagawa-recomp/pull/1534)).
- **Set up and calibrate controllers in the player.** Open **Settings**, then **CONTROLLER SETTINGS**,
  to remap the 14 digital PSP controls and the analog stick. It warns about conflicts, sets the dead zone and
  trigger threshold, shows your inputs live, and saves the profile safely to `input_profile.json`
  ([#455](https://github.com/Jstar269/nakagawa-recomp/pull/455)).
- **Build a game's package and watch it happen.** Click **BUILD PACKAGE** (or **REBUILD PACKAGE**) to recompile a
  game through `tools/nk_cli.py build-package`. You see four stages (Preflight, Extract, Compile, Package) and the
  time taken, and you can cancel, which stops every process the build started. In a running game, `F3` shows a
  performance overlay ([#465](https://github.com/Jstar269/nakagawa-recomp/pull/465),
  [#469](https://github.com/Jstar269/nakagawa-recomp/pull/469)).
- **Build with no download.** The release ZIP carries the pinned build tools **BUILD PACKAGE** needs (CPython 3.15.1,
  and the MSYS2 UCRT64 compiler, GNU Make, SDL3 and their libraries) in `prereqs/`. The player checks that folder
  against the pinned manifest before each build and then uses it, with no consent card and no network connection
  ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285)). Without that folder, as in a source checkout,
  the player offers one consent card for the same pinned tools, listing each one's version, source, size and licence,
  then downloads and checks them ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)). You can also
  install Python, GNU Make and the MSYS2 UCRT64 compiler yourself.
- **See real disc icons, and keep your settings.** Library cards show the disc's own `ICON0.PNG` over a dimmed
  `PIC1.PNG` background, read safely from the ISO, with a lettered badge if there is none. Your settings (resolution
  scale, frame cap, VSync, reduce motion) are saved safely to `settings.json` and come back after a restart, and the
  status messages match what is really happening ([#458](https://github.com/Jstar269/nakagawa-recomp/pull/458)).
- **Use an encrypted disc as it is.** When you add a UMD disc image you own, the player decrypts its encrypted
  `EBOOT.BIN` and the encrypted modules it needs, on your PC, with tables built into the program, the way PPSSPP does
  ([#1494](https://github.com/Jstar269/nakagawa-recomp/pull/1494)). The decrypted files stay on your PC, in
  `<user data>/titles/<DISC_ID>/decrypted/`. Nothing is downloaded, and PSP system software (firmware) is never
  decrypted. If a file can't be decrypted, the check names it and the reason, and a plain copy you made from your own
  disc in that folder is used instead ([#428](https://github.com/Jstar269/nakagawa-recomp/pull/428)).
- **Pause a running game.** Press `Esc`, `F1` or the controller's Guide button: the game stops on its last frame
  under a dark screen with its sound held, and a button you were holding does nothing until you press it again. Press
  the same key to carry on where it stopped. The pause has no menu yet
  ([#1159](https://github.com/Jstar269/nakagawa-recomp/issues/1159)).
- **Read game files straight from archives.** The runtime serves game data directly from validated `.xb` archives
  through a read-only virtual filesystem (`sceIoOpen`, `sceIoGetstat`, `sceIoDopen`), so you don't have to unpack
  loose files before playing ([#475](https://github.com/Jstar269/nakagawa-recomp/pull/475)).
- **Use your own PSP's fonts.** Games draw their text with the PSP's system fonts, which come from your own PSP:
  copy `jpn0.pgf`, `ltn0.pgf`–`ltn15.pgf` and `kr0.pgf` from its `flash0:/font/` folder to your PC. In the player,
  **Settings > PSP FONTS** has **IMPORT FONTS FOLDER**; from a command line,
  `python tools/nk_cli.py fonts import <folder>` does the same.
  [How to copy the fonts from your PSP](SETUP.md#copying-the-fonts-from-your-psp) walks through it. The importer
  checks each file's header and writes its
  manifest in one safe step, and the project-authored PGF reader draws them
  ([#441](https://github.com/Jstar269/nakagawa-recomp/pull/441), [#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)).
  When a game asks for a font you haven't imported, it gets a stand-in, and the Fonts check and the game's log say
  which one ([#1469](https://github.com/Jstar269/nakagawa-recomp/pull/1469)).

## Public verification you can reproduce

You can check these parts of v0.0.1 yourself, with no retail ISO, PRX, save, key or decrypted module:

- **The player:** `mingw32-make player` builds `build/nakagawa_player.exe` with SDL3. It takes an ISO by drag and drop
  or from a file dialog, reads the ISO9660 volume and `PARAM.SFO` itself in C, stages the disc's files, and works with
  a gamepad.
- **Display smoke test:** `mingw32-make display-smoke` checks the two-phase pipeline, the loader, NID imports, VBLANK
  delivery, the display latch and the off-screen window check. The version that goes through the player,
  `mingw32-make display-smoke-player`, doesn't pass in this release yet.
- **Production smoke test:** `mingw32-make production-smoke` makes a synthetic PSP PRX, recompiles it from MIPS to C
  to native code, then checks and runs the result.
- **Fail-closed dispatch:** `mingw32-make production-smoke-gap` leaves one synthetic function out of the translated
  code on purpose, and checks that the real dispatcher hands it to the fail-closed interpreter, which runs it and
  returns to translated code.
- **Differential cosimulation:** `mingw32-make cosim-selftest` runs synthetic cases both as translated code and in the
  interpreter, and compares their traces, writes, memory and CPU state.
- **Platform ladder:** `mingw32-make platform-ladder` exercises relocations, threads and scheduling, scalar floating
  point, and the filesystem over synthetic workloads.
- **Core runtime self-tests:** `selftest`, `sched-selftest`, `hle-thread-selftest` and `public-safe-verify` pass on
  the verified Windows host.
- **Movies:** PSMF demuxing and H.264 video decoding (through Windows Media Foundation) handle video cutscenes.

From a clean Windows checkout with the supported toolchain, the four headline commands are:

```powershell
mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

They create their synthetic inputs under the ignored `build/` folder.

## What v0.0.1 is not

- **No games included.** This repository contains no game programs, game assets, firmware modules, decryption keys
  or private test recordings. Nakagawa Recomp never downloads games or game data, and the release makes no network
  connection: its build tools are in the ZIP. Only without them, as in a source checkout, does the player offer to
  download the build tools named in the prerequisite manifest, with your consent. You supply your own disc image of a
  game you own, as an
  uncompressed standard `.iso` ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Not a finished emulator, and not every kind of game file yet.** Nakagawa Recomp is an experimental static
  recompiler. Hot Shots Tennis is the one game rated **Playable**; [`COMPATIBILITY.md`](COMPATIBILITY.md) lists
  what each tested game reaches. The
  player decrypts UMD disc images (`.iso`); PSN downloads (`EBOOT.PBP` packages) are not decrypted yet, and
  PGD-protected content is unsupported; broader ISO-to-Play support is in the works
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Other games are experimental.** A valid PSP disc with no profile yet is imported as **Experimental**. The card
  says compatibility is unknown, the checklist shows what is missing, and Play stays off until a matching runtime
  package exists. PAC-MAN Championship Edition reaches its menu and plays a stage on the maintainer's copy, through
  general fixes with no title-specific branch ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552),
  [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)); the full game and other games are in the works
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). Images that aren't PSP discs are refused.
- **No PSP fonts included.** The PSP's own fonts belong to Sony, so the project doesn't include or share them; you
  import them from your own PSP ([#300](https://github.com/Jstar269/nakagawa-recomp/issues/300)). Without them a game
  shows the project's own fonts, made from open font files by its converter `tools/ttf2pgf.py`
  ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)). They look close to the PSP's, but not the same
  ([#1300](https://github.com/Jstar269/nakagawa-recomp/issues/1300)). All 18 of the PSP's own system fonts import
  ([#1536](https://github.com/Jstar269/nakagawa-recomp/pull/1536)).
- **Windows only for playing; Linux is for development.** The desktop player supports only Windows 11 x64 today.
  Linux support for the runtime and player is in progress and follows the work to make the backend seams portable
  ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)), with macOS on Apple Silicon
  ([#329](https://github.com/Jstar269/nakagawa-recomp/issues/329)) and Android ARM64
  ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)) after it. For now, Linux and WSL are for building
  the player and the portable core and running the native tests
  ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306), [#471](https://github.com/Jstar269/nakagawa-recomp/pull/471)).
- **Sound needs an audio device.** Public builds drive one SDL3 audio stream per `sceAudio` channel
  ([#301](https://github.com/Jstar269/nakagawa-recomp/issues/301), [#411](https://github.com/Jstar269/nakagawa-recomp/pull/411)).
  With no audio device, the game runs silently after one message. A group of SAS checks against real hardware is
  planned ([#311](https://github.com/Jstar269/nakagawa-recomp/issues/311)).

## Checking your download

v0.0.1 is not code-signed, so the first time you run `nakagawa_player.exe`, Windows SmartScreen may say
"Windows protected your PC", because the file has no publisher signature. That warning is about the missing
signature, not about anything found in the file. Don't turn SmartScreen or your antivirus off. Instead, before you
choose **More info** and then **Run anyway**, check that the download is the one this project built:

1. Before you extract it, in the folder where you saved the ZIP, compare its SHA-256 with
   `nakagawa-recomp-0.0.1-windows-x64.zip.sha256`, published next to it on the release page:

   ```powershell
   (Get-FileHash .\nakagawa-recomp-0.0.1-windows-x64.zip -Algorithm SHA256).Hash.ToLower()
   ```

2. After extracting, open PowerShell in the extracted folder (the one with `bin\` in it).
   `SHA256SUMS.txt` there lists the SHA-256 of every file in the package.
3. With the [GitHub CLI](https://cli.github.com/), check that each executable and DLL under `bin\` was
   built by this repository's release workflow. Each file carries a signed build attestation (SLSA Build
   Level 2). The GitHub CLI needs you to sign in once first (`gh auth login`). Set `$Commit` to the full commit
   the release page names (the commit the `v0.0.1` tag points at),
   then check one file:

   ```powershell
   $Commit = '<the 40-character commit from the release page>'
   gh attestation verify .\bin\nakagawa_player.exe --repo Jstar269/nakagawa-recomp `
       --signer-workflow Jstar269/nakagawa-recomp/.github/workflows/provenance-attestation.yml `
       --source-ref refs/heads/main --source-digest $Commit
   ```

   Or every file under `bin\` at once:

   ```powershell
   Get-ChildItem .\bin -Recurse -File | ForEach-Object {
       gh attestation verify $_.FullName --repo Jstar269/nakagawa-recomp `
           --signer-workflow Jstar269/nakagawa-recomp/.github/workflows/provenance-attestation.yml `
           --source-ref refs/heads/main --source-digest $Commit
   }
   ```

   The signer workflow and the commit together prove the file came from this release's build, not from
   some other workflow run in the repository.

If any check fails, don't run the file; report it as described under [How to report](#how-to-report).

### Source of the bundled build tools

The source of the build tools in `prereqs/` is published next to the ZIP as
`nakagawa-recomp-0.0.1-prerequisite-sources.zip`, with its own `.sha256`: the MSYS2 source package of each MSYS2
package, the CPython source release, and the source releases of the libraries built into the CPython runtime
(OpenSSL, libffi, SQLite and others). The Microsoft C runtime that comes with CPython has no public source; its
redistribution terms are in `prereqs/notices`. `prereqs/notices/SOURCES.txt` in the ZIP names each file of the source
archive with its SHA-256, and `prereqs/notices` holds every bundled package's licence files. The bundled tools leave
out their manuals (`share/doc`, `share/info`, `share/man`), which a build never reads; the source archive has them.

## Requirements when running the packaged player

The v0.0.1 package layout (defined in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md)) puts `bin/nakagawa_player.exe`
next to the public source under `source/`, which includes `tools/nk_cli.py`. The package includes the SDL3 and
SDL3_ttf DLLs and the DLLs they depend on, with generated third-party notices and the material you need to relink
them. The exact list of DLLs and their licences is in `THIRD_PARTY_NOTICES` and the SBOM. The Vulkan loader and a
Vulkan-capable graphics driver come from your own system:

- **Running the player:** start `nakagawa_player.exe` with the packaged `SDL3.dll` beside it and a Vulkan-capable GPU
  driver. The player runs without a source checkout; you can browse the library and change settings.
- **Text in the player:** the package puts `SDL3_ttf.dll` and the DLLs it depends on beside the executable when they
  are available, so the player's readable font loads from the package without a toolchain on `PATH`. The notices and
  SBOM list every library shipped.
- **BUILD PACKAGE:** recompiling a game needs Python, GCC and GNU Make. The package carries the pinned ones in
  `prereqs/`, next to `bin/` and `source/`. Before each build the player checks them against
  `source/assets/prereq_manifest.json`, then uses them with no consent card and no download
  ([#1285](https://github.com/Jstar269/nakagawa-recomp/issues/1285)). If that folder is damaged, the player names the
  first missing or changed file (`PREREQUISITE_BUNDLE_INVALID`) instead of downloading a replacement: extract the ZIP
  again. Without `prereqs/`, the player can install the pinned tools after one consent; the consent card lists each
  one and downloads only from the official sources named in the manifest
  ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)).

  The player looks for `tools/nk_cli.py` in this order: the folder named by the `NK_INSTALL_ROOT` environment
  variable (a folder that contains `tools/`), beside the executable, `<exe dir>/../tools`,
  `<exe dir>/../source/tools` (this release's layout), the working directory, and its parent. If you bring your own
  toolchain, it needs `python`, `gcc` and `mingw32-make` on `PATH`; the supported install is MSYS2 UCRT64, described in
  [`SETUP.md`](SETUP.md).

  A missing piece is always named: `CLI_NOT_FOUND` lists every place it looked and the `NK_INSTALL_ROOT` fix,
  `BUILD_TOOLCHAIN_MISSING` names the missing tool, and `PYTHON_NOT_FOUND` names the missing interpreter. Without
  `prereqs/`, these lead you to the consent card (prerequisite installation landed in #547). If you run the player
  outside the package, set `NK_INSTALL_ROOT` to the unzipped `source/` folder so the player uses this package's CLI.

The exact packaging command and the check that stops it on any problem are in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md).

**Hardware:** Windows 11 x64 with a Vulkan GPU, 8 GB RAM (16 GB recommended) and 5 GB of free
disk space (10 GB on an SSD recommended). Building a game is the heavy part: one compiler process
peaked at about 3 GB. The measured numbers are in
[`SETUP.md`](SETUP.md#system-requirements).

**Reference PC:** the speeds in these notes were measured on Windows 11 with an AMD Ryzen 9 5900X and an NVIDIA
GeForce RTX 3080. Slower PCs have not been measured yet.

**Download size:** the build tools in the ZIP's `prereqs/` unpack to 7,299 files and 697 MB. Their source is a
separate 525 MB download next to the ZIP; see
[Source of the bundled build tools](#source-of-the-bundled-build-tools). The release page gives the ZIP's own size.

## Title-specific compatibility patches

Hot Shots Tennis runs with 11 title-specific compatibility patches: the forced branches, success stubs and scheduler or display surrogates that `python tools/compat_overrides.py --debt-census` lists. They fire only for that title. The recompiler emits its patches only under that title's profile, and the runtime applies the rest only through that title's validated configuration, so generic runtime code outside the configuration layer carries no title address. They are not fail-closed, so a missing behaviour can look like success on that route. Each run's log names every patch that fired, or says that none did, in its `COMPAT_PATCH_REPORT` lines. Retiring them is v0.0.2 work, tracked in [#363](https://github.com/Jstar269/nakagawa-recomp/issues/363).

## Known issues

- **A few late sound wakes while loading.** Sound threads in Hot Shots Tennis now rarely wait behind the game's busy
  thread, which is what dropped sound effects and text clicks in earlier builds. A few late wakes remain, mostly
  while the game loads ([#1296](https://github.com/Jstar269/nakagawa-recomp/issues/1296)).
- **Some movies show black, and some VRAM reads are wrong.** A movie whose frames pass through a VRAM mirror shows a
  black screen for now, and a CPU read of depth data through a VRAM mirror is still wrong for some depth-buffer
  settings ([#1049](https://github.com/Jstar269/nakagawa-recomp/issues/1049)).
- **Only one other game has been tried.** PAC-MAN Championship Edition reaches its menu and plays a stage on the
  maintainer's copy, through general fixes with no title-specific branches
  ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)).
  That isn't the full game. Other games tried so far stop at imports Nakagawa doesn't support yet, and work on more
  games is in the works under [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).
- **Not every disc can be set up yet.** The player builds and checks a package for a supported game, but it does not
  make every ISO playable: profiles and imports that aren't supported, and PGD-protected data, stop with a message
  that says what is missing. Wider support is in the works
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **3D work stays on the CPU.** In-game rendering transforms vertices in software and presents through Vulkan.
  Moving GE transform, lighting and skinning to the GPU waits for GE correctness and CPU cost measurements, proven
  against the software reference ([#318](https://github.com/Jstar269/nakagawa-recomp/issues/318)).
- **No networking.** No networking API (`sceNet`, `sceNetAdhoc`, `sceNetApctl`) is implemented. Loading a network
  module (`sceUtilityLoadNetModule`) is refused with `0x80110001` (`SCE_ERROR_UTILITY_INVALID_STATUS`); sockets and
  infrastructure mode return unsupported, with no fake connections
  ([#1139](https://github.com/Jstar269/nakagawa-recomp/issues/1139), [#307](https://github.com/Jstar269/nakagawa-recomp/issues/307)).
- **Controller mapping is global unless you choose otherwise.** Controller settings save your mapping and calibration
  to `input_profile.json`. A custom mapping for one game is optional. The Controls page shows and rebinds the
  keyboard keys ([#1467](https://github.com/Jstar269/nakagawa-recomp/pull/1467)), but you can't change them from a paused game yet: use
  the launcher's Controls page.
- **ATRAC3 sound effects.** A private replay still reports ATRAC3 decode failures for some sound effects, and the
  decoder's behaviour is not yet matched to PSP hardware ([#1096](https://github.com/Jstar269/nakagawa-recomp/issues/1096)).
- **Movie timing.** The number of decoded pictures and the audio offset are not yet fully accounted for
  ([#279](https://github.com/Jstar269/nakagawa-recomp/issues/279)).
- **Not code-signed.** Windows SmartScreen warns on first run because the files have no publisher signature; see
  [Checking your download](#checking-your-download) for how to check a download before you run it. The bundled
  third-party build tools keep their own upstream files and signatures.
- **Fixed: one-frame glitch in skinned characters.** The glitch at the switch from the title screen to the main menu
  was traced to a VFPU3 compare-decode error and fixed in [#822](https://github.com/Jstar269/nakagawa-recomp/pull/822).
  The maintainer confirmed on 2026-10-19 that it no longer appears with the GPU presenter, and
  [#69](https://github.com/Jstar269/nakagawa-recomp/issues/69) closed on 2026-10-20. Please reopen #69 if you see it
  again.

## How to report

If something goes wrong in the player (`bin\nakagawa_player.exe`), note what you did and what the screen said.
Testers who want a full pass/fail run can follow [`SMOKE_TEST.md`](SMOKE_TEST.md).

Send smoke test results and bug reports with the
[Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).
To tell us how far a game gets, post in
[Compatibility reports](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/compatibility-reports), and
for questions, ask in [Help](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/help).

When you report a problem:

1. Say which step in [`docs/SMOKE_TEST.md`](SMOKE_TEST.md) failed and what the error card said. **Copy details** on
   the card puts a short report on the clipboard, including the error code (for example `ISO_CORRUPT`).
2. Attach the build logs from `%LOCALAPPDATA%\Nakagawa\data\logs` (`build_<DISC_ID>_progress.jsonl` and
   `build_<DISC_ID>.log`).
   There are two log folders: builds write to `Nakagawa\data\logs`, games to `Nakagawa\logs`.
   For a problem during play, attach `game-<DISC_ID>-last.txt` from the player's logs folder
   (`%LOCALAPPDATA%\Nakagawa\logs`): the output of the last game session, kept after the game exits. The run before it
   is `game-<DISC_ID>-prev.txt`. **Copy details** on the error card names the file.
3. Read every log before you attach it, to make sure it contains nothing private: no disc images, no game files, no
   decryption keys, and no personal paths you don't want to share.

## Thanks

Thank you to the people outside the maintainer team who contributed to this release:

- [@gufernandess](https://github.com/gufernandess): Windows CMake linking for the player targets
  ([#869](https://github.com/Jstar269/nakagawa-recomp/pull/869)), and the original portable FFmpeg/libavcodec H.264
  backend ([#888](https://github.com/Jstar269/nakagawa-recomp/pull/888)), which inspired the maintained backend that
  landed in [#1087](https://github.com/Jstar269/nakagawa-recomp/pull/1087).
- [@Nitjsefnie](https://github.com/Nitjsefnie): the on-screen keyboard (OSK) utility handlers return the
  PSP-measured state codes ([#1215](https://github.com/Jstar269/nakagawa-recomp/pull/1215), landed with their commits
  in [#1219](https://github.com/Jstar269/nakagawa-recomp/pull/1219)).
- [@Apurvbkt](https://github.com/Apurvbkt): closing out a stale entry in `ISSUES.md`
  ([#1162](https://github.com/Jstar269/nakagawa-recomp/pull/1162)).
