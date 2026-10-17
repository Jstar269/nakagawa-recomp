# PPSSPP replacement fonts

The active public source tree contains only this provenance summary in `font/`;
no generated PGF is committed here. The four replacement PGF payloads evaluated
for local development remain excluded. The runtime build generates the project
Japanese slot from the pinned open-font input documented below. Other slots
still use an imported font when available, otherwise their absence is reported.

The public evidence summary below identifies the compared upstream bytes. The
active public source profile continues to exclude the compared PGF payloads.
`make project-japanese-font` builds `nkjpn.pgf` under the configured build
directory from the pinned M PLUS 1p input; the runtime `compile` target stages
that file and its OFL text beside the executable. See [NOTICE.md](../NOTICE.md)
and the [public source profile](../docs/PUBLIC_SOURCE_PROFILE.md) for the
enforced exclusion of the compared payloads.

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
  `388ac3c` (2020-11-23, "Update jpn0.pgf … Fixes
  [#13702](https://github.com/hrydgard/ppsspp/pull/13702)"). The October 2020
  predecessors named Ume Gothic S5 (`77c1e96`) and Ume Hy Gothic (`f68e0fe`,
  PR [#13588](https://github.com/hrydgard/ppsspp/pull/13588), "fixed manually
  all of JIS Kanji-Level1 (2946 character)").
- kr0.pgf descends from a **Source Han Sans** conversion: header
  `Source Han Sans / Regular` at `75bdb5f` (2020-11-30, "Switch to
  nassau-tk's latest Korean font. See issue
  [#13190](https://github.com/hrydgard/ppsspp/issues/13190)").
- ltn0.pgf and ltn8.pgf descend from **Ume Hy Gothic** (Ume-family licence,
  modified BSD) conversions: header `Ume Hy Gothic / Regular` at `b5e2300`
  (2020-11-29, PR [#13721](https://github.com/hrydgard/ppsspp/pull/13721)) and
  `737e0d5` (2020-11-23). The Ume P Mincho
  history applies to odd ltn1/3/5/7, not ltn0.
- Conversion chain (contributor's own notes, PPSSPP
  [#13718](https://github.com/hrydgard/ppsspp/issues/13718)/[#13589](https://github.com/hrydgard/ppsspp/issues/13589)):
  TTF → PGF via tpunix pgftool / ttf2pgfj.exe, glyph shaping in a font editor,
  and hex-editor metric patching (ascender/descender in 26.6 fixed point).
- On 2021-01-20 (`dc34bea`, "PGF Fixed Bold & Italic property and
  **camouflage the Font name**") the headers were rewritten to compatibility
  names: jpn0 → `FTT-NewRodin Pro DB`, kr0 → `AsiaKNHH-SONY-uni`, ltn0/ltn8 →
  `FTT-NewRodin Pro Latin` (all `Regular`). Those names are admitted
  camouflage, not evidence of Sony or Fontworks outlines; redistributing the
  payloads under those names would misattribute Adobe/Ume outlines.

Residual unknowns: the exact Source Han Sans release/variant file (never named
by the contributor and not narrowed by any byte comparison), the exact Ume Hy
Gothic release, and the converter tools' licence terms (pgftool ships no
licence file). See FONT_ORIGINS.md §7.

## Chosen route

The Japanese slot is generated from the pinned M PLUS 1p input by the
project-owned deterministic converter. The PGF uses the project-owned name
`Nakagawa Open Japanese`; its manifest and OFL text are staged beside it in the
build output. The source TTF, pin and licence are under
[`fixtures/fonts/mplus1p/`](../fixtures/fonts/mplus1p/). The four compared PGF
payloads remain excluded, and generated PGFs are not checked in.

## Runtime lookup

font_load() in ../src/rt/hle.c honors an absolute SR_FONTDIR or the executable's
sibling font directory. A missing or invalid root is reported as an error.

The public source tree excludes the payloads. Public builds read fonts through
the project-authored reader (`src/rt/pgf_public.c`, #349), which fails closed on
malformed or unsupported fonts; point SR_FONTDIR at fonts you supplied yourself.
