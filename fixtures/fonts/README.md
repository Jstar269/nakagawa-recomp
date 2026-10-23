# Pinned public font fixtures

Pinned, permissively licensed input fonts for the deterministic TTF to PGF
converter (`tools/ttf2pgf.py`, issue #313). Each family directory carries
three files and nothing else:

| File | Role |
| --- | --- |
| `<Family>.ttf` | The exact pinned source bytes, unmodified. |
| `OFL.txt` | The full licence text required by OFL section 2; its local digest is in `PIN.json`. |
| `PIN.json` | The `font-pin/v1` record: SHA-256 and size of the font, SPDX id, copyright line, licence-text digest, Reserved Font Names, and the upstream URL/repository/path/retrieved provenance. |

The converter refuses any input whose bytes do not match the pin
(`pin-digest-mismatch`), refuses output names that use a Reserved Font Name
or a recorded brand-mimicking marker, and embeds the pin's complete
licence material in the conversion manifest. See
[`docs/cleanroom/PGF_CONVERTER_SPEC.md`](../../docs/cleanroom/PGF_CONVERTER_SPEC.md)
for the full contract, including why generated PGFs are never committed here:

- Firmware and camouflaged PPSSPP payloads stay excluded
  (`assets/public_source_profile.json` excludes `font/*.pgf`);
- test conversions write only to temporary or `build/` paths;
- the fixture PGF is a project-generated rendering of these pinned outlines,
  never an authenticity claim about PSP system fonts.

## Inventory

| Family | File | SPDX | Reserved Font Names |
| --- | --- | --- | --- |
| Gowun Dodum | `gowundodum/GowunDodum-Regular.ttf` | OFL-1.1 | none declared |
| Gudea | `gudea/Gudea-Regular.ttf` | OFL-1.1 | `Gudea` |
| M PLUS 1p | `mplus1p/MPLUS1p-Regular.ttf` | OFL-1.1 | none declared |

M PLUS 1p is the project Japanese build input. Its pin uses the immutable
Google Fonts commit recorded in `mplus1p/PIN.json`; the adjacent `OFL.txt`
contains the complete licence text from that commit. The pin records the
downloaded licence digest and the local digest after its one trailing space
was removed. The converter's built-in Japanese set covers JIS X 0201 Roman
U+0020-U+007E (95 points), half-width katakana U+FF61-U+FF9F (63), and JIS X
0208 rows 1-8 (524 non-kanji points) plus rows 16-47 (all 2,965 level-1
kanji), for 3,647 points total. Sawarabi Gothic was evaluated first but misses
80 points in rows 1-8, so it does not meet the required symbol coverage.

M PLUS 1p is also the project Latin build input. Its built-in text set contains
348 points across ASCII, Latin-1 Supplement, Latin Extended-A, and common
punctuation and symbols; the pinned TTF covers all 348. The Latin slot is
built at 13 ppem with the converter's synthetic emboldening (6/8 px wider,
4/8 px taller strokes, 3/8 px extra advance; converter specification section 3
rule 10), so title text reads at the stroke weight and line length that system
text of the same nominal size has. The Japanese and Korean slots keep 12 ppem
without emboldening.

Gowun Dodum is the project Korean build input. Its pin uses the immutable
Google Fonts commit recorded in `gowundodum/PIN.json`, and the adjacent
`OFL.txt` is the full licence text from that source. The KS X 1001 repertoire
is decoded with Python's standard `euc_kr` codec and includes ASCII, assigned
symbols and jamo in rows 1-4 and 6-9, plus all 11,172 modern Hangul syllables.
It contains 11,951 unique points and the selected input covers every one.
Hanja is out of scope; Roman numerals, Japanese kana, Greek, and Cyrillic are
outside this Korean-text repertoire. The generated PGF name is
`Nakagawa Open Korean`; the source-family name is not copied into its header.
