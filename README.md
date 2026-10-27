<!-- markdownlint-disable-file MD041 -- the centred header starts with a div -->
<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/brand/nakagawa-mark-dark.svg">
  <img src="docs/images/brand/nakagawa-mark-light.svg" width="96" height="96" alt="Nakagawa mark">
</picture>

# Nakagawa Recomp

**Your PSP games, recompiled into native Windows programs.**

Part of the Nakagawa family, with [hst-decomp](https://github.com/Jstar269/hst-decomp), the matching decompilation of Hot Shots Tennis: Get a Grip.

[![CI](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform: Windows 11 x64](https://img.shields.io/badge/platform-Windows%2011%20x64-0078D6.svg)](docs/SETUP.md)

</div>

## What is this?

Nakagawa Recomp turns a PSP game's code into a real Windows program, much like a proper PC port. It is not an
emulator: an emulator pretends to be a PSP while the game runs, but here the game's own code is translated once,
ahead of time, and compiled into a program your PC runs directly.

The same approach is behind [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) (built with
[N64Recomp](https://github.com/N64Recomp/N64Recomp)) and [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp),
the PC port of Sonic Unleashed (built with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)). Nakagawa
brings it to the PlayStation Portable. It is an early, work-in-progress project: one game is far along, and others
are just starting.

## See it running

| ![Title screen of Hot Shots Tennis: Get a Grip running natively in Nakagawa Recomp](docs/media/hst-title-screen.png) |
| :---: |
| **Title screen** |

<div align="center"><sub>Hot Shots Tennis: Get a Grip belongs to its publisher. This repository contains no game data: you need your own copy of the game, and nothing here downloads one.</sub></div>

| ![Nakagawa 3D Showcase: a brightly textured cube rotating against a dark background in the recompiled demo](docs/images/showcase-ge-scene.gif) | ![Nakagawa Breakout Showcase: the ball moving through a colored brick field above the paddle and score](docs/images/showcase-breakout.gif) |
| :---: | :---: |
| **3D Showcase** | **Breakout Showcase** |

<div align="center"><sub>Two project-written PSP programs, recompiled and running at the PSP's native 480×272. No commercial game is shown in these two demos, and none is included.</sub></div>

## Can I play it?

**Not as a simple download yet.** There is no release to download today. The first one, v0.0.1, is being prepared
([#1094](https://github.com/Jstar269/nakagawa-recomp/pull/1094), [#278](https://github.com/Jstar269/nakagawa-recomp/issues/278)),
and no date is set. Until then you build the player from source yourself: see [Build it yourself](#build-it-yourself).

To play, you need:

- **A Windows 11 PC** (64-bit) with a graphics card that supports Vulkan.
- **8 GB of RAM** (16 GB recommended) and **5 GB of free disk space**, ideally on an SSD. Building a
  game is the heavy part; [System requirements](docs/SETUP.md#system-requirements) has the measured numbers.
- **Your own copy of the game**, as an uncompressed `.iso` disc image. Nothing here downloads games, firmware or fonts.
- **Decrypted files from your own copy** if the game's code is encrypted, as it is on most retail PSP discs.
  Nakagawa does not decrypt games. [`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md) explains what goes where.

What to expect: one game is far along, one plays a stage, and the rest are untested. The next section has the detail.

## What works today?

| Game | Status | What that means today |
| --- | --- | --- |
| *Hot Shots Tennis: Get a Grip* | Boots to a named boundary | The game the project is built around. With the public player, it starts and then stops at a named point that isn't supported yet. Going from a disc image to a running game with no manual setup is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). |
| PAC-MAN Championship Edition | Partially works | Reaches its menu and plays a stage on the maintainer's own copy, through general fixes with no game-specific code ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)). Not the whole game. |
| Any other PSP game | Experimental or Untested | **Experimental** if someone has seen it boot, **Untested** if there is no record. The player imports it and names what is missing. |
| The two showcase demos above | Partially works | Project-written test programs that build and run without any game files. |

The status words are defined in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md#status-words), which also has the full
per-game and per-feature detail. The long-term goals are native play, a byte-matching decompilation, and a general
PSP toolkit ([`docs/PROJECT_MODEL.md`](docs/PROJECT_MODEL.md)).

## Why is it different?

- **A PC program, not an emulator.** The game's MIPS code is translated ahead of time into C and compiled into a real
  Windows program. A built-in runtime stands in for the PSP's system software, sound and graphics (SDL3 and Vulkan).
- **It tells you what it can't do.** When a game needs something that isn't supported yet, the player stops and names
  the missing piece in a plain message. It never pretends something worked when it didn't.
- **Bring your own game.** No games, firmware, keys or system fonts ship here, and no game data is ever downloaded.
- **Offline by default.** Nakagawa connects to the internet only when you agree, and today only to fetch build
  tools ([network use](docs/SETUP.md#network-use)).

## The matching decomp behind it

[![hst-decomp code matched](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FJstar269%2Fhst-decomp%2Fmain%2Fdocs%2Fobjdiff-report.json&query=%24.measures.matched_code_percent&suffix=%25&label=hst-decomp%20code%20matched&color=6E9600)](https://github.com/Jstar269/hst-decomp)

Hot Shots Tennis also has a project of its own: [hst-decomp](https://github.com/Jstar269/hst-decomp), a matching
decompilation that rebuilds the game's code as C and C++ that compiles back to the exact original bytes. On
27 October 2026 it had 35.9% of the game's code matched (12,199 of 19,634 functions); the badge shows today's
figure.

The two projects share findings, not code. The decomp's function names can label the C that Nakagawa generates
([#1495](https://github.com/Jstar269/nakagawa-recomp/pull/1495)), and what it shows about the game helps replace
Nakagawa's Hot Shots Tennis workarounds with general fixes
([#363](https://github.com/Jstar269/nakagawa-recomp/issues/363)). No decomp source is copied into Nakagawa,
which is meant for any PSP game, not just this one.

## How is it made?

Nakagawa Recomp is developed with AI coding agents, mainly Claude Code and Codex, directed by the maintainer. AI
output is treated like any other untrusted contribution: a person must understand and verify it, and it lands only
after passing the same checks as everything else.

- **Strict automatic tests** run on every pull request, chosen by what it changes, and must pass before it merges
  ([`docs/CI.md`](docs/CI.md)).
- **Publication checks** keep game data, keys and private files out of the repository.
- **Every commit made with AI help says so**, in an `Assisted-by:` line naming the tool.

The full rules, including what may never be sent to an AI service, are in [`docs/AI_USAGE.md`](docs/AI_USAGE.md).

## Build it yourself

This is the developer route, and today the only way to play. New to this? The
[prerequisites](docs/SETUP.md#prerequisites) are one list in [`docs/SETUP.md`](docs/SETUP.md).

1. **Build the player** from a checkout: `mingw32-make player` (Windows 11 x64 with a Vulkan GPU, PowerShell 7.4+ and
   MSYS2 UCRT64).
2. **Add a game you own.** Start `build\nakagawa_player.exe`, then drag its `.iso` into the player window, or choose
   **START SETUP WIZARD** (later **ADD ANOTHER ISO**).
3. **Add decrypted files if the game is encrypted.** Nakagawa does not decrypt games; it uses decrypted files from
   your own copy, and the player names any that are missing ([`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md)
   shows what goes where).
4. **Press BUILD PACKAGE, then Play.** On a clean PATH, consent once and the player fetches its pinned build tools.
5. **Optional:** text uses the project's own open fonts ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313));
   for the PSP's original typography, import the fonts from your own PSP with
   `python tools/nk_cli.py fonts import <folder>`.

No game at hand? `mingw32-make player showcase` (on Windows it needs WSL 2 and PSPDEV: [steps](docs/SETUP.md#showcase-demos-on-windows-wsl-2-and-pspdev)) builds the two demos above, and the player finds them on its own.

## Learn more

[Project website](https://jstar269.github.io/nakagawa-recomp/) · [How it works](docs/ARCHITECTURE.md#pipeline) · [All documentation](docs/README.md) · [Open issues](https://github.com/Jstar269/nakagawa-recomp/issues) ·
[Contributing](CONTRIBUTING.md) · [Accessibility](ACCESSIBILITY.md) · [Getting help](SUPPORT.md) · [Security](SECURITY.md) ·
[Code of conduct](CODE_OF_CONDUCT.md) ·
[hst-decomp](https://github.com/Jstar269/hst-decomp)

## Project health

[![CodeQL](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/codeql.yml)
[![Coverage (Python tooling and native core tests)](https://codecov.io/gh/Jstar269/nakagawa-recomp/graph/badge.svg?branch=main)](https://app.codecov.io/gh/Jstar269/nakagawa-recomp)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15701/badge)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Baseline](https://www.bestpractices.dev/projects/15701/baseline)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/Jstar269/nakagawa-recomp/badge)](https://scorecard.dev/viewer/?uri=github.com/Jstar269/nakagawa-recomp)

## License and lineage

GPL-3.0-or-later ([LICENSE](LICENSE)); some files keep GPL-2.0-or-later or their upstream terms. The project began as
a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project) and still contains
code from it and code adapted from [PPSSPP](https://github.com/hrydgard/ppsspp); the ATRAC audio decoders come from
FFmpeg. It is not endorsed by those upstream authors. Details: [NOTICE.md](NOTICE.md) and [CREDITS.md](CREDITS.md).

## Legal

Nakagawa Recomp is an independent fan project. It is not affiliated with or endorsed by Sony Interactive
Entertainment, Clap Hanz, or any game publisher. PlayStation and PSP are trademarks of Sony Interactive
Entertainment Inc., and game titles are trademarks of their owners; they are named here only to say what the project
works with. This repository contains no game files, keys, firmware or system fonts, and Nakagawa does not decrypt
games.

Rights holders with a concern about anything here can contact the maintainer privately through
[GitHub private reporting](https://github.com/Jstar269/nakagawa-recomp/security/advisories/new) or the contact on
the [maintainer's profile](https://github.com/Jstar269). We aim to respond promptly and will remove material where
appropriate.

## Dedication

Dedicated to Blitzen: One of the greatest pets you could ever have. A companion from beginning to end.

An absolute sweetheart.

2012-2026
