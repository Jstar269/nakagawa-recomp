# Fonts

This folder is where Nakagawa Recomp looks for fonts. In the source tree it holds only this file. A build copies the
folder next to the program (for example `build/font/`) and adds the project's own fonts to it.

## The project's own fonts

The build makes 18 fonts in the PSP's font format (`.pgf`) from open fonts, one for each of the PSP's 18 system
fonts, with the project's converter `tools/ttf2pgf.py`:

| File | Stands in for | Made from | Licence |
| --- | --- | --- | --- |
| `nkjpn.pgf` | `jpn0`, Japanese text | M PLUS 1p Regular | SIL Open Font License 1.1 |
| `nkltn.pgf`, `nkltn1.pgf` to `nkltn7.pgf` | `ltn0` to `ltn7`, Latin text at the PSP's larger size | Liberation Sans and Liberation Serif, Regular, Italic, Bold and Bold Italic | SIL Open Font License 1.1 |
| `nkltns.pgf`, `nkltn9.pgf` to `nkltn15.pgf` | `ltn8` to `ltn15`, Latin text at the PSP's smaller size | The same eight faces | SIL Open Font License 1.1 |
| `nkkr.pgf` | `kr0`, Korean text | Gowun Dodum Regular | SIL Open Font License 1.1 |

Each Latin file has the family and style of the PSP font it stands in for, in the PSP's order: within each size,
sans-serif regular, serif regular, sans-serif italic, serif italic, sans-serif bold, serif bold, sans-serif bold
italic and serif bold italic.

Each font comes with its licence text (`nkjpn-OFL.txt` and so on) and a manifest (`nkjpn.json` and so on) that names
its exact source file and the converter settings. The source fonts are pinned in
[`fixtures/fonts/`](../fixtures/fonts/): each pin file gives the file's SHA-256 and where it came from. Generated
fonts are never committed.

These fonts are stand-ins. They let a game draw its text when you have not imported your PSP's fonts, but they look
different from the PSP's. The Latin fonts are sized and spaced to come close: their nominal sizes are the PSP's, and
their x-height, cap height, stem width and spacing are set from aggregate measurements of the PSP fonts they stand in
for. No letter shape comes from those fonts.

## Without your PSP's fonts: what changes

You don't need a PSP to play. Without its fonts, every game still starts and still draws its text, with the fonts
above standing in. **Settings > PSP fonts** says how many of the PSP's 18 fonts are your own and which ones are
stand-ins, and the player's checks before a game starts say the same.

What stays the same:

- **Latin characters.** Every character of the PSP's Latin fonts is in the Latin stand-ins: ASCII, Latin-1, Latin
  Extended-A, the Windows-1252 punctuation and symbols, and the Cyrillic alphabet.
- **Latin sizes and line lengths.** Each Latin stand-in has its PSP font's family, style and nominal size, and a
  sentence or a menu line in any of the 16 Latin faces comes out within 3 % of its width on a PSP. Digits in the
  sans-serif faces all have the PSP's width.

What changes:

- **Letter shapes.** The text looks like Liberation Sans and Liberation Serif (Latin), M PLUS 1p (Japanese) or Gowun
  Dodum (Korean), not the PSP's fonts.
- **Capitals against small letters.** In Liberation's faces capitals are wider, compared with small letters, than in
  the PSP's. The spacing splits the difference, so in the regular sans-serif faces capitals average about 2 % wider
  than the PSP's and small letters about 2 % narrower. A line in capitals, such as a menu, runs up to 3 % longer and
  a sentence up to 2 % shorter.
- **Japanese and Korean size.** Both stand-ins are drawn at 12 pixels per em; the PSP's Japanese and Korean fonts
  are larger, so Japanese and Korean text is smaller than on a PSP and lines are shorter.
- **Missing characters.** A character the stand-in lacks is drawn as the font library's replacement character: an
  underscore, unless the game chooses another. Where the stand-in lacks that one too, nothing is drawn. The Japanese
  stand-in has only the 2,965 level-1 kanji of JIS X 0208 and lacks 6,128 of the PSP Japanese font's 9,773
  characters. The Korean stand-in lacks 512 of the PSP Korean font's 11,836, all in U+1200 to U+12FF and U+1400 to
  U+14FF; Gowun Dodum has none of them.

### Measured differences

