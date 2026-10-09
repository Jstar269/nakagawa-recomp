# Project-authored behaviour specification - deterministic OpenType/TTF to PGF converter

**Status:** specification with implementation. The converter ships as
`tools/ttf2pgf.py` (issue #313), writes through the deterministic writer
`tools/pgf_writer.py`, and is verified by `tools/test_ttf2pgf.py` against the
public reader `src/rt/pgf_public.c`. This document is also the converter's
independence and provenance record; per
[`../provenance/INDEPENDENCE_MODEL.md`](../provenance/INDEPENDENCE_MODEL.md)
the defensible claim is that this unit's behaviour is independently established
and its expression is project-authored - not the term "clean room", which the
model reserves for a process this project did not follow.

Written under the campaign protocol (`../INDEPENDENCE_CAMPAIGN.md`) rules for
spec-first work: from the public OpenType format documentation, the
project-authored reader specification `PGF_SPEC.md`, the route decision in
[`../provenance/FONT_ORIGINS.md`](../provenance/FONT_ORIGINS.md), and
project-owned measurements. Every behavioural rule below is either cited to
those sources or marked as a project decision with its rationale.

## 1. Role and scope

The converter turns one pinned TrueType-outline font plus a metric-target
policy into a public-safe PGF fixture:

- **In scope.** sfnt parsing of static `glyf` fonts; cmap lookup for a
  contiguous 16-bit code range, or an explicit ascending code-point set that
  yields a sparse character map; unhinted rasterization to 4-bit alpha;
  26.6 metric derivation; honest output naming; a machine-readable conversion
  manifest; refusal of every malformed, unsupported, or policy-violating
  input named in section 3.
- **Out of scope.** CFF/OTF and variable fonts; hint execution; pair kerning
  (PGF carries per-glyph advances only - `PGF_SPEC.md` section 3.10); text
  shaping; authentic firmware-font provisioning (route #300, which remains
  preferable when fidelity to the PSP system fonts is required); claiming any
  pixel identity with a firmware font (non-goal of issue #313).
- **CJK.** Issue #313 permits a CJK source only when legally and technically
  suitable. No CJK source is pinned by this change: the OFL CJK candidates are
  multi-megabyte payloads whose admission needs the qualified review that
  `FONT_ORIGINS.md` route (2) requires before any such fixture enters the
  tree. The converter itself is codepoint-driven and has no Latin-only
  assumption, so a future CJK pin needs a pin file and a coverage range, not a
  converter change.

## 2. Input contract

### 2.1 sfnt container

The input must be a TrueType sfnt: scaler type `0x00010000` or Apple `true`.
`OTTO` and any other scaler are refused by name. The directory must hold
1..512 unique ASCII tags whose byte ranges lie inside the file. Required
tables: `cmap`, `glyf`, `head`, `hhea`, `hmtx`, `loca`, `maxp`. Table
checksums are **not** verified: integrity of a pinned input is established by
its SHA-256 in the pin, and `head.checkSumAdjustment` bookkeeping varies
across legitimate toolchains (measured: the pinned fixture's own `head`
checksum does not match its recomputed value). A malformed `head`, `maxp`,
`hhea`, `hmtx`, or `loca` is refused with a section-named reason.

### 2.2 Character map

Subtables are considered in a fixed preference order - Unicode platform
records first, then Microsoft full-repertoire, Microsoft BMP, Microsoft
symbol, then legacy records - and the first subtable in a supported format
(0, 4, 6, or 12) parses successfully wins. Ties keep directory order. A
mapping to glyph 0 (`.notdef`) counts as absent. Every code in the requested
range must resolve, or the conversion is refused with `missing-code-point`
naming the first gap in ascending order.

### 2.3 Outlines

Simple glyphs are read as documented: contour ends, hint length (skipped,
never executed), repeat-coded flags, delta-encoded coordinates. Composite
glyphs support XY component offsets with identity, uniform, or axis-aligned
scale transforms (including the scaled-offset flag). Point-matching
arguments, 2x2 transforms, nesting beyond 8 levels, and expansions beyond 64
components are refused by name; component hint programs are skipped. All
component coordinates are composed exactly with rational arithmetic before
scaling.

### 2.4 Reason catalogue

| Reason | Meaning |
| --- | --- |
| `unsupported-sfnt-version` | Scaler type is not a TrueType sfnt. |
| `unsupported-outline-format` | `OTTO`, `"CFF "`, or `CFF2`: PostScript outlines. |
| `unsupported-variable-font` | `fvar`/`gvar`/`cvar`/`HVAR`/`MVAR`/`STAT` present. |
| `unsupported-loca-format` | `indexToLocFormat` is neither 0 nor 1. |
| `unsupported-cmap-format` | No usable Unicode subtable in formats 0/4/6/12. |
| `unsupported-composite-point-args` | Composite matches points instead of XY offsets. |
| `unsupported-composite-transform` | Composite uses a 2x2 transform. |
| `missing-table` | A required table is absent. |
| `truncated-table` | A directory entry claims bytes past end of file. |
| `malformed-font` / `malformed-head` / `malformed-maxp` / `malformed-hhea` / `malformed-hmtx` / `malformed-loca` / `malformed-cmap` / `malformed-glyph` | Bounds or invariant violation in that section. |
| `missing-code-point` | A requested code has no glyph. |
| `bad-code-range` | `--codes` is unparseable, inverted, or outside 16 bits. |
| `bad-codepoint-list` | A `--codepoints` file is unreadable, not ASCII, holds a line that is not a code point, or leaves 16 bits. |
| `codepoints-unsorted` | A `--codepoints` file repeats a code or is not in ascending order. |
| `empty-codepoint-set` | A `--codepoints` file lists no code point. |
| `ppem-out-of-range` | Size outside 1..128. |
| `glyph-too-large` | A raster exceeds the writer's 7-bit record fields (127 px). |
| `composite-too-deep` / `composite-too-many-components` | Composite expansion bounds. |
| `metric-targets-invalid` / `metric-target-unreachable` | Policy document is malformed / no size satisfies it. |
| `conflicting-parameters` | `--ppem` together with `--metric-targets`. (`--codes` and `--codepoints` are mutually exclusive at the CLI.) |
| `pin-invalid` / `pin-digest-mismatch` / `pin-size-mismatch` / `pin-license-mismatch` | Source pin does not bind the exact input and licence bytes. |
| `reserved-font-name` / `camouflage-font-name` | Output name policy violation (section 6). |
| writer reasons (`font-field-invalid`, `nominal-size-out-of-range`, `metric-table-overflow`, ...) | Re-raised unchanged from `pgf_writer.py`. |

`--codepoints FILE` supplies the code points instead of `--codes`. The file lists
one code point per line as `0x41`, `U+0041`, or decimal; `#` starts a comment.
Codes must be strictly ascending, so a set has exactly one reading, and every
listed code needs a glyph (`missing-code-point` otherwise). The file's SHA-256
and leaf name go into the manifest. The writer then emits a sparse character map
over the span from the smallest to the largest code, and codes outside the set
are misses.

A refusal means no byte is written: the CLI builds the complete image and
manifest in memory first and writes only after every stage succeeds.

## 3. Determinism contract

1. **Scaling.** Font units to 26.6: `floor(value * ppem * 64 / upem + 1/2)`
   in exact rational arithmetic, rounded exactly once. Ties round toward
   positive infinity.
2. **Curve flattening.** Quadratic contours are expanded with implied
   on-curve midpoints, then subdivided by exact rational de Casteljau halving
   until the second-difference manhattan norm is at most 2 (26.6 units), or a
   hard depth of 24. No floating point participates anywhere. Each flattened
   vertex is then rounded once to an integer 26.6 coordinate by
   `floor(v + 1/2)`, ties toward positive infinity, the same rule as scaling.
   Contour `endPtsOfContours` indices must strictly increase; a violation is
   refused as `malformed-glyph`.
3. **Bounding box.** `left = floor(min_x / 64)`, `right = ceil(max_x / 64)`
   (likewise vertically) over the flattened, rounded outline. Flattening plus
   final rounding stays within `2/8 + 1/2` = 0.75 units of the true curve,
   while the first sub-sample centre sits 4 units inside a pixel boundary, so
   the grid can never clip a covered sub-sample nor include an empty border
   row or column.
4. **Supersample grid.** 8x8 samples per pixel. Sub-scanline centres are at
   `top26 - (row * 8 + step) * 8 - 4` in y-up 26.6; sub-column centres at
   `8 * j + 4` relative to `left26`.
5. **Fill rule.** Non-zero winding over edges using the half-open vertical
   rule `y0 <= y < y1`. Crossing intervals keep sub-columns strictly inside
   the active span (centres on an interval boundary belong to neither side),
   so shared contour vertices can never double-count a sample.
6. **Coverage rounding.** `floor((count * 15 + 32) / 64)` with `count` in
   0..64, mapping to the writer's 4-bit samples.
7. **Advances.** `hmtx` advance scaled by rule 1; `advance_y` is 0.
8. **Iteration order.** Codes ascend; the writer sorts glyphs, metric tables,
   and map entries itself; manifest JSON uses sorted keys, 2-space indent,
   ASCII, and a trailing newline.
9. **No ambient input.** No clock, timestamp, locale, environment variable,
   hash-order iteration, or random source is read. The only dates anywhere
   are static provenance fields inside the pin (section 7).

Same pinned input plus same parameters plus same converter version therefore
produce byte-identical `.pgf` and manifest output on every supported host,
which `tools/test_ttf2pgf.py` asserts by repeated runs.

## 4. Geometry to PGF metrics

For a glyph whose raster box is `left, bottom, right, top` in whole pixels
(relative to the origin on the baseline, y up):

| Field | Value |
| --- | --- |
| `width`, `height` | box size in pixels (7-bit record fields). |
| `dimension_width`, `dimension_height` | `width * 64`, `height * 64`. |
| `x_left` | `left * 64` (bitmap left edge relative to the pen). |
| `x_center` | writer default: `x_left + advance_x // 2`. |
| `y_base` | `top * 64` (pixels of bitmap above the baseline). |
| descender (derived) | `y_base - dimension_height = bottom * 64`, matching `PGF_SPEC.md` section 3.9. |
| `adjust_x`, `adjust_y` | 0; positioning is carried by the 26.6 metrics, and the reader's draw path uses only the caller's position (`PGF_SPEC.md` section 3.10). |
| `row_order` | 1 (raster order). |

Header extrema are then whatever `pgf_writer` derives from those records:
`ascender = max(y_base)`, `descender = min(y_base - dimension_height)`, and
the maxima of dimension, advance, and pixel size. The header's horizontal and
vertical size fields (`0x24`, `0x28`) are not extrema: they carry the nominal
em size, `ppem * 64` in 26.6 units, so a game reads the chosen size rather than
the largest advance. A glyph with no covered
pixel (a space) is emitted as a zero-area record with only its advance, which
the reader reports as present-but-unmapped exactly like `PGF_SPEC.md`
section 3.9 describes.

## 5. Metric-target policy

Sizing policy is a separate input from the outlines so PSP-like metrics can
be produced and tested without embedding any retail font bytes
(`FONT_ORIGINS.md` route (2) asks for ascender 9..10 px and descender -2 px
as the small-latin target). `--metric-targets FILE` takes:

```json
{
  "ascender_px": 9,
  "descender_px": -2,
  "tolerance_px": 1,
  "ppem_min": 6,
  "ppem_max": 24
}
```

The search picks the **smallest** `ppem` in `[ppem_min, ppem_max]` whose
exact header extrema (rule set of section 4) land within `tolerance_px` of
both targets; a cheap control-point bound prunes candidate sizes and only
candidates within tolerance plus two pixels are measured exactly. If no size
qualifies, the refusal is `metric-target-unreachable` - the policy is never
silently relaxed. The chosen ppem and both achieved values are recorded in
the manifest's `metric_targets` block. `--ppem` and `--metric-targets` are
mutually exclusive.

## 6. Output and naming policy

The container is written entirely by `tools/pgf_writer.py`: revision 2, the
392-byte base header, direct character map only, no shadow records, no
subcharmap tables - the subset `PGF_SPEC.md` documents for revision 2 and the
writer's own contract (a code span with a sparse character map when the code
set has gaps, at most 255 entries per metric table). Kerning beyond per-glyph advances does not exist in PGF and none is
fabricated (`PGF_SPEC.md` section 3.10).

The default output name is `Nakagawa Open Latin` / `Regular`. Before anything
is written the converter refuses:

1. PSP firmware font names (`jpn0`, `kr0`, `ltn0`..`ltn15`, with or without
   `.pgf`) - enforced by the writer and re-raised unchanged.
2. The recorded Fontworks/Sony camouflage markers `ftt-newrodin` and
   `asiaknhh` (case-insensitive), per `FONT_ORIGINS.md` section 5.3.
3. The Adobe OFL Reserved Font Name `source`, per `FONT_ORIGINS.md` route (2).
4. Any Reserved Font Name listed by the active source pin (for the pinned
   fixture: `Gudea`), per OFL section 3.

Generated PGFs are fixtures, never an authenticity claim.

## 7. Source pin and conversion manifest

`--pin FILE` must bind the exact input bytes (schema `font-pin/v1`):

| Pin field | Verified against |
| --- | --- |
| `sha256`, `size_bytes` | SHA-256 and length of the input file. |
| `license.license_text`, `license.license_text_sha256` | Licence file beside the pin, read as UTF-8. |
| `license.spdx`, `license.copyright`, `license.reserved_font_names` | Carried into the manifest verbatim. |
| `source.url`, `source.repository`, `source.path`, `source.retrieved` | Static provenance of the pinned bytes; carried into the manifest. |

`--manifest FILE` (which requires `--pin`) emits canonical JSON with:

| Block | Contents |
| --- | --- |
| `converter` | Name, version, and this specification's path. |
| `input` | Path, SHA-256, size, `unitsPerEm`, glyph count, outline format, tables used. |
| `output` | Path, SHA-256, size, font name/type, revision, first/last glyph, the header size fields and extrema read back from the emitted bytes. |
| `coverage` | First/last code, converted code count, code span, and `U+XXXX..U+YYYY` range. |
| `code_points` | For `--codepoints`: the file's leaf name, count, and SHA-256; `null` for a range. |
| `glyphs` | Count, zero-area codes, maximum pixel dimensions. |
| `metric_targets` | The policy, the chosen ppem, and achieved 26.6 values - or `null`. |
| `parameters` | ppem, nominal em size (26.6), supersample, coverage bits, rounding formulas, hinting mode. |
| `license` | SPDX id, copyright line, reserved font names, the **full licence text**, its digest, and the source pin's provenance record - or `null` without a pin. |

## 8. Independence and provenance record

- **Sources used.** The public OpenType/TrueType format documentation (table
  layouts and semantics as published by the format stewards), the
  project-authored reader specification `PGF_SPEC.md` and its public reader
  `src/rt/pgf_public.c`, the writer contract of `tools/pgf_writer.py`, the
  route decision and measurements in `FONT_ORIGINS.md`, and project-owned
  tests.
- **Sources deliberately not used.** `pgftool` (no licence grant,
  `FONT_ORIGINS.md` section 4), `ttf2pgfj.exe` (provenance unknown),
  PPSSPP's font or PGF converter sources, JPCSP or sal063 sources, and any
  retail, firmware, or camouflaged font payload. No bytes of those inputs are
  read, embedded, or compared against. This record exists so the claim in
  issue #313 is checkable rather than asserted.
- **Classification.** Candidate `project-authored-independent` under
  `INDEPENDENCE_MODEL.md`: behaviour established from public format facts and
  project-owned specification, expression written for this project. The
  authoritative attestation is the maintainer's trusted ledger entry, not
  this paragraph; publication admission of the new paths is maintainer-side.
- **Pinned fixture.** `fixtures/fonts/gudea/` carries `Gudea-Regular.ttf`
  (SHA-256 `a4b5410090a821ea17021627312babdf3d8db0271217fd8c4d9f629ef0a2d90b`,
  22,932 bytes), its `OFL.txt` (SHA-256
  `315a576cbc7ab61c9e347b5725893bc8498fdcb8fc10831793c6864bc2cefba8`), and
  `PIN.json`. The source is the SIL Open Font License 1.1 family *Gudea* by
  Agustina Mingote as published in `google/fonts` at `ofl/gudea/`. OFL
  section 2 material ships alongside the bytes; the output name avoids the
  family's Reserved Font Name.
- **Generated PGFs.** Test runs write PGFs only to temporary or ignored
  (`build/`) paths; this change commits no generated `.pgf` and no
  `font/*.pgf` payload, so `public_source_profile.json` keeps excluding the
  firmware-font route unchanged.

## 9. Verification map

| Property | Test |
| --- | --- |
| Byte-identical repeat conversions (in-process and CLI) | `ConversionDeterminismTests` |
| Manifest digests, coverage, licence material, header agreement | `ConversionDeterminismTests` |
| Pin binds exact input and licence bytes | `SourcePinTests` |
| Reader round trip, drawn samples, `H` shape, zero-area space, composites | `ReaderRoundTripTests` |
| Metric targets land header extrema in tolerance | `MetricTargetTests` |
| Code-point set converts only the listed codes, sparse map, digest in manifest, nominal size | `CodePointSetTests` |
| Sparse code-point set reads back through the reader; absent codes are misses | `ReaderRoundTripTests` |
| Every named refusal writes nothing | `RefusalTests`, `CompositeRefusalTests` |
| Independent structural validation | `nk_core.fonts.validate_pgf_data` on the output |
