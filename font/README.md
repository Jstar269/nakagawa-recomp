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

- jpn0.pgf's current blob descends from the Source Han Sans header line
  introduced on 2020-11-23. The earlier October Ume Gothic S5/Hy Gothic
  replacement is a superseded intermediate.
- kr0.pgf's immediately preceding revisions identify Source Han Sans.
- ltn0.pgf and ltn8.pgf are the even Latin family and identify Ume Hy Gothic
  in the material revisions. The Ume P Mincho history applies to odd
  ltn1/3/5/7, not ltn0.
- PPSSPP later changed the metadata to compatibility names such as
  FTT-NewRodin Pro DB, AsiaKNHH-SONY-uni, and FTT-NewRodin Pro Latin. Those
  names are not evidence of Sony or Fontworks outlines.

The exact Source Han Sans or Ume TTF filename/release used for the current
blobs, the complete manual-edit chain, and the required accompanying notices
remain unknown. Do not assign a blanket GPL or proprietary label. The
repository-level GPL-3.0-or-later declaration is not component clearance.

## Runtime lookup

font_load() in ../src/rt/hle.c honors an absolute SR_FONTDIR or the executable's
sibling font directory. A missing or invalid root is reported as an error.
Users who cannot redistribute these optional binaries may supply a separately
licensed compatible PGF directory.
