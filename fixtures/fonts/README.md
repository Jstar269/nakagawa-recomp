# Pinned public font fixtures

Pinned, permissively licensed input fonts for the deterministic TTF to PGF
converter (`tools/ttf2pgf.py`, issue #313). Each family directory carries
three files and nothing else:

| File | Role |
| --- | --- |
| `<Family>.ttf` | The exact pinned source bytes, unmodified. |
| `OFL.txt` | The licence text required by OFL section 2, byte-identical to the upstream copy. |
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
