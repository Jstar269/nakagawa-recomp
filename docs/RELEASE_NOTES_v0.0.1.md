# Nakagawa Recomp v0.0.1 Release Notes

Status: DRAFT. The maintainer publishes releases; agents never create tags or releases.
Release qualification authority: [issue #278](https://github.com/Jstar269/nakagawa-recomp/issues/278).
Audience: players and contributors trying the first public build.

## What v0.0.1 is

Nakagawa Recomp is an experimental static recompiler that translates user-supplied PlayStation Portable (PSP) executables into C, links them with a native C runtime, and runs the resulting binary on Windows via SDL3 and Vulkan. Executables must be plain already or decrypted by the built-in boundary using the user's own local key file. v0.0.1 is the first public build, featuring a standalone desktop player (`build/nakagawa_player.exe`), ahead-of-time (AOT) package building, synthetic verification suites, and a fail-closed runtime architecture where unsupported, missing, or unimplemented behavior halts at a named semantic boundary with a clear message.

## What you can do

- **Identify discs directly from `PARAM.SFO`**: Drag and drop or select a PSP `.iso` image in the player; the player reads `PSP_GAME/PARAM.SFO` directly in C to extract the title and Disc ID, checking it against supported title profiles without using an update SFO or volume ID ([#426](https://github.com/Jstar269/nakagawa-recomp/pull/426)).
- **Play source-owned showcase demos**: Run project-authored PSPDEV demos (a lit, textured 3D scene and Breakout with paddle input, sound effects, and savedata high score) built from source into deterministic plaintext ISOs and packages, discovered automatically with authentic icons in the player ([#477](https://github.com/Jstar269/nakagawa-recomp/pull/477)).
- **Configure and calibrate controllers in the player**: Open Controller Settings (`VIEW_CONTROLLER_SETTINGS`) from Settings to remap 14 digital PSP controls and the analog stick with conflict detection, deadzone and trigger threshold calibration, live input monitoring, and atomic profile persistence to `input_profile.json` ([#455](https://github.com/Jstar269/nakagawa-recomp/pull/455)).
- **Build runtime packages with live progress and cancel**: Click **BUILD PACKAGE** (or **REBUILD PACKAGE**) in the player to recompile a title via `tools/nk_cli.py build-package`, with real-time 4-stage progress (Preflight, Extract, Compile, Package), elapsed time display, in-app cancellation ending the child process tree, and an in-game performance HUD overlay toggled via `F1` ([#465](https://github.com/Jstar269/nakagawa-recomp/pull/465), [#469](https://github.com/Jstar269/nakagawa-recomp/pull/469)).
- **Install missing build prerequisites after one consent**: On a clean `PATH`, the player offers one consent card for the pinned tools needed by **BUILD PACKAGE**, shows each tool's version, host, size, licence and the total, then downloads and verifies the declared artifacts before resuming the build ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)). You can also install Python, GNU Make and the MSYS2 UCRT64 compiler yourself.
- **Display real disc icons and persist settings**: Library cards show the disc's authentic `ICON0.PNG` and dimmed `PIC1.PNG` background loaded safely at runtime from the ISO with fallback to monogram badges; settings (resolution scale, frame cap, VSync, reduce motion) persist atomically to `settings.json` across restarts, alongside truthful status text ([#458](https://github.com/Jstar269/nakagawa-recomp/pull/458)).
- **Decrypt with your own key or supply plain executables**: The built-in boundary decrypts supported `EBOOT.BIN` files and encrypted PRX modules with a user-supplied local key file, writing `EBOOT.elf` and decoded modules into `<user data>/titles/<DISC_ID>/decrypted/`. The project ships no keys; modules with missing key entries are named and fail closed. You can also supply plain `EBOOT.elf` and required PRXs in that folder ([#548](https://github.com/Jstar269/nakagawa-recomp/pull/548), [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550), [#428](https://github.com/Jstar269/nakagawa-recomp/pull/428)).
- **Serve game assets directly through archive-backed VFS**: The runtime serves game data directly from validated `.xb` archives using a read-only archive-backed virtual filesystem (`sceIoOpen`, `sceIoGetstat`, `sceIoDopen`), eliminating the need to pre-extract loose files before running ([#475](https://github.com/Jstar269/nakagawa-recomp/pull/475)).
- **Import PSP system fonts into a validated local cache**: Import dumped PSP console firmware fonts (`jpn0.pgf`, `ltn0.pgf`–`ltn15.pgf`, `kr0.pgf`) into `<user data>/fonts/v1/` using `python tools/nk_cli.py fonts import <folder>`, with structural header validation, atomic manifest generation, and the project-authored public PGF reader ([#441](https://github.com/Jstar269/nakagawa-recomp/pull/441), [#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)).

## Public verification you can reproduce

The public, game-input-free verification suite and the media subsystem are the parts of v0.0.1 you can
confirm yourself, with no retail ISO, PRX, save, key, or decrypted module:

- **Native player application:** `mingw32-make player` compiles `build/nakagawa_player.exe` using SDL3. It supports drag-and-drop and file-dialog ISO loading, parses the ISO9660 PVD and `PARAM.SFO` metadata in C, performs asset census staging, and handles gamepad navigation.
- **Display smoke verification:** `mingw32-make display-smoke` and `mingw32-make display-smoke-player` prove the two-phase pipeline, loader, NID imports, vblank delivery, display latch, and native player child runtime launch.
- **Production smoke:** `mingw32-make production-smoke` generates a synthetic PSP PRX, statically recompiles it MIPS-to-C-to-native, and verifies and runs the AOT output.
- **Fail-closed dispatch:** `mingw32-make production-smoke-gap` proves the fail-closed dispatch path when a synthetic function is intentionally omitted from AOT emission.
- **Differential cosimulation:** `mingw32-make cosim-selftest` compares AOT and interpreter traces, writes, memory, and architectural state over synthetic cases.
- **Platform ladder:** `mingw32-make platform-ladder` exercises relocations, scheduler threading, scalar FPU, and filesystem semantics across synthetic workloads.
- **Core runtime selftests:** `selftest`, `sched-selftest`, `hle-thread-selftest`, and `public-safe-verify` pass on the verified Windows host.
- **Media subsystem:** PSMF demuxing and H.264 video decoding pipelines process video cutscenes.

From a clean Windows checkout with the supported toolchain, the four headline commands are:

```powershell
mingw32-make --no-print-directory production-smoke
mingw32-make --no-print-directory production-smoke-gap
mingw32-make --no-print-directory player
mingw32-make --no-print-directory display-smoke
```

The commands generate their synthetic inputs beneath the ignored `build/` tree.

## What v0.0.1 is not

- **No bundled games**: This repository does not include game binaries, proprietary game assets, firmware modules, decryption keys, or private oracle traces. Nakagawa Recomp never downloads games or game data. With your consent, it downloads only the build tools declared in the prerequisite manifest. Users must supply their own lawfully obtained PSP game disc image in uncompressed standard `.iso` format ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Decryption needs your own key file**: Nakagawa Recomp is an experimental static recompiler, not a finished consumer emulator. No commercial PSP game is claimed playable end-to-end from an ISO alone. Most retail PSP executables (`EBOOT.BIN` and `.prx` modules) are encrypted. The player and `nk_cli` include a built-in boundary that decrypts supported executables and modules when the user supplies a matching local key file, writing them into `<user data>/titles/<DISC_ID>/decrypted/`; the project ships no keys. If a module's key entry is absent, the player names that module and fails closed. PGD-protected content is unsupported; broader ISO-to-Play support is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). The supported KIRK boundary is covered by [#548](https://github.com/Jstar269/nakagawa-recomp/pull/548) and [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550).
- **Experimental titles**: A valid PSP disc that has no profile yet is imported as **Experimental**. The card notes that compatibility is unknown, checklist items indicate what is missing, and Play stays unavailable until a matching runtime package exists. PAC-MAN Championship Edition reaches its menu and plays a stage through generic fixes with no title-specific branch ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)); full-game compatibility and other title candidates are in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). Non-PSP images are refused.
- **Fonts need the user's own firmware fonts**: Authentic in-game typography requires Sony firmware font files (`jpn0.pgf`, `ltn0.pgf`) dumped from a real PSP console (`flash0:/font/`). These proprietary files cannot legally be bundled and must be user-provided; import them into the validated local cache via `python tools/nk_cli.py fonts import <folder>` ([#300](https://github.com/Jstar269/nakagawa-recomp/issues/300)). Public builds read these fonts with the project-authored PGF reader ([#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)); the project-authored open-font converter is in the works ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).
- **Windows is the supported player platform and Linux is development-only**: Only Windows 11 x64 is supported today for the desktop player runtime. Linux host runtime/player support is in progress after backend seams are portable ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306)), followed by macOS (Apple Silicon) ([#329](https://github.com/Jstar269/nakagawa-recomp/issues/329)) and Android ARM64 ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)). Linux/WSL currently serves as a development platform for building the player and portable core and running native tests ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306), [#471](https://github.com/Jstar269/nakagawa-recomp/pull/471)).
- **Audio output requires a connected audio device**: Public builds drive one SDL3 audio stream per `sceAudio` channel ([#301](https://github.com/Jstar269/nakagawa-recomp/issues/301), [#411](https://github.com/Jstar269/nakagawa-recomp/pull/411)); with no audio device the game runs silently after one message. The SAS hardware conformance group is planned ([#311](https://github.com/Jstar269/nakagawa-recomp/issues/311)).

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

## Known issues

- **One-frame skinned-character geometry corruption**: A single frame of skinned character geometry can render corrupted at the title-to-main-menu transition, then recovers. Root cause is not established, so other titles' skinned models may be affected. The maintainer deferred this known issue to v0.0.2 with disclosure; investigation remains in the works ([#69](https://github.com/Jstar269/nakagawa-recomp/issues/69), [decision](https://github.com/Jstar269/nakagawa-recomp/issues/69#issuecomment-5858768374)).
- **Second-title scope**: PAC-MAN Championship Edition reaches its menu and plays a stage through generic fixes with no title-specific branches ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)). This is not full-game compatibility; other candidates stop at named unsupported imports and further title bring-up is in the works under [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308).
- **GPU vertex transform, lighting, and skinning**: In-game rendering currently uses software transformation with Vulkan presentation; moving GE transform, lighting, and skinning to Vulkan waits for GE correctness and CPU cost measurements behind software-reference differential proof ([#318](https://github.com/Jstar269/nakagawa-recomp/issues/318)).
- **Networking APIs unimplemented**: All networking APIs (`sceNet`, `sceNetAdhoc`, `sceNetApctl`) are unimplemented. Network module loading (`sceUtilityLoadNetModule`) returns controlled refusal (`0x80110001` `SCE_ERROR_UTILITY_INVALID_STATUS`); sockets and infrastructure return unsupported with no fake success or phantom connections ([#281](https://github.com/Jstar269/nakagawa-recomp/issues/281), [#307](https://github.com/Jstar269/nakagawa-recomp/issues/307)).
- **ISO-to-Play still has title boundaries**: The player can install pinned build prerequisites after one consent, then build and validate a package for a supported title ([#547](https://github.com/Jstar269/nakagawa-recomp/pull/547)). It does not make every ISO playable: unsupported profiles/imports and PGD-protected data fail closed with named guidance; broader title intake is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).
- **Controller remapping is global**: Controller settings support global host input configuration and calibration saved to `input_profile.json`; per-title profiles and advanced gamepad-only binding capture are in the works ([#357](https://github.com/Jstar269/nakagawa-recomp/issues/357)).

## How to report

If you encounter issues while running the native desktop player (`build/nakagawa_player.exe`), follow the pass/fail verification procedure in [`SMOKE_TEST.md`](SMOKE_TEST.md).

Submit smoke test results and bug reports using the [Smoke Test Report issue template](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=smoke_test_report.yml).

When reporting a problem:

1. Note the specific step in [`docs/SMOKE_TEST.md`](SMOKE_TEST.md) that failed and the exact error code or message displayed on the card (e.g. `ISO_CORRUPT`).
2. Attach build logs from `%LOCALAPPDATA%\Nakagawa\data\logs` (`build_<DISC_ID>_progress.jsonl` and `build_<DISC_ID>.log`) and console output from the terminal.
3. Check all logs before attaching to ensure they contain nothing private: no disc images, no game files, no decryption keys, and no personal paths you do not want to share.
