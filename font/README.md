# Fonts

This folder is where Nakagawa Recomp looks for fonts. In the source tree it holds only this file. A build copies the
folder next to the program (for example `build/font/`) and adds the project's own fonts to it.

## The project's own fonts

The build makes three fonts in the PSP's font format (`.pgf`) from open fonts, with the project's converter
`tools/ttf2pgf.py`:

| File | Used for | Made from | Licence |
| --- | --- | --- | --- |
| `nkjpn.pgf` | Japanese text | M PLUS 1p Regular | SIL Open Font License 1.1 |
| `nkltn.pgf` | Latin text | M PLUS 1p Regular | SIL Open Font License 1.1 |
| `nkkr.pgf` | Korean text | Gowun Dodum Regular | SIL Open Font License 1.1 |

Each font comes with its licence text (`nkjpn-OFL.txt` and so on) and a manifest (`nkjpn.json` and so on) that names
its exact source file and the converter settings. The source fonts are pinned in
[`fixtures/fonts/mplus1p/`](../fixtures/fonts/mplus1p/) and [`fixtures/fonts/gowundodum/`](../fixtures/fonts/gowundodum/):
each `PIN.json` gives the file's SHA-256 and where it came from. Generated fonts are never committed.

These fonts are stand-ins. They let a game draw its text when you have not imported your PSP's fonts, but they look
different from the PSP's.

## Your PSP's own fonts

For the PSP's original look, import the system fonts from your own PSP: the setup wizard's fonts step does it, and so
does `python tools/nk_cli.py fonts import <folder>` (see [System fonts](../docs/SETUP.md#system-fonts)). Imported fonts
are kept in your user data folder (`<user data>/fonts/v2`), not here. The project never includes, downloads or uploads
the PSP's fonts.

## How a game finds its fonts

When a game asks for one of the PSP's system fonts, it gets your imported copy of that font first. A Latin font you
have not imported is served by your nearest imported font of the same size, and otherwise by `nkltn.pgf`; a Japanese
or Korean font you have not imported is served by `nkjpn.pgf` or `nkkr.pgf`. The full order, and how to see which
file serves each font, is in [System fonts](../docs/SETUP.md#system-fonts).

The `SR_FONTDIR` environment variable points the runtime at another folder instead of the one next to the program. A
folder that is missing or cannot be read is reported as an error; nothing quietly takes its place.

## The earlier comparison with PPSSPP's fonts

Before the project made its own fonts, it compared the replacement fonts that PPSSPP ships. This project does not use,
copy or distribute those files. Their origins (conversions of Source Han Sans and Ume Gothic, renamed to match the
PSP's font names) and the exact bytes compared are recorded in
[docs/provenance/FONT_ORIGINS.md](../docs/provenance/FONT_ORIGINS.md).
