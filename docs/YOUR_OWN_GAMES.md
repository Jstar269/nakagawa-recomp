# Playing your own games

Status: CURRENT. This guide explains what the native player needs in order to
play a PSP game you own, what it does for you automatically, and what is still
in the works.

## Status of retail games

Nakagawa Recomp never downloads games, keys, firmware or fonts. You bring your own.

A title the maintainer has tested on a documented route keeps exactly the status its recorded evidence supports, as listed in [`COMPATIBILITY.md`](COMPATIBILITY.md). Any other retail game is:

- **Experimental**, if it has been seen to boot or run, with no maintainer qualification; or
- **Untested**, if there is no record at all.

Importing a disc does not change its status. The **EXPERIMENTAL** badge on the game card says only that compatibility is unknown.

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
  your own local key file for the built-in decryption boundary (see
  [Key file for encrypted executables](#key-file-for-encrypted-executables)) or
  put your own unencrypted files in the per-title folder.
- **PSP system fonts (optional, recommended for text).** In-game typography
  uses Sony firmware font files (`.pgf`) dumped from your own PSP's
  `flash0:/font/`. The project does not include or distribute them. Copy them
  to a folder and run `python tools/nk_cli.py fonts import <folder>`. If two
  files cover the same slot, pick one with `--choose <slot>=<file>` (slots:
  `japanese`, `latin`, `korean`). Check the result with `python tools/nk_cli.py
  fonts status`. Until [#1142](https://github.com/Jstar269/nakagawa-recomp/issues/1142)
  (serving each Latin font index from its own file) lands, text may clip or
  show placeholder glyphs, and a title's own font library may draw project
  fonts as dashes ([#1055](https://github.com/Jstar269/nakagawa-recomp/issues/1055)).
  The project-authored open-font converter (`tools/ttf2pgf.py`,
  [#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)) builds open
  replacement fonts.

Expectation: adding an ISO identifies the disc and shows a compatibility
checklist of what will and will not work. **BUILD PACKAGE** recompiles the
title in front of you, and **PLAY NOW** starts it once the package validates.
For most retail discs you also need an unencrypted executable, from your own
copy or from your key file. Building needs a C toolchain (Python, GCC and GNU
Make). The player can fetch pinned build tools after you approve each item, or
you can install them yourself ([`SETUP.md`](SETUP.md)). Playing straight from
an ISO with no local toolchain is in the works
([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).

## What the player does automatically

1. Add the ISO: drag it into the player window, or click **START SETUP WIZARD**
   (on an empty library) or **ADD ANOTHER ISO**. If the
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
4. Once an unencrypted executable is available, click **BUILD PACKAGE**
   (or **REBUILD PACKAGE** when the package is stale).
   - If the manifest reads BSS metadata from the disc's `~PSP` executable
     header, `build-package` extracts it directly from the disc image; you do
     not need to provide a separate header file.
   - The compiled package ships the decrypted modules it compiled against in
     `<package>/modules`, and the launcher sets `SR_MODULE_DIR` to point there
     so launches are self-contained.
   - A run starts at the title's declared fallback entry when one is defined
     (`runtime_bindings.fallback_entry`), or at the executable entry point.
   - The player recompiles the game, shows each stage, and turns on **PLAY NOW** when the
     package is ready. Anything it can't handle yet stops the build with a message
     naming what is missing and its tracking issue. It never pretends a build
     succeeded.

## Supplying unencrypted files

Some people use third-party tools to produce unencrypted copies of a game's
executable and modules from their own copy. Some run on your own PSP, others on
a PC. The project does not include, endorse or link to these tools and cannot
check what they do. It contains no decryption keys. For an encrypted
executable, use your own key file (see below) or your own unencrypted copy.

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

### Key file for encrypted executables

Most retail executables are encrypted. The project ships no keys and does not
make one. With a key file for your own copy, the built-in decryption boundary
can decrypt the executable and the modules it needs:

- **Where it goes:** `<user data>/keys/psp-keyfile.json`. On Windows this is
  normally `%LOCALAPPDATA%\Nakagawa\data\keys\psp-keyfile.json`. If the
  `NAKAGAWA_PSP_KEY_FILE` environment variable is set, it names the file
  instead and takes precedence.
- **What it contains:** JSON with `"format": "nakagawa-psp-keystore-1"` and an
  `entries` object, with hexadecimal values only. The schema and the entry
  names are in [`SETUP.md`](SETUP.md#built-in-decryption-boundary-issue-295).
- **What `MISSING_KEY_ENTRY` means:** the boundary fails closed and reports
  `MISSING_KEY_ENTRY <entry>`, naming the entry the container needs that your
  key file does not have. Each module fails on its own; the others still
  decrypt. Add the named entry to your own key file, or supply a decrypted
  file instead.
- **Where output goes:** decrypted files are written only under your user data
  folder, under `titles/<DISC_ID>/decrypted/`. A plain copy you put there
  yourself always wins over automatic decryption.
- **Keep it private:** never share, upload or commit the key file.

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
  imported with the **EXPERIMENTAL** badge (see [Status of retail games](#status-of-retail-games)).
  The card notes that compatibility is unknown,
  checklist items indicate what is missing, and **PLAY NOW** stays unavailable until a
  matching runtime package exists. Bring-up of further titles and generic
  title intake are in the works
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
- **Missing firmware fonts:** if the player reports a missing system font, use
  the import step in [What you need](#what-you-need) and the font notes there.
  Guest translated `libfont` startup runs through its guest entry; when that startup is
  unavailable (an untranslated entry, or `SR_REAL_MODULE_START=0`), the runtime
  reports `LIBFONT_STARTUP_UNAVAILABLE`, fails closed, and leaves readiness
  unchanged. Manifests that still set `libfont_ready_flag_addr` fail with
  `LIBFONT_READY_FLAG_RETIRED`; remove the field and use translated guest startup.
  Real-title validation of startup dependencies remains in the works
  ([#299](https://github.com/Jstar269/nakagawa-recomp/issues/299)). The
  project-authored open-font converter is complete
  ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).

[`COMPATIBILITY.md`](COMPATIBILITY.md) carries the full per-title and
subsystem breakdown with tracking issues.

## What is still in the works

| Area | Today | Tracking |
| --- | --- | --- |
| Encrypted executables and PRX modules | The built-in boundary handles supported containers with a user-supplied local key file; plain `BOOT.BIN`/`EBOOT.elf` and decrypted modules remain alternatives. PGD/amctrl is unsupported. | [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |
| Commercial-game packages in public builds | Packages build with the public runtime, and public builds render imported PGF fonts through the project-authored reader for supported inputs (PR [#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)). Composite glyphs and the supported revision and shadow-map variants render too ([#521](https://github.com/Jstar269/nakagawa-recomp/issues/521)); a revision-3 font's compressed character-map subtables and any other shadow-map width are still refused by name. | [PGF_SPEC.md §4](cleanroom/PGF_SPEC.md#4-non-requirements-and-named-boundaries) |
| Per-index Latin font serving and title font libraries | Text may clip or show placeholder glyphs; see [What you need](#what-you-need). | [#1142](https://github.com/Jstar269/nakagawa-recomp/issues/1142), [#1055](https://github.com/Jstar269/nakagawa-recomp/issues/1055) |
| Games other than the maintainer-tested titles | Other games import with the **EXPERIMENTAL** badge and stay Untested unless someone has seen them boot; bring-up of further titles is ongoing. | [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |

See [`SETUP.md`](SETUP.md) for developer setup and the command-line tools.

## Command-line reference (`nk_cli`)

`python tools/nk_cli.py` is the headless interface to everything the player does, plus a few diagnostic tools. Run any
command with `--help` for its exact options; this section describes what each command reads, what it writes and how
it reports the result. The runtime's environment switches are in [`DEBUGGING.md`](DEBUGGING.md).

### Common conventions

- **Player data directory.** Commands that touch the library, packages or fonts use the player's per-user data
  directory. Override it with `--user-data-root <dir>` (named `--root` on `inspect`).
- **Machine-readable output.** `--json` (or `--progress-json` on `build-package`) prints JSON instead of text, so
  scripts never parse prose.
- **Exit status.** `0` means success. `1` means the command ran and refused or failed, with the reason on stderr.
  `2` means the arguments were invalid (argparse usage errors, or a disc ID that is not nine characters).
- **Fail closed.** A step that cannot be done correctly stops with a named reason; nothing reports success it did
  not achieve.

### Commands

| Command | Reads | Writes / prints |
| --- | --- | --- |
| `inspect <iso> [--json] [--root DIR]` | A PSP `.iso` and its `PSP_GAME/PARAM.SFO` | Title, disc ID, executable kind and whether packages, fonts and decrypted inputs are present |
| `prepare <iso> [--dest DIR] [--user-data-root DIR] [--player EXE]` | A PSP `.iso` | Adds the disc to the library (raw discs may go to `--dest`); archive discs are set up through the player executable |
| `build-package <disc_id> [--module-dir DIR] [--psp-header HDR] [--progress-json [FILE]] [--log-file FILE]` | The library entry for `disc_id`, your decrypted executable and modules | Recompiles the title into `<player data>/packages/<DISC_ID>/`; progress as one JSON object per line with `--progress-json` |
| `launch <game_dir> [--profile NAME] [--fps-cap N] [--software]` | A prepared package directory with its `manifest.json` | Plans and starts the run; `--profile` is `Standard`, `Performance`, `Benchmark` or `Diagnostics`; `--software` uses the software GE rasterizer |
| `fonts import <folder> [--choose SLOT=FILE] [--json]` | `.pgf` font files you dumped from your own PSP | Copies them into the player data for the `japanese`, `latin` and `korean` slots |
| `fonts status [--project-root DIR] [--json]` | The player data and project `font/` folder | The source of each font slot |
| `fonts remove (--slot SLOT \| --all)` | The player data | Removes fonts this tool imported |
| `bringup <iso> --work-dir DIR --report FILE [--launch-timeout S] [--instruction-trace]` | A PSP `.iso` | Runs a bounded bring-up in a private work directory and writes a public-safe JSON report |
| `vram <image> --addr A --width W --height H --format F [...] --output PNG` | A 2 MiB raw VRAM capture | Decodes one surface to PNG; `--from-sidecar FILE --out-dir DIR` exports every surface a runtime capture describes |

### Privacy boundary

Game files, keys, decrypted modules and work directories stay on your machine. The only outputs meant for sharing are
the public-safe `bringup` report and VRAM PNGs you choose to export; the private inputs behind them are never copied
into the repository.
