# Fonts

This folder is where Nakagawa Recomp looks for fonts. In the source tree it holds only this file. A build copies the
folder next to the program (for example `build/font/`) and adds the project's own fonts to it.

## The project's own fonts

The build makes four fonts in the PSP's font format (`.pgf`) from open fonts, with the project's converter
`tools/ttf2pgf.py`:

| File | Used for | Made from | Licence |
| --- | --- | --- | --- |
| `nkjpn.pgf` | Japanese text | M PLUS 1p Regular | SIL Open Font License 1.1 |
| `nkltn.pgf` | Latin text, the PSP's larger size (`ltn0` to `ltn7`) | M PLUS 1p Regular | SIL Open Font License 1.1 |
| `nkltns.pgf` | Latin text, the PSP's smaller size (`ltn8` to `ltn15`) | M PLUS 1p Regular | SIL Open Font License 1.1 |
| `nkkr.pgf` | Korean text | Gowun Dodum Regular | SIL Open Font License 1.1 |

Each font comes with its licence text (`nkjpn-OFL.txt` and so on) and a manifest (`nkjpn.json` and so on) that names
its exact source file and the converter settings. The source fonts are pinned in
[`fixtures/fonts/mplus1p/`](../fixtures/fonts/mplus1p/) and [`fixtures/fonts/gowundodum/`](../fixtures/fonts/gowundodum/):
each `PIN.json` gives the file's SHA-256 and where it came from. Generated fonts are never committed.

These fonts are stand-ins. They let a game draw its text when you have not imported your PSP's fonts, but they look
different from the PSP's. The two Latin fonts are sized and weighted to come close: their nominal sizes are the PSP's,
and their x-height, cap height, stem width and spacing are set from aggregate measurements of the PSP fonts of the same
size. No letter shape comes from those fonts, and the bold and italic faces are served by the regular one.

## Without your PSP's fonts: what changes

You don't need a PSP to play. Without its fonts, every game still starts and still draws its text, with the fonts
above standing in. **Settings > PSP fonts** says how many of the PSP's 18 fonts are your own and which ones are
stand-ins, and the player's checks before a game starts say the same.

What stays the same: every letter, digit and punctuation mark of plain English text (the 94 printable ASCII
characters) and all of Latin-1 are in the Latin stand-ins, at the PSP's own two nominal sizes. A sentence of
English in the regular sans-serif face comes out within 2 % of its width on a PSP.

What changes:

- **Letter shapes.** The text looks like M PLUS 1p (Japanese and Latin) or Gowun Dodum (Korean), not the PSP's
  fonts.
- **Styles.** The PSP has eight Latin faces at each size: sans-serif and serif, each regular, italic, bold and bold
  italic. One regular sans-serif face serves all eight, so serif text runs wider and bold text narrower than it
  does on a PSP.
- **Capitals and digits.** They are narrower than the PSP's, so a line in capitals, such as a menu, comes out 5 to
  9 % narrower than in the PSP's sans-serif faces.
- **Missing characters.** A character the stand-in lacks is drawn as the font library's replacement character: an
  underscore, unless the game chooses another. Where the stand-in lacks that one too, nothing is drawn.
- **Japanese and Korean size.** Both stand-ins are drawn at 12 pixels per em; the PSP's Japanese and Korean fonts
  are larger, so Japanese and Korean text is smaller than on a PSP and lines are shorter.

### Measured differences

Measured on 2026-11-04 at `9ea3cc410` by reading each of the 18 fonts of one PSP-3000 (firmware 6.61) and the
stand-in that serves it through the player's own font reader (`src/rt/pgf_public.c`), with the stand-ins built by
`tools/ttf2pgf.py` with the Makefile's settings. Widths are the sum of the characters' advances. "Sentence" is
`The quick brown fox jumps over the lazy dog.`; "menu line" is `NEW GAME  CONTINUE  OPTIONS  0123456789`.

| PSP fonts | Stand-in | Characters the stand-in lacks | Width against the PSP font |
| --- | --- | --- | --- |
| `ltn0` to `ltn7` (larger Latin size) | `nkltn.pgf` | 111 of 322: 95 Cyrillic letters; the symbols `№ ∆ ∙ √ ∞ ≈ ≠ ≤ ≥`; `ƒ ˆ ˜` and four combining accents (U+0301, U+0306, U+0308, U+0338) | Sans-serif regular (`ltn0`): sentence −2.0 %, menu line −7.3 %. Serif faces (odd numbers): sentence +6.0 % to +13.8 %. Sans-serif bold (`ltn4`, `ltn6`): sentence −6.2 %, menu line −9.3 %. Capitals average 1.05 px narrower and digits 1.91 px narrower. |
| `ltn8` to `ltn15` (smaller Latin size) | `nkltns.pgf` | The same 111 of 322 | Sans-serif regular (`ltn8`): sentence −0.2 %, menu line −5.5 %. Serif faces: sentence +7.9 % to +15.9 %. Sans-serif bold (`ltn12`, `ltn14`): sentence −4.5 %, menu line −7.5 %. |
| `jpn0` | `nkjpn.pgf` | 6,128 of 9,773: 4,953 of the 7,919 kanji (the stand-in has the 2,965 level-1 kanji of JIS X 0208 and none of level 2), 263 enclosed and squared characters, 365 symbols, 84 Latin-1 and 71 other Latin and Greek letters, and smaller groups | Kana and kanji 6.00 px narrower each on average; the widest advance is 12.48 px against 18.72 px. The font header gives a nominal size of 12 points where the PSP font gives 10.125. |
| `kr0` | `nkkr.pgf` | 512 of 11,836, all in U+1200 to U+12FF and U+1400 to U+14FF; all 11,172 Hangul syllables are there | Hangul syllables 7.44 px narrower each on average. The header gives a nominal size of 12 points where the PSP font gives 10.125. |

Of the characters the stand-ins lack, all 111 Latin ones and 2,765 of the Japanese ones (2,112 of them kanji) are in
the pinned source fonts, so a wider character set in the converter settings would add them. None of the 512 Korean
ones is in Gowun Dodum.

## Your PSP's own fonts

For the PSP's original look, import the system fonts from your own PSP: the setup wizard's fonts step does it, and so
does `python tools/nk_cli.py fonts import <folder>` (see [System fonts](../docs/SETUP.md#system-fonts)). Imported fonts
are kept in your user data folder (`<user data>/fonts/v2`), not here. The project never includes, downloads or uploads
the PSP's fonts.

## How a game finds its fonts

When a game asks for one of the PSP's system fonts, it gets your imported copy of that font first. A Latin font you
have not imported is served by your nearest imported font of the same size, and otherwise by `nkltn.pgf` or
`nkltns.pgf`, whichever has its size; a Japanese or Korean font you have not imported is served by `nkjpn.pgf` or
`nkkr.pgf`. The full order, and how to see which file serves each font, is in
[System fonts](../docs/SETUP.md#system-fonts). The log has one `flash0: game opened` line for each font a game opens,
with the file that serves it.

The `SR_FONTDIR` environment variable points the runtime at another folder instead of the one next to the program. A
folder that is missing or cannot be read is reported as an error; nothing quietly takes its place.

## The earlier comparison with PPSSPP's fonts

Before the project made its own fonts, it compared the replacement fonts that PPSSPP ships. This project does not use,
copy or distribute those files. Their origins (conversions of Source Han Sans and Ume Gothic, renamed to match the
PSP's font names) and the exact bytes compared are recorded in
[docs/provenance/FONT_ORIGINS.md](../docs/provenance/FONT_ORIGINS.md).
