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
or a recorded Fontworks/Sony camouflage marker, and embeds the pin's complete
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
