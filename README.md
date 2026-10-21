<!-- markdownlint-disable-file MD041 -- the centred header starts with a div -->
<div align="center">

# Nakagawa Recomp

**Your PSP games, recompiled into native Windows programs.**

[![CI](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/ci.yml)
[![CodeQL](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/Jstar269/nakagawa-recomp/actions/workflows/codeql.yml)
[![Coverage](https://codecov.io/gh/Jstar269/nakagawa-recomp/graph/badge.svg?branch=main)](https://app.codecov.io/gh/Jstar269/nakagawa-recomp)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/15701/badge)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Baseline](https://www.bestpractices.dev/projects/15701/baseline)](https://www.bestpractices.dev/projects/15701)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/Jstar269/nakagawa-recomp/badge)](https://scorecard.dev/viewer/?uri=github.com/Jstar269/nakagawa-recomp)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
[![Platform: Windows 11 x64](https://img.shields.io/badge/platform-Windows%2011%20x64-0078D6.svg)](docs/SETUP.md)

</div>

| ![Nakagawa 3D Showcase demo: a smooth-shaded, textured cube on a dark background, recompiled and rendered by Nakagawa Recomp](docs/images/showcase-ge-scene-v120.png) | ![Nakagawa Breakout Showcase demo: a brick field, score row, paddle and ball, recompiled and rendered by Nakagawa Recomp](docs/images/showcase-breakout-v180.png) |
| :---: | :---: |
| **3D Showcase** | **Breakout Showcase** |

<div align="center"><sub>Two project-written PSP programs, recompiled and running at the PSP's native 480×272. No commercial game is shown or included.</sub></div>

## Why it's different

- **Native, not emulated.** The game's MIPS code is translated ahead of time into C and compiled into a real Windows
  program. A native runtime supplies the PSP's system services, audio and an SDL3 + Vulkan renderer.
- **Honest by design.** When something isn't supported yet, it stops at a named boundary with a plain message. It
  never fakes success.
- **Bring your own game.** No games, firmware, keys or Sony fonts ship here, and no game data is ever downloaded.

## Quick start

1. **Build the player** from a checkout: `mingw32-make player` (Windows 11 x64 with a Vulkan GPU, PowerShell 7.4+ and
   MSYS2 UCRT64; see [`docs/SETUP.md`](docs/SETUP.md)).
2. **Add a game you own.** Drop its `.iso` on `build/nakagawa_player.exe`, or choose **START SETUP WIZARD** (later
   **ADD ANOTHER ISO**).
3. **Unlock it with your own key file.** Supported encrypted executables are decrypted locally
   ([#548](https://github.com/Jstar269/nakagawa-recomp/pull/548), [#550](https://github.com/Jstar269/nakagawa-recomp/pull/550));
   [`docs/YOUR_OWN_GAMES.md`](docs/YOUR_OWN_GAMES.md) shows what goes where.
4. **Press BUILD PACKAGE, then Play.** On a clean PATH, consent once and the player fetches its pinned build tools.
5. **Optional:** text uses the project's own open fonts ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313));
   for the PSP's original typography, import the fonts from your own PSP with
   `python tools/nk_cli.py fonts import <folder>`.

No game at hand? `mingw32-make player showcase` builds the two demos above, and the player finds them on its own.

## Status

| | |
| --- | --- |
| Flagship title | *Hot Shots Tennis: Get a Grip*, the game the project is built around |
| Second title | PAC-MAN Championship Edition plays a stage through generic fixes ([#556](https://github.com/Jstar269/nakagawa-recomp/pull/556)) |
| More titles | In the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)); per-title detail in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) |
| Long-term goals | Native play, a byte-matching decompilation, and a general PSP toolkit ([`docs/PROJECT_MODEL.md`](docs/PROJECT_MODEL.md)) |

## Learn more

[How it works](docs/ARCHITECTURE.md) · [All documentation](docs/README.md) · [Open issues](https://github.com/Jstar269/nakagawa-recomp/issues) ·
[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Code of conduct](CODE_OF_CONDUCT.md)

## License and lineage

GPL-3.0-or-later ([LICENSE](LICENSE)); some files keep GPL-2.0-or-later or their upstream terms. The project began as
a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project) and still contains
code from it and code adapted from [PPSSPP](https://github.com/hrydgard/ppsspp); the ATRAC audio decoders come from
FFmpeg. It is not affiliated with or endorsed by Sony Interactive Entertainment, Clap Hanz, or those upstream authors.
Details: [NOTICE.md](NOTICE.md) and [CREDITS.md](CREDITS.md).

## Dedication

Part of **Project Blitzen**, dedicated to Blitzen, my German Shepherd and companion for 13½ years. The best dog you
could ever have.
