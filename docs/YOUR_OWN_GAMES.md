# Playing your own games

Status: CURRENT. This guide explains what the native player needs to play a PSP
game you own, what it does for you, and what is still in the works.

## Status of retail games

Nakagawa Recomp never downloads games, keys, firmware or fonts. You bring your own.

A game the maintainer has tested on a documented route keeps exactly the status its recorded evidence supports, as listed in [`COMPATIBILITY.md`](COMPATIBILITY.md). Any other retail game is:

- **Experimental**, if someone has seen it start or run but the maintainer hasn't checked it; or
- **Untested**, if no one has reported on it.

Importing a disc doesn't change its status. The **EXPERIMENTAL** badge on the game card only means no one knows yet how well it runs.

## What you need

- **Windows 11 x64** with a Vulkan-capable graphics card and current GPU
  drivers. Windows is the only supported player platform today; other platforms
  are in the works ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306), [#329](https://github.com/Jstar269/nakagawa-recomp/issues/329), [#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).
- **A disc image of a game you own, in uncompressed standard `.iso` format.**
  The project doesn't make or supply images. Whether you may make an image of
  a disc, or of a digital purchase, depends on where you live and on the terms
  you accepted. Check both. Nakagawa Recomp never downloads games and contains no
  game data. Don't use copies you don't own.
- **Nothing else, for most commercial games.** Nakagawa turns the game's own
  program code into a native program on your computer, so it has to be able to
  read that code. Most retail PSP programs (`EBOOT.BIN` and some `.prx` modules)
  are encrypted, and Nakagawa decrypts them from your own disc on your PC when it
  checks the disc (see [Encrypted games](#supplying-unencrypted-files)).
- **PSP system fonts (optional, recommended for in-game text).** To see a
  game's text in the PSP's own lettering, copy the Sony font files (`.pgf`)
  from your own PSP's `flash0:/font/`. The project doesn't include or share
  them. Copy them to a folder and run `python tools/nk_cli.py fonts import <folder>`.
  If two files cover the same slot, pick one with `--choose <slot>=<file>`
  (slots: `japanese`, `latin`, `korean`). Check the result with
  `python tools/nk_cli.py fonts status`. Without them, Nakagawa uses open
  replacement fonts that the project builds with its own converter
  (`tools/ttf2pgf.py`, [#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).

Before you add a game, you can check that Nakagawa runs on your PC. When the
showcase demos come with your copy of the player, the library lists them in
their own **Test this PC** row, apart from your games. Play one to check that
graphics, sound and controls work ([`SHOWCASE.md`](SHOWCASE.md)).

What to expect: adding an ISO identifies the disc and shows a checklist of what
will and won't work. **BUILD PACKAGE** recompiles the game while you watch, and
**PLAY** starts it once the package checks out. For most retail discs the
player first decrypts the game's program from your own disc, on your PC. Building needs a
C toolchain (Python, GCC and GNU Make). The player can download the exact
versions it needs after you agree once, or you can install them yourself
([`SETUP.md`](SETUP.md)). Playing straight from an ISO with no local toolchain
is in the works ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)).

## What the player does automatically

1. Add the ISO: drag it into the player window, or click **START SETUP WIZARD**
   (on an empty library) or the **Add a disc image you own** card at the end of
   your games. If the disc is already in your library, adding it again keeps the
   files already unpacked from it, as long as
   the disc ID, version and image size match.
2. The player reads the disc's own `PARAM.SFO` to find the game's title and disc ID.
   When it starts, the player also loads every title manifest in `<user data>/manifests`
   (up to 8, in alphabetical order), so a game described by a manifest you added
   is recognized without extra options.
3. It checks the game's program file:
   - An **unencrypted** program, including a disc that carries an unencrypted
     `BOOT.BIN`, is picked automatically. Homebrew and the project's showcase
     demos are in this group.
   - An **encrypted** program is decrypted from your own disc on your PC, into
     the game's folder (`<user data>/titles/<DISC_ID>/decrypted/`). If a file can't
     be decrypted, the game card says which one and why, and an `EBOOT.elf` you
     put in that folder later is picked up automatically when you build.
4. Once an unencrypted program is available, click **BUILD PACKAGE**
   (or **REBUILD PACKAGE** when the package is out of date).
   - If the manifest reads BSS metadata from the disc's `~PSP` program header,
     `build-package` takes it straight from the disc image; you don't need to
     provide a separate header file.
   - The finished package carries the decrypted modules it was built against in
     `<package>/modules`, and the launcher sets `SR_MODULE_DIR` to point there,
     so the package has everything it needs to start.
   - A run starts at the game's declared fallback entry when it has one
     (`runtime_bindings.fallback_entry`), or else at the program's entry point.
   - The player recompiles the game, shows each stage, and turns on **PLAY** when the
     package is ready. If it meets something it can't handle yet, the build stops
     with a message that says what is missing and which issue tracks it. It never
     pretends a build succeeded.

## Supplying unencrypted files

When the player checks a disc you added (when you add it, on Check again, and
before BUILD PACKAGE), it decrypts the disc's encrypted `EBOOT.BIN` and
encrypted modules on your PC, the way PPSSPP does, with the public game key
tables it carries. Each decrypted file goes into the per-title folder below
only after it checks out as a valid PSP program. Nothing is downloaded or
uploaded, and nothing leaves your PC. Nakagawa never reads a key file and never
decrypts PSP firmware: an updater, the boot loader or other system software is
refused by name. You only need to put files there yourself when a file can't be
decrypted.

This is not legal advice. Laws on decrypting and copying games differ by
country. Check the rules where you live.

When you do, put the unencrypted files in the game's folder that the player shows
on the game card:

```text
<user data>/titles/<DISC_ID>/decrypted/
├── EBOOT.elf        (the game's main executable)
└── <module>.prx     (only modules the player or build says are required)
```

On Windows, `<user data>` is normally `%LOCALAPPDATA%\Nakagawa\data`. The
player looks at `%LOCALAPPDATA%` first, then Windows `FOLDERID_LocalAppData`,
then `%APPDATA%`; it never falls back to the top of your user profile. If the old
`%USERPROFILE%\Nakagawa\data` exists and the current folder doesn't, the player
and Doctor show its exact path so you can move it yourself. A usable file
is a 32-bit little-endian MIPS ELF. It starts with the bytes `7F 45 4C 46`, and the
build tools check that it is a MIPS ELF before using it. A file that starts with
`~PSP` or `~SCE` is still encrypted, and the player will say so. The player
notices the folder on its own the next time it checks the game.

Decrypted game modules can be named either after their file name on the disc
(for example `psmf.prx`) or after their manifest module name (for example
`scePsmf_library.prx`). If a module is missing or isn't a plain decrypted ELF, the
build error message shows both names.

### What an encrypted file reports

A file Nakagawa can't decrypt stops with a named reason, for example:

```text
ENCRYPTED_EXECUTABLE: EBOOT.BIN is encrypted and built-in decryption stopped: KEY_REQUIRED (<detail>). Place a decrypted EBOOT.elf from your own copy in <dir>.
```

`KEY_REQUIRED` means the game uses an encryption the built-in tables don't
cover yet; `MALFORMED_INPUT` and `INTEGRITY_FAILURE` mean the file on the disc
looks damaged or changed. An encrypted module stops the same way with
`ENCRYPTED_MODULE: <name> is encrypted and built-in decryption stopped: ...`.
A plain copy you put in the folder yourself is always used and never replaced.

A PSN release of a UMD game (an `EBOOT.PBP` from the PlayStation Store whose
game is a disc image inside the package) can be added like a disc: pick the
`EBOOT.PBP`. The player turns it back into its disc image once, on this PC, in
`<user data>/discs/<DISC_ID>/<DISC_ID>.iso`, checks that the result is a valid
disc image whose disc ID matches the package, and then adds that image exactly as
if you had picked it. The conversion shows its progress and can be cancelled; a
cancelled or failed conversion keeps nothing. Your `EBOOT.PBP` is only read,
never changed. **Remove from library** keeps the converted image in
`<user data>/discs/<DISC_ID>/`, and the decrypted files in
`<user data>/titles/<DISC_ID>/decrypted/`, so adding the game again is quick;
delete those two folders yourself to get the space back. Other PSN downloads
(PSP minis and games tied to the PSP that bought them), PSOne Classics and
PGD-protected data aren't supported yet.

### Supplying title manifests

If your game needs a title manifest of its own (a file that describes the game to Nakagawa), put the `.json` file in:

```text
<user data>/manifests/
└── <manifest_name>.json
```

The player loads every `.json` manifest in this folder when it starts (in
alphabetical order, up to 8 manifests). The command-line tool `tools/nk_cli.py
build-package` also looks in this folder, after the titles in the repository, and
finds the same title the player does.

## Keep it to your own games

- Only use games you own.
- Don't share decrypted files, generated packages or disc images. Everything
  Nakagawa decrypts or builds stays in your user data folder.
- Issues, pull requests and project discussions must never contain game files,
  keys, or links to downloads or decryption tools.

## When something is not supported yet

Nakagawa never fakes what it can't do. When something is unsupported, missing
or not built yet, it stops cleanly, names the missing piece and tells you why,
instead of pretending it worked or crashing without a word.

- **Game not in the built-in list:** a valid PSP disc with no profile yet is
  imported with the **EXPERIMENTAL** badge (see [Status of retail games](#status-of-retail-games)).
  The card says compatibility is unknown, the checklist shows what is missing,
  and **PLAY** stays unavailable until a matching runtime package exists.
  Support for more games, and for games in general, is in the works
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). A disc image
  with no `PSP_GAME` folder isn't a PSP game disc and is refused as `DISC_NOT_PSP`.
- **Compressed disc image:** a `.cso` (or `.zso`) file is refused as
  `DISC_IMAGE_COMPRESSED`. Nakagawa reads plain `.iso` images only, so turn it
  back into an `.iso` with the tool that compressed it, or copy your disc again.
- **PlayStation Store package:** a file that starts with the `PBP` package
  signature is a package, not a disc image, whatever it is called. The player
  turns a PSN release of a UMD game into its disc image (see
  [What an encrypted file reports](#what-an-encrypted-file-reports)). A PSOne Classic is refused as
  `PSONE_CLASSIC_UNSUPPORTED`, a system software update as `FIRMWARE_OUT_OF_SCOPE`,
  and any other package as `PBP_PACKAGE_UNSUPPORTED`, each with a sentence naming
  the title and disc ID, before any key is read. The command-line inspection
  (`tools/nk_core/iso_inspect.py`) still refuses every package with
  `PBP_PACKAGE_UNSUPPORTED`; only the package's own `PARAM.SFO` is read to write
  that sentence.
- **Encrypted program:** the player decrypts it from your own disc
  ([#295](https://github.com/Jstar269/nakagawa-recomp/issues/295)). If that stops, the checklist
  reports `ENCRYPTED_EXECUTABLE` or `ENCRYPTED_MODULE` with the reason and names
  the game's folder. An unencrypted `BOOT.BIN` on the disc, or a valid
  `EBOOT.elf` already in that folder, is used as it is. PGD/amctrl protection is
  not supported.
- **No package built yet:** if a game has been identified and set up but there
  is no valid package for it in `<user data>/packages/<DISC_ID>/`, the
  checklist marks `RUNTIME_PACKAGE` as missing and the card offers
  **BUILD PACKAGE**. Play stays off until a valid package is built
  ([#465](https://github.com/Jstar269/nakagawa-recomp/pull/465)). The one
  exception is a non-experimental catalogued game whose developer runtime the
  launcher can find, which plays without a package
  ([#483](https://github.com/Jstar269/nakagawa-recomp/pull/483)). A package that
  is out of date, or one the player can no longer use (for example after an
  upgrade), shows **REBUILD PACKAGE** with one line saying why. A rebuild asks
  for the same consent and shows the same progress as a first build, and puts
  the old package back if it fails
  ([#1147](https://github.com/Jstar269/nakagawa-recomp/issues/1147)). If the
  package check can't start, the card shows `PACKAGE CHECK FAILED` with a
  `RETRY PACKAGE CHECK` button; background checks try again after a short,
  limited wait.
- **Missing firmware fonts:** if the player reports a missing system font, use
  the import step in [What you need](#what-you-need) and the font notes there.
  A game's translated `libfont` starts through its own entry point; **BUILD
  PACKAGE** translates the copy on the game's disc too. When that can't run (an
  untranslated entry, or `SR_REAL_MODULE_START=0`), the runtime reports
  `LIBFONT_STARTUP_UNAVAILABLE`, never pretends the library is ready, and stops
  the game; the player then shows that the game stopped early instead of a
  frozen screen. Manifests that still set `libfont_ready_flag_addr`
  fail with `LIBFONT_READY_FLAG_RETIRED`; remove the field and let the game's
  own startup run. A game that loads its `libfont.prx` by a path relative to
  its data directory gets the translated copy too
  ([#299](https://github.com/Jstar269/nakagawa-recomp/issues/299)). The
  project's open-font converter is finished
  ([#313](https://github.com/Jstar269/nakagawa-recomp/issues/313)).

[`COMPATIBILITY.md`](COMPATIBILITY.md) has the full breakdown for every game and
every part of the PSP, with the issues that track them.

## What is still in the works

| Area | Today | Tracking |
| --- | --- | --- |
| Encrypted programs and PRX modules | The player decrypts UMD disc images (`.iso`) with built-in tables, and turns PSN releases of UMD games (`EBOOT.PBP`) into their disc image first. A file it can't decrypt is named, and a decrypted copy from your own disc can stand in. Other PSN packages and PGD data aren't decrypted yet. | [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |
| Commercial-game packages in public builds | Packages build with the public runtime, and public builds draw imported PGF fonts through the project-authored reader for supported inputs (PR [#474](https://github.com/Jstar269/nakagawa-recomp/pull/474)). Composite glyphs and the supported revision and shadow-map variants draw too ([#521](https://github.com/Jstar269/nakagawa-recomp/issues/521)); a revision-3 font's compressed character-map subtables, and any other shadow-map width, are still refused by name. | [PGF_SPEC.md §4](cleanroom/PGF_SPEC.md#4-non-requirements-and-named-boundaries) |
| Games other than the maintainer-tested ones | Other games import with the **EXPERIMENTAL** badge and stay Untested unless someone has seen them start; work on more games goes on. | [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |

See [`SETUP.md`](SETUP.md) for developer setup and the command-line tools.

## Command-line reference (`nk_cli`)

`python tools/nk_cli.py` can do everything the player does without a window, plus a few diagnostic tools. Run any
command with `--help` for its exact options; this section says what each command reads, what it writes and how it
reports the result. The runtime's environment switches are in [`DEBUGGING.md`](DEBUGGING.md).

### Common conventions

- **Player data folder.** Commands that touch the library, packages or fonts use the player's per-user data
  folder. Choose another with `--user-data-root <dir>` (named `--root` on `inspect`). Open the player on the
  same folder with `nakagawa_player.exe --user-data-root=<dir>`: its library, packages, settings, logs and
  saves then all stay in that folder (settings in `config`, saves in `saves`), and the normal profile is left
  alone. This keeps a second library, for example every disc of a collection under test, apart from the one
  you play.
- **Machine-readable output.** `--json` (or `--progress-json` on `build-package`) prints JSON instead of text, so
  scripts never have to parse sentences.
- **Exit status.** `0` means success. `1` means the command ran and refused or failed, with the reason on stderr.
  `2` means the arguments were invalid (argparse usage errors, or a disc ID that is not nine characters).
- **Fail closed.** A step that can't be done correctly stops with a named reason; nothing reports a success it
  didn't achieve.

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
| `bringup <iso> --work-dir DIR --report FILE [--launch-timeout S] [--instruction-trace]` | A PSP `.iso` | Runs a time-limited bring-up in a private work folder and writes a JSON report that is safe to share |
| `vram <image> --addr A --width W --height H --format F [...] --output PNG` | A 2 MiB raw VRAM capture | Decodes one surface to PNG; `--from-sidecar FILE --out-dir DIR` exports every surface a runtime capture describes |

### Privacy boundary

Game files, keys, decrypted modules and work folders stay on your machine. The only outputs meant for sharing are
the `bringup` report, which is safe to share, and VRAM PNGs you choose to export; the private inputs behind them are
never copied into the repository.
