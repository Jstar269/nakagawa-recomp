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
[![CodeQL](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/codeql.yml)
[![Coverage (Python tooling and native core tests)](https://codecov.io/gh/Jstar269/nakagawa-recomp/graph/badge.svg?branch=main)](https://app.codecov.io/gh/Jstar269/nakagawa-recomp)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15701/badge)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Baseline](https://www.bestpractices.dev/projects/15701/baseline)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/Jstar269/nakagawa-recomp/badge)](https://scorecard.dev/viewer/?uri=github.com/Jstar269/nakagawa-recomp)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform: Windows 11 x64](https://img.shields.io/badge/platform-Windows%2011%20x64-0078D6.svg)](docs/SETUP.md)

</div>

| ![Nakagawa 3D Showcase: a brightly textured cube rotating against a dark background in the recompiled demo](docs/images/showcase-ge-scene.gif) | ![Nakagawa Breakout Showcase: the ball moving through a colored brick field above the paddle and score](docs/images/showcase-breakout.gif) |
| :---: | :---: |
| **3D Showcase** | **Breakout Showcase** |

<div align="center"><sub>Two project-written PSP programs, recompiled and running at the PSP's native 480×272. No commercial game is shown in these two demos, and none is included.</sub></div>

## Screenshots

| ![Title screen of Hot Shots Tennis: Get a Grip running natively in Nakagawa Recomp](docs/media/hst-title-screen.png) |
| :---: |
| **Title screen** |

<div align="center"><sub>Hot Shots Tennis: Get a Grip belongs to its publisher. This repository contains no game data: you need your own copy of the game, and nothing here downloads one.</sub></div>

## Why it's different

- **Native, not emulated.** The game's MIPS code is translated ahead of time into C and compiled into a real Windows
  program. A native runtime supplies the PSP's system services, audio and an SDL3 + Vulkan renderer.
- **Honest by design.** When something isn't supported yet, it stops at a named boundary with a plain message. It
  never fakes success.
- **Bring your own game.** No games, firmware, keys or Sony fonts ship here, and no game data is ever downloaded.
- **Offline by default.** Nakagawa connects to the internet only when you agree, and today only to fetch build
  tools ([network use](docs/SETUP.md#network-use)).

## Quick start

New to this? The [prerequisites](docs/SETUP.md#prerequisites) are one list in [`docs/SETUP.md`](docs/SETUP.md).

1. **Build the player** from a checkout: `mingw32-make player` (Windows 11 x64 with a Vulkan GPU, PowerShell 7.4+ and
   MSYS2 UCRT64).
2. **Add a game you own.** Start `build\nakagawa_player.exe`, then drag its `.iso` into the player window, or choose
   **START SETUP WIZARD** (later **ADD ANOTHER ISO**).
3. **Unlock it with your own key file.** Supported encrypted executables are decrypted locally
   ([#548](https://github.com/Jstar269/nakagawa-recomp/pull/548), [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550));
   [`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md) shows what goes where.
4. **Press BUILD PACKAGE, then Play.** On a clean PATH, consent once and the player fetches its pinned build tools.
5. **Optional:** text uses the project's own open fonts ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313));
   for the PSP's original typography, import the fonts from your own PSP with
   `python tools/nk_cli.py fonts import <folder>`.

No game at hand? `mingw32-make player showcase` (on Windows it needs WSL 2 and PSPDEV: [steps](docs/SETUP.md#showcase-demos-on-windows-wsl-2-and-pspdev)) builds the two demos above, and the player finds them on its own.

## Status

| | |
| --- | --- |
| Flagship title | *Hot Shots Tennis: Get a Grip*, the game the project is built around |
| Second title | PAC-MAN Championship Edition plays a stage through generic fixes ([#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)) |
| More titles | In the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)); per-title detail in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) |
| Long-term goals | Native play, a byte-matching decompilation, and a general PSP toolkit ([`docs/PROJECT_MODEL.md`](docs/PROJECT_MODEL.md)) |

## Learn more

[How it works](docs/ARCHITECTURE.md#pipeline) · [All documentation](docs/README.md) · [Open issues](https://github.com/Jstar269/nakagawa-recomp/issues) ·
[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Code of conduct](CODE_OF_CONDUCT.md)

## License and lineage

GPL-3.0-or-later ([LICENSE](LICENSE)); some files keep GPL-2.0-or-later or their upstream terms. The project began as
a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project) and still contains
code from it and code adapted from [PPSSPP](https://github.com/hrydgard/ppsspp); the ATRAC audio decoders come from
FFmpeg. It is not affiliated with or endorsed by Sony Interactive Entertainment, Clap Hanz, or those upstream authors.
Details: [NOTICE.md](NOTICE.md) and [CREDITS.md](CREDITS.md).

## Dedication

Dedicated to Blitzen: One of the greatest pets you could ever have. A companion from beginning to end.

An absolute sweetheart.

2012-2026
