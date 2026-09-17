# PPSSPP replacement fonts

The active public source tree contains only this provenance summary in `font/`.
Four replacement PGF payloads evaluated for local development were copied
byte-for-byte from PPSSPP; the payloads and their detailed redistribution-review
records remain excluded. They were not extracted from a PSP, firmware image, or
the user's game. The HLE sceFont path treats them as optional data: an invalid or
missing font root is reported instead of hidden by a synthetic font.

The public evidence summary below identifies the compared upstream bytes. The
active public build links the fail-closed PGF boundary and does not assume
replacement fonts are present. See [NOTICE.md](../NOTICE.md) and the
[public source profile](../docs/PUBLIC_SOURCE_PROFILE.md) for the enforced
exclusion.

## Byte provenance

Source repository: <https://github.com/hrydgard/ppsspp>

Pinned comparison commit:
f0baf3ade7bcb6c86f0835962b36eb4e51559d8f

Upstream path: assets/flash0/font/

| Filename | Bytes | Git blob ID |
| --- | ---: | --- |
| jpn0.pgf | 4,316,284 | 17304b24f6175f7a221f167425b307e761ccb6bb |
| kr0.pgf | 1,641,624 | 9f8cce0390c827fa2195a5dc3e9885a997db6488 |
| ltn0.pgf | 38,236 | 1f3d907ac717270a005c2e8a00e05c385d718d22 |
| ltn8.pgf | 29,976 | a58450c7675c6f1b4e0840cef5f2dd9787eed68c |

## Current evidence summary

Determined facts (full evidence in
[docs/provenance/FONT_ORIGINS.md](../docs/provenance/FONT_ORIGINS.md); PGF
headers parsed per PPSSPP `Core/Font/PGF.h` from exact upstream bytes):

- jpn0.pgf descends from a **Source Han Sans** (SIL OFL 1.1, Adobe, Reserved
  Font Name 'Source') conversion: header `Source Han Sans / Regular` at
  `388ac3c` (2020-11-23, "Update jpn0.pgf … Fixes #13702"). The October 2020
  predecessors named Ume Gothic S5 (`77c1e96`) and Ume Hy Gothic (`f68e0fe`,
  PR #13588, "fixed manually all of JIS Kanji-Level1 (2946 character)").
- kr0.pgf descends from a **Source Han Sans** conversion: header
  `Source Han Sans / Regular` at `75bdb5f` (2020-11-30, "Switch to
  nassau-tk's latest Korean font. See issue #13190").
- ltn0.pgf and ltn8.pgf descend from **Ume Hy Gothic** (Ume-family licence,
  modified BSD) conversions: header `Ume Hy Gothic / Regular` at `b5e2300`
  (2020-11-29, PR #13721) and `737e0d5` (2020-11-23). The Ume P Mincho
  history applies to odd ltn1/3/5/7, not ltn0.
- Conversion chain (contributor's own notes, PPSSPP #13718/#13589): TTF → PGF
  via tpunix pgftool / ttf2pgfj.exe, glyph shaping in a font editor, and
  hex-editor metric patching (ascender/descender in 26.6 fixed point).
- On 2021-01-20 (`dc34bea`, "PGF Fixed Bold & Italic property and
  **camouflage the Font name**") the headers were rewritten to compatibility
  names: jpn0 → `FTT-NewRodin Pro DB`, kr0 → `AsiaKNHH-SONY-uni`, ltn0/ltn8 →
  `FTT-NewRodin Pro Latin` (all `Regular`). Those names are admitted
  camouflage, not evidence of Sony or Fontworks outlines; redistributing the
  payloads under those names would misattribute Adobe/Ume outlines.

Residual unknowns: the exact Source Han Sans release/variant file (bounded to
releases {2.001, 2.002}, never named by the contributor), the exact Ume Hy
Gothic release, and the converter tools' licence terms (pgftool ships no
licence file). See FONT_ORIGINS.md §7.

## Chosen route

(2) Generate our own PGF payloads from pinned OFL sources with the
project-owned, reproducible converter at `tools/pgf_build.py` — with (3) as
the interim: payloads stay excluded and users supply fonts locally until a
converter-produced payload with pinned inputs, shipped OFL/Ume notices, an
honest non-RFN name, qualified review, and a path-specific provenance record
lands. (1) Redistributing the PPSSPP payloads is rejected: notices alone
cannot cure the false Sony/Fontworks designation, and the exact inputs are
unrecoverable. OFL/Ume licence texts will be added under
`THIRD_PARTY_LICENSES/` only when a payload is actually shipped.

## Runtime lookup

font_load() in ../src/rt/hle.c honors an absolute SR_FONTDIR or the executable's
sibling font directory. A missing or invalid root is reported as an error.
Users who cannot redistribute these optional binaries may supply a separately
licensed compatible PGF directory.
