# Playing your own games

Status: CURRENT. This guide explains what the native player needs in order to
play a PSP game you own, what it does for you automatically, and what is still
in the works.

## What you need

- **Windows 11 x64** with a Vulkan-capable graphics card and current GPU
  drivers. Windows is the only supported player platform today; other platforms
  are in the works ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306), [#329](https://github.com/Jstar269/nakagawa-recomp/issues/329), [#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).
- **A disc image of a game you own, in uncompressed standard `.iso` format.**
  The project does not make or supply images. Whether you may make an image of
  a disc, or of a digital purchase, depends on where you live and on the terms
  you accepted. Check both. Nakagawa Recomp never downloads games and contains no
  game data. Don't use copies you don't own.
- **The game's executable in unencrypted form, for most commercial games.**
  Nakagawa recompiles the game's own program code into a native program on your
  computer, so it has to be able to read that code. Most retail PSP executables
  (`EBOOT.BIN` and some `.prx` modules) are encrypted. You can either supply
  your own local key file to the built-in decryption boundary (see below) or
  put your own unencrypted files in the per-title folder.
- **PSP system fonts (optional but recommended for text).** Authentic in-game
  typography requires Sony firmware font files (`jpn0.pgf`, `ltn0.pgf`) dumped
  from a real PSP console. The project does not include or distribute these
  files; you provide them from your own PSP (see the import step below)
  ([#300](https://github.com/Jstar269/nakagawa-recomp/issues/300));
  a clean-room open-font converter is in the works
  ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).

Expectation: adding an ISO identifies the disc and shows a compatibility
checklist of what will and will not work. **BUILD PACKAGE** recompiles the
title in front of you, and **Play** starts it once the package validates; for
most retail discs you also supply an unencrypted executable. Both steps
compile C on your machine, so they need the MSYS2 UCRT64 developer toolchain
([`SETUP.md`](SETUP.md)); one-click play straight from an ISO without it is in
the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).

## What the player does automatically

1. Add the ISO: drag it into the player window, or click **Add Game**. If the
   disc is already in your library, re-adding it keeps its completed extraction
   and staged files as long as the disc ID, version, and image size match.
2. The player reads the disc's own `PARAM.SFO` to identify the title and disc ID.
   At start-up, the player also loads every title manifest in `<user data>/manifests`
   (up to 8, in alphabetical order), so any title recognized from a user-supplied
   manifest is identified without extra options.
3. It checks the executable:
   - An **unencrypted** executable, including discs that carry an unencrypted
     `BOOT.BIN`, is selected automatically. Homebrew and the project's showcase
     demos are in this group.
   - For an **encrypted** executable, the game card says so and names the
     folder where the player looks for your unencrypted files (`<user data>/titles/<DISC_ID>/decrypted/`).
     If an experimental profile was created while the executable was still
     encrypted, supplying `EBOOT.elf` in that folder is picked up automatically
     when building.
4. Once an unencrypted executable is available, click **Build package**.
   - If the manifest reads BSS metadata from the disc's `~PSP` executable
     header, `build-package` extracts it directly from the disc image; you do
     not need to provide a separate header file.
   - The compiled package ships the decrypted modules it compiled against in
     `<package>/modules`, and the launcher sets `SR_MODULE_DIR` to point there
     so launches are self-contained.
   - A run starts at the title's declared fallback entry when one is defined
     (`runtime_bindings.fallback_entry`), or at the executable entry point.
   - The player recompiles the game, shows each stage, and turns on **Play** when the
     package is ready. Anything it can't handle yet stops the build with a message
     naming what is missing and its tracking issue. It never pretends a build
     succeeded.

## Supplying unencrypted files

Some people use third-party tools to produce unencrypted copies of a game's
executable and modules from their own copy. Some run on your own PSP, others on
a PC. The project does not include, endorse or link to these tools and cannot
check what they do. It contains no decryption keys and needs none.

This is not legal advice. Laws on decrypting and copying games differ by
country. Check the rules where you live.

Put the unencrypted files in the per-title folder the player shows on the game
card:

```text
<user data>/titles/<DISC_ID>/decrypted/
├── EBOOT.elf        (the game's main executable)
└── <module>.prx     (only modules the player or build says are required)
```

On Windows, `<user data>` normally is `%LOCALAPPDATA%\Nakagawa\data`. The
player resolves `%LOCALAPPDATA%` first, then Windows `FOLDERID_LocalAppData`,
then `%APPDATA%`; it never falls back to the profile root. If the old
`%USERPROFILE%\Nakagawa\data` exists and the current root does not, the player
and Doctor report its exact path so you can move it manually. A usable file
is a 32-bit little-endian MIPS ELF. It starts with `7F 45 4C 46`, and the
build tools check that it is a MIPS ELF before using it. A file that starts with
`~PSP` or `~SCE` is still encrypted, and the player will say so. The player
picks up the folder automatically the next time it checks the game.

Decrypted guest modules can be named either after their file name on the disc
(for example `psmf.prx`) or after their manifest module name (for example
`scePsmf_library.prx`). If a module is missing or not a plain decrypted ELF, the
build error message displays both names.

### Supplying title manifests

If your game requires an external title manifest overlay, place the `.json` manifest in:

```text
<user data>/manifests/
└── <manifest_name>.json
```

The player loads every `.json` manifest in this directory on start-up (sorted
alphabetically, up to 8 manifests). The command-line tool `tools/nk_cli.py
build-package` also inspects this folder after checking repository titles,
resolving the title identically.

## Keep it to your own games

- Only use games you own.
- Don't share decrypted files, generated packages or disc images. Everything
  Nakagawa builds stays in your user data folder.
- Issues, pull requests and project discussions must never contain game files,
  keys, or links to downloads or decryption tools.

## When something is not supported yet

The software follows a **fail-closed** policy: when something is unsupported,
missing, or unimplemented, it halts cleanly at a named semantic boundary and
tells you why. It never fakes success, simulates a phantom state, or silently
crashes.

- **Game not in the built-in list:** a valid PSP disc that has no profile yet is
  imported as **Experimental**. The card notes that compatibility is unknown,
  checklist items indicate what is missing, and Play stays unavailable until a
  matching runtime package exists. Second-title verification is in the works
  ([#285](https://github.com/Jstar269/nakagawa-recomp/issues/285)) and generic
  title intake is in the works
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). Images that
  are not PSP game discs are refused.
- **PlayStation Store package:** a file whose first bytes are the `PBP` package
  magic (a store download renamed to `.iso`, for example) is not a disc image,
  however it is named. It is refused before any disc reading with the boundary
  `PBP_PACKAGE_UNSUPPORTED` and a sentence naming the title and disc ID the
  package carries. Only the package's own `PARAM.SFO` entry is read to form that
  sentence; no other package section is opened, installed, or executed.
- **Encrypted executable:** with a matching local key file, the built-in boundary
  decrypts supported encrypted executables and selected/required PRX modules into the
  per-title folder ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). The
  checklist names missing key entries; if an unencrypted `BOOT.BIN` is present on disc
  or a valid user-supplied `EBOOT.elf` is found in that folder, it is selected
  automatically. The project ships no keys; PGD/amctrl remains unsupported.
- **Missing compiled runtime:** if a game has been identified and staged but no
  matching valid package exists in `<user data>/packages/<DISC_ID>/`, the
  preflight checklist reports `RUNTIME_PACKAGE` as missing and the card offers
  **BUILD PACKAGE**; Play stays disabled until a valid package is built
  ([#465](https://github.com/Jstar269/nakagawa-recomp/pull/465)). The one
  exception is a non-experimental catalog title whose developer runtime the
  launcher resolves, which is playable without a package
  ([#483](https://github.com/Jstar269/nakagawa-recomp/pull/483)); a stale
  package shows **REBUILD PACKAGE** and an incompatible one shows
  `PACKAGE INCOMPATIBLE`. If the package-check worker cannot start, the card
  shows `PACKAGE CHECK FAILED` with a `RETRY PACKAGE CHECK` action; background
  rescans retry after a bounded backoff.
- **Missing firmware fonts:** if in-game fonts cannot be found, the player notes
  that the PSP font `jpn0.pgf` is missing and directs you to run
  `python tools/nk_cli.py fonts import <folder>`
  ([#300](https://github.com/Jstar269/nakagawa-recomp/issues/300)), rather than
  substituting mismatched system fonts that cause text clipping. Guest
  translated `libfont` startup runs through its guest entry; when that startup is
  unavailable (an untranslated entry, or `SR_REAL_MODULE_START=0`), the runtime
  reports `LIBFONT_STARTUP_UNAVAILABLE`, fails closed, and leaves readiness
  unchanged. Manifests that still set `libfont_ready_flag_addr` fail with
  `LIBFONT_READY_FLAG_RETIRED`; remove the field and use translated guest startup.
  Real-title validation of startup dependencies remains in the works
  ([#299](https://github.com/Jstar269/nakagawa-recomp/issues/299)), as is the
  clean-room open-font converter
  ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).

[`COMPATIBILITY.md`](COMPATIBILITY.md) carries the full per-title and
subsystem breakdown with tracking issues.

## What is still in the works

| Area | Today | Tracking |
| --- | --- | --- |
| Encrypted executables and PRX modules | The built-in boundary handles supported containers with a user-supplied local key file; plain `BOOT.BIN`/`EBOOT.elf` and decrypted modules remain alternatives. PGD/amctrl is unsupported. | [#295](https://github.com/Jstar269/nakagawa-recomp/issues/295), [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |
| Commercial-game packages in public builds | Packages build with the public runtime, and public builds render imported PGF fonts through the project-authored reader for supported inputs (PR [#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)). Composite glyphs and the supported revision and shadow-map variants render too ([#521](https://github.com/Jstar269/nakagawa-recomp/issues/521)); a revision-3 font's compressed character-map subtables and any other shadow-map width are still refused by name. | [PGF_SPEC.md §4](cleanroom/PGF_SPEC.md#4-non-requirements-and-named-boundaries) |
| In-game system fonts | Import fonts from your own PSP with `python tools/nk_cli.py fonts import <folder>`. | [#300](https://github.com/Jstar269/nakagawa-recomp/issues/300) |
| Games beyond the verified title | Other games import as Experimental; bring-up of further titles is ongoing. | [#285](https://github.com/Jstar269/nakagawa-recomp/issues/285), [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |

See [`SETUP.md`](SETUP.md) for developer setup and the command-line tools.
