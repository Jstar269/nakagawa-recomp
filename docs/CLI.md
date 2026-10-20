# Command-line reference (`nk_cli`)

`python tools/nk_cli.py` is the headless interface to everything the player does, plus a few diagnostic tools. Run any
command with `--help` for its exact options; this page describes what each command reads, what it writes and how it
reports the result. The desktop player itself is described in [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md), and the
runtime's environment switches in [`DEBUGGING.md`](DEBUGGING.md).

## Common conventions

- **Player data directory.** Commands that touch the library, packages or fonts use the player's per-user data
  directory. Override it with `--user-data-root <dir>` (named `--root` on `inspect`).
- **Machine-readable output.** `--json` (or `--progress-json` on `build-package`) prints JSON instead of text, so
  scripts never parse prose.
- **Exit status.** `0` means success. `1` means the command ran and refused or failed, with the reason on stderr.
  `2` means the arguments were invalid (argparse usage errors, or a disc ID that is not nine characters).
- **Fail closed.** A step that cannot be done correctly stops with a named reason; nothing reports success it did
  not achieve.

## Commands

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

## Privacy boundary

Game files, keys, decrypted modules and work directories stay on your machine. The only outputs meant for sharing are
the public-safe `bringup` report and VRAM PNGs you choose to export; the private inputs behind them are never copied
into the repository.
