<!-- markdownlint-disable-file MD041 -- the centred header starts with a div -->
<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/brand/nakagawa-mark-dark.svg">
  <img src="docs/images/brand/nakagawa-mark-light.svg" width="96" height="96" alt="Nakagawa mark">
</picture>

# Nakagawa Recomp

**Your PSP games, recompiled into native Windows programs.**

Part of the Nakagawa family, with [hst-decomp](https://github.com/Jstar269/hst-decomp), the matching decompilation of Hot Shots Tennis: Get a Grip.

**[Website](https://jstar269.github.io/nakagawa-recomp/) · [Discussions](https://github.com/Jstar269/nakagawa-recomp/discussions) · [Wiki](https://github.com/Jstar269/nakagawa-recomp/wiki)**

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

[![A rally in Hot Shots Tennis: Get a Grip, running natively in Nakagawa Recomp; play the clip](docs/media/hst-rally-poster.png)](https://jstar269.github.io/nakagawa-recomp/media/hst-rally.mp4)

**[Watch a 28-second rally with sound](https://jstar269.github.io/nakagawa-recomp/media/hst-rally.mp4)** (MP4, 4.5 MB).

<div align="center"><sub>Hot Shots Tennis: Get a Grip belongs to its publisher. This repository contains no game data: you need your own copy of the game, and nothing here downloads one.</sub></div>

| ![Nakagawa 3D Showcase: a brightly textured cube rotating against a dark background in the recompiled demo](docs/images/showcase-ge-scene.gif) | ![Nakagawa Breakout Showcase: the ball moving through a colored brick field above the paddle and score](docs/images/showcase-breakout.gif) |
| :---: | :---: |
| **3D Showcase** | **Breakout Showcase** |

<div align="center"><sub>Two project-written PSP programs, recompiled and running at the PSP's native 480×272. No commercial game is shown in these two demos, and none is included.</sub></div>

## Can I play it?

**Yes, with your own copy of a supported game.** Download v0.0.1, the first release, from its
[release page](https://github.com/Jstar269/nakagawa-recomp/releases/tag/v0.0.1): one ZIP for Windows 11 that already holds the build tools, so it needs
no other download. The [release notes](docs/RELEASE_NOTES_v0.0.1.md) say what works, what doesn't yet, and how to
check the download. You can also [build it yourself](#build-it-yourself).

To play, you need:

- **A Windows 11 PC** (64-bit) with a graphics card that supports Vulkan.
- **8 GB of RAM** (16 GB recommended) and **5 GB of free disk space**, ideally on an SSD. Building a
  game is the heavy part; [System requirements](docs/SETUP.md#system-requirements) has the measured numbers.
- **Your own copy of the game**, as an uncompressed `.iso` disc image. Nothing here downloads games, firmware or fonts.
- **Nothing else, for most discs.** The game's code is encrypted on most retail PSP discs, and Nakagawa decrypts your
  own disc on your PC when you add it, the way PPSSPP does. It never touches firmware.
  [`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md) explains what it does and what to do if a file can't be decrypted.

What to expect: one game plays on the maintainer's setup, one plays a stage, and the rest are untested. The next
section has the detail.

## What works today?

| Game | In the player | What to expect |
| --- | --- | --- |
| *Hot Shots Tennis: Get a Grip* | Playable | The game the project is built around. Its title file ships with Nakagawa, so the player recognizes your own copy: add the disc, BUILD PACKAGE, PLAY. On the maintainer's own copy it plays full matches and Story Mode at the game's own 30 frames per second on the reference PC; movies show black for now ([#1049](https://github.com/Jstar269/nakagawa-recomp/issues/1049)). For games without a title file, going straight from your disc to a running game is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). |
| PAC-MAN Championship Edition | In-game | Reaches its menu and plays a stage on the maintainer's own copy, through general fixes with no game-specific code ([#552](https://github.com/Jstar269/nakagawa-recomp/pull/552), [#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)). The rest of the game hasn't been tried yet. |
| Any other PSP game | Not rated yet | **Experimental** if someone has seen it start, **Untested** if no one has tried it. The player imports it and tells you what's missing. |
| The two showcase demos above | Playable | Two small demos the project wrote itself as tests. They build and run without any game files. |

These are the ratings the player shows on each game's page. [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md#status-words)
explains them and has the full detail for every game and feature. The long-term goals are native play, a
byte-matching decompilation, and a general PSP toolkit ([`docs/PROJECT_MODEL.md`](docs/PROJECT_MODEL.md)).

## Why is it different?

- **A PC program, not an emulator.** The game's MIPS code is translated ahead of time into C and compiled into a real
  Windows program. A built-in runtime stands in for the PSP's system software, sound and graphics (SDL3 and Vulkan).
- **It tells you what it can't do.** When a game needs something that isn't supported yet, the player stops and names
  the missing piece in a plain message. It never pretends something worked when it didn't.
- **Bring your own game.** No games, firmware or system fonts ship here, and no game data is ever downloaded.
- **Offline by default.** Nakagawa connects to the internet only when you agree, and today only to fetch build
  tools ([network use](docs/SETUP.md#network-use)).

## The matching decomp behind it

[![hst-decomp code matched](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FJstar269%2Fhst-decomp%2Fmain%2Fdocs%2Fobjdiff-report.json&query=%24.measures.matched_code_percent&suffix=%25&label=hst-decomp%20code%20matched&color=6E9600)](https://github.com/Jstar269/hst-decomp)

Hot Shots Tennis also has a project of its own: [hst-decomp](https://github.com/Jstar269/hst-decomp), a matching
decompilation that rebuilds the game's code as C and C++ that compiles back to the exact original bytes. The badge
shows how much of the game's code it has matched so far.

The two projects share findings, not code. The decomp's function names can label the C that Nakagawa generates
([#1495](https://github.com/Jstar269/nakagawa-recomp/pull/1495)), and what it shows about the game helps replace
Nakagawa's Hot Shots Tennis workarounds with general fixes
([#363](https://github.com/Jstar269/nakagawa-recomp/issues/363)). No decomp source is copied into Nakagawa,
which is meant for any PSP game, not just this one.

## Build it yourself

This is the developer route; the [release ZIP](#can-i-play-it) is the easier one. New to this? The
[prerequisites](docs/SETUP.md#prerequisites) are one list in [`docs/SETUP.md`](docs/SETUP.md).

1. **Build the player** from a checkout: `mingw32-make player` (Windows 11 x64 with a Vulkan GPU, PowerShell 7.4+ and
   MSYS2 UCRT64).
2. **Add a game you own.** Start `build\nakagawa_player.exe`, then drag its `.iso` into the player window, or choose
   **START SETUP WIZARD**. Later games go in through the **Add a disc image you own** tile in the library.
3. **Let the player check the disc.** If the game's code is encrypted, Nakagawa decrypts your own disc on your PC
   into that game's folder. If a file can't be decrypted, the player tells you which one and why, and you can add a
   decrypted copy from your own disc instead ([`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md)).
4. **Press BUILD PACKAGE, then PLAY.** If your PC doesn't have the build tools yet, consent once and the player
   downloads the exact versions it needs.
5. **Optional:** text uses the project's own open fonts ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313));
   for the PSP's original typography, import the fonts from your own PSP with
   `python tools/nk_cli.py fonts import <folder>`.

No game at hand? `mingw32-make player showcase` (on Windows it needs WSL 2 and PSPDEV: [steps](docs/SETUP.md#showcase-demos-on-windows-wsl-2-and-pspdev)) builds the two demos above, and the player finds them on its own.

## Learn more

[Project website](https://jstar269.github.io/nakagawa-recomp/) · [How it works](docs/ARCHITECTURE.md#pipeline) · [All documentation](docs/README.md) · [Open issues](https://github.com/Jstar269/nakagawa-recomp/issues) ·
[Contributing](CONTRIBUTING.md) · [Accessibility](ACCESSIBILITY.md) · [Getting help](SUPPORT.md) · [Security](SECURITY.md) ·
[Code of conduct](CODE_OF_CONDUCT.md) · [AI use](docs/AI_USAGE.md) ·
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
code from it and code adapted from [PPSSPP](https://github.com/hrydgard/ppsspp); the ATRAC audio and H.264 video
decoders come from FFmpeg. It is not endorsed by those upstream authors. Details: [NOTICE.md](NOTICE.md) and
[CREDITS.md](CREDITS.md).

## Legal

Nakagawa Recomp is an independent fan project. It is not affiliated with or endorsed by Sony Interactive
Entertainment, Clap Hanz, or any game publisher. PlayStation and PSP are trademarks of Sony Interactive
Entertainment Inc., and game titles are trademarks of their owners; they are named here only to say what the project
works with. This repository contains no game files, firmware or system fonts. Nakagawa decrypts your own disc
locally with the public game key tables PPSSPP ships; it never downloads games and never decrypts firmware.

Rights holders with a concern about anything here can contact the maintainer privately through
[GitHub private reporting](https://github.com/Jstar269/nakagawa-recomp/security/advisories/new) or the contact on
the [maintainer's profile](https://github.com/Jstar269). We aim to respond promptly and will remove material where
appropriate.

## Dedication

Dedicated to Blitzen: One of the greatest pets you could ever have. A companion from beginning to end.

An absolute sweetheart.

2012-2026