Measured on 2026-11-04 at `9ea3cc410` by reading each of the 18 fonts of one PSP-3000 (firmware 6.61) and the
stand-in that serves it through the player's own font reader (`src/rt/pgf_public.c`), with the stand-ins built by
`tools/ttf2pgf.py` with the Makefile's settings. Widths are the sum of the characters' advances, without spaces.
"Sentence" is `The quick brown fox jumps over the lazy dog.`; "menu line" is `NEW GAME  CONTINUE  OPTIONS  0123456789`.
The "Now" columns compare the current stand-ins with those recorded PSP widths (#2005); the "Before" columns are the
measurement of the earlier stand-ins.

| PSP fonts | Before: characters lacking | Before: width against the PSP font | Now: characters lacking | Now: width against the PSP font |
| --- | --- | --- | --- | --- |
| `ltn0` to `ltn7` (larger Latin size) | 111 of 322: 95 Cyrillic letters; the symbols `№ ∆ ∙ √ ∞ ≈ ≠ ≤ ≥`; `ƒ ˆ ˜` and four combining accents (U+0301, U+0306, U+0308, U+0338) | One face for all eight. Sans-serif regular (`ltn0`): sentence −2.0 %, menu line −7.3 %. Serif faces (odd numbers): sentence +6.0 % to +13.8 %. Sans-serif bold (`ltn4`, `ltn6`): sentence −6.2 %, menu line −9.3 %. Capitals average 1.05 px narrower and digits 1.91 px narrower. | None | A face per font. Every sentence and menu line within 3 %: sans-serif regular and italic −1.7 % and +2.9 %, serif −2.1 % to +2.8 %, sans-serif bold and bold italic +0.4 % and −0.3 %. In `ltn0`, small letters average 0.27 px narrower and capitals 0.28 px wider; digits have the PSP's width. |
| `ltn8` to `ltn15` (smaller Latin size) | The same 111 of 322 | Sans-serif regular (`ltn8`): sentence −0.2 %, menu line −5.5 %. Serif faces: sentence +7.9 % to +15.9 %. Sans-serif bold (`ltn12`, `ltn14`): sentence −4.5 %, menu line −7.5 %. | None | Every sentence and menu line within 3 %: sans-serif regular and italic −1.1 % and +2.8 %, serif −2.0 % to +2.8 %, sans-serif bold and bold italic +0.2 % and −0.2 %. In `ltn8`, small letters average 0.14 px narrower and capitals 0.17 px wider; digits have the PSP's width. |
| `jpn0` | 6,128 of 9,773: 4,953 of the 7,919 kanji (the stand-in had the 2,965 level-1 kanji of JIS X 0208 and none of level 2), 263 enclosed and squared characters, 365 symbols, 84 Latin-1 and 71 other Latin and Greek letters, and smaller groups | Kana and kanji 6.00 px narrower each on average. Nominal size 12 points where the PSP font gives 10.125. | Unchanged | Unchanged |
| `kr0` | 512 of 11,836, all in U+1200 to U+12FF and U+1400 to U+14FF | Hangul syllables 7.44 px narrower each on average. Nominal size 12 points where the PSP font gives 10.125. | Unchanged | Unchanged |

## Your PSP's own fonts

For the PSP's original look, import the system fonts from your own PSP: the setup wizard's fonts step does it, and so
does `python tools/nk_cli.py fonts import <folder>` (see [System fonts](../docs/SETUP.md#system-fonts)). Imported fonts
are kept in your user data folder (`<user data>/fonts/v2`), not here. The project never includes, downloads or uploads
the PSP's fonts.

## How a game finds its fonts

When a game asks for one of the PSP's system fonts, it gets your imported copy of that font first. A Latin font you
have not imported is served by your nearest imported font of the same size, and otherwise by the project's face for
that font (`nkltn5.pgf` for `ltn5`, `nkltns.pgf` for `ltn8`, and so on); a Japanese or Korean font you have not
imported is served by `nkjpn.pgf` or `nkkr.pgf`. The full order, and how to see which file serves each font, is in
[System fonts](../docs/SETUP.md#system-fonts). The log has one `flash0: game opened` line for each font a game opens,
with the file that serves it.

The `SR_FONTDIR` environment variable points the runtime at another folder instead of the one next to the program. A
folder that is missing or cannot be read is reported as an error; nothing quietly takes its place.

## The earlier comparison with PPSSPP's fonts

Before the project made its own fonts, it compared the replacement fonts that PPSSPP ships. This project does not use,
copy or distribute those files. Their origins (conversions of Source Han Sans and Ume Gothic, renamed to match the
PSP's font names) and the exact bytes compared are recorded in
[docs/provenance/FONT_ORIGINS.md](../docs/provenance/FONT_ORIGINS.md).
