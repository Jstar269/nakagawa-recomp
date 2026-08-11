# PGF implementation qualified-review packet

**Status: engineering provenance evidence for qualified review, not legal
advice or publication clearance.** This packet supports qualified legal review for PGF implementation licensing and describes
the implementation at Nakagawa commit
[`77c3aba7a66be84a12ff507e35630644aaf89bd8`](https://github.com/Jstar269/nakagawa-recomp/commit/77c3aba7a66be84a12ff507e35630644aaf89bd8).

The complete source-to-source evidence is preserved in
[`provenance/PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md`](provenance/PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md).
That record, rather than the former August 4 comparison against a modern
PPSSPP snapshot, is the factual basis for this packet.

## Decision requested

For the exact source and binary configuration proposed for distribution,
please determine:

1. whether the current `src/rt/pgf.c` and `src/rt/pgf.h` should be retained,
   excluded, partly replaced, or independently replaced;
2. what file-level SPDX, copyright, attribution, modification, and notice
   presentation is required if any current implementation is retained;
3. whether protectable intraFont expression flowed through JPCSP and PPSSPP,
   and what downstream effect, if any, follows; and
4. whether the resulting presentation is consistent across the exact source
   tree, build configuration, binary contents, and accompanying materials.

Do not answer these questions by changing SPDX text alone. This packet makes no
legal determination.

## Technical provenance conclusion

The public-source investigation is complete to the limit of presently
recoverable evidence. It establishes the engineering chain:

`Nakagawa pgf.c/h -> sal063 pgf.c/h -> PPSSPP PGF.cpp/h ->`
`JPCSP SceFontInfo.java + PGF.java + Debug.java + sceFont.java -> intraFont`

The last arrow records source correspondence and JPCSP's express intraFont
credit. It does not assert that intraFont's terms apply downstream.

Key facts:

- sal063 introduced its PGF files in
  [`f3123f30`](https://github.com/sal063/PSP-recompilation-project/commit/f3123f30fc422e9567fd7a538723a610d5c8f5f6)
  as a C port of PPSSPP `Core/Font/PGF.cpp`. Current Nakagawa retains 277 of
  sal063's 298 normalized `pgf.c` lines.
- sal063's
  [`496b8856` disclosure](https://github.com/sal063/PSP-recompilation-project/blob/496b8856f500b4a24242ccfe2d00141383478b61/CREDITS.md#L19-L36)
  records substantial Anthropic Claude assistance across runtime/HLE work,
  including PPSSPP C++-to-C translation, and separately itemizes PGF as a
  direct port. No file-specific prompt/transcript record is public.
- PPSSPP first introduced PGF in
  [`ba0362d8`](https://github.com/hrydgard/ppsspp/commit/ba0362d817130f68cfdc33f9558ece78a4347a53).
  Its [initial source](https://github.com/hrydgard/ppsspp/blob/ba0362d817130f68cfdc33f9558ece78a4347a53/Core/Font/PGF.cpp#L18-L42)
  already contains the JPCSP-copying/GPLv3 warning. No recoverable PPSSPP PGF
  code predates that warning.
- JPCSP first added `PGF.java` in
  [`900dcea9`](https://github.com/jpcsp/jpcsp/commit/900dcea943579f35d65f4e4b336f4c7f78478652)
  and `SceFontInfo.java` in
  [`f82a15e3`](https://github.com/jpcsp/jpcsp/commit/f82a15e3f50dd82c87acec2c05c5c12a8ef64d3f).
  The closest complete pre-PPSSPP snapshot is
  [`e7c70edf`](https://github.com/jpcsp/jpcsp/commit/e7c70edf49cad2e4663298e3d59d4b61938b7ab7).
  `SceFontInfo.java`, not just `PGF.java`, contains the principal glyph/RLE
  implementation and expressly credits BenHur's intraFont.
- intraFont 0.1 at
  [`fe79dcf6`](https://github.com/tpimh/intraFont/commit/fe79dcf62e242c5a9e346c9745a15f77f951f166)
  (2007) already contains corresponding bit-reader, table, glyph, and RLE
  organization. The last recoverable pre-JPCSP revision is
  [`5164d99c`](https://github.com/tpimh/intraFont/commit/5164d99ccb14041e2837f79877c34b2a50b65021),
  whose [license](https://github.com/tpimh/intraFont/blob/5164d99ccb14041e2837f79877c34b2a50b65021/LICENSE)
  states CC BY-SA 3.0.
- Contemporaneous public documentation independently records many PGF format
  facts. Commonality must therefore be assessed at the function/control-flow
  level rather than inferred from constants or field names alone.

## PPSSPP revision resolution

The exact checkout remains unproven, but the earlier statement that it is
wholly unrecoverable is now too broad:

| Revision fact | Result |
| --- | --- |
| Earliest PPSSPP content containing every inherited feature | [`ca4a0a848a1fc2896236a49f3e8f046eef73cff1`](https://github.com/hrydgard/ppsspp/commit/ca4a0a848a1fc2896236a49f3e8f046eef73cff1) |
| Strongest exact-checkout candidate | [`4e109dd6ae34cbcb39751bb7647d345569700161`](https://github.com/hrydgard/ppsspp/commit/4e109dd6ae34cbcb39751bb7647d345569700161) |
| Candidate `PGF.cpp` blob | `b3503a727f7e19bb3f2df7847c3315a2955c6b45` |
| Last public PPSSPP master before sal063's root | [`72fdcb25d96039cbda314ec08728ee8f782b10a6`](https://github.com/hrydgard/ppsspp/commit/72fdcb25d96039cbda314ec08728ee8f782b10a6) |

`4e109dd6` is the strongest candidate because sal063 pins another direct port
(`mpeg.c`) to that full-repository revision, records a complete local PPSSPP
tree, has a 1,619-entry NID table that exactly regenerates from that revision,
and matches its PGF content. The NID output remains identical through
`72fdcb25`; the intervening PGF difference is a five-line PPSSPP GPU flush that
sal063 does not carry. The PGF credits row itself contains no SHA and the local
checkout is absent from public Git. `4e109dd6` must therefore remain a candidate,
not be stored as a proven `upstream_revision`.

## Function/block result

The archaeology record contains the complete matrix. Its concise result is:

| Current surface | Technical classification |
| --- | --- |
| flags, header, glyph/font state | Ported declarations and state organization mixed with externally observable format facts |
| `pgf_getBits`, `pgf_consume` | Near-verbatim/translated PPSSPP-JPCSP-intraFont lineage |
| `read_char_glyph` | Near-verbatim structural translation, including distinctive field order, signed correction, shadow assembly, and four metric branches |
| parser table/map/pointer/glyph flow | Structurally ported eager parser with later sal063/Nakagawa ownership and bounds hardening |
| lookup, font info, char info | Structurally ported lookup/fallback and ABI-writer organization mixed with public PSP layout facts |
| pixel writer and RLE decoder | Near-verbatim structural translation; the RLE cases themselves are also publicly documented format facts |
| two-axis subpixel blend | Later PPSSPP-specific expression; JPCSP's later subpixel implementation differs materially |
| memory/file wrappers, cleanup, style tail, by-ID API | Nakagawa-first-visible integration work; individual pre-import authorship is unrecoverable |
| exact VRAM dirty ranges and wide-path open | Later attributable Nakagawa work |
| `pgf_api.h` and `pgf_unavailable.c` | Project-authored public-safe seam that fails closed |

This supports the ledger's `derived-translated` classification for the retained
PGF core. It does not classify any row as legally protectable or unprotectable.

## Independently specifiable requirements

An independent replacement can specify the `PGF0` signature, observable
header/table fields, field widths/signedness, little-endian storage and table
padding, charmap/pointer roles, low-bit-first bit packing, the two RLE cases,
bitmap orientation, PSP font/pixel/guest layouts, and fixed-point scaling from
public format/API evidence. Shadow, malformed-input, clipping, subpixel, and
error-precedence details need additional public or hardware-backed behavioral
evidence.

The present expression is not reduced to those facts: it retains the same
state decomposition, eager construction order, extraction sequence, repeated
metric branches, shadow-flags assembly, fallback ordering, decoded-buffer/RLE
organization, pixel switch, and PPSSPP blend formula.

## Current repository posture

- The repository-level declaration is GPL-3.0-or-later. That substantially
  narrows the former project-level GPL2-versus-JPCSP-GPL3 version-selection
  mismatch, but does not decide file notices, intraFont downstream effect, or
  the exact distribution presentation.
- `src/rt/pgf.c` and `pgf.h` remain labelled GPL-2.0-or-later. This packet does
  not prescribe a replacement identifier or change their metadata.
- [`public-safe-v1`](PUBLIC_SOURCE_PROFILE.md) already excludes `pgf.c`,
  `pgf.h`, and all bundled PGF fonts and builds `pgf_unavailable.c`. The seam
  returns unavailable/failure rather than synthetic font or glyph success.
- Replacement-font rights remain documented in [THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt](../THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt).

## Conservative engineering choices

1. **Exclude PGF from the initial public source.** Keep `public-safe-v1` and its
   fail-closed seam. This is already implemented and does not decide final PGF posture.
2. **Retain the current implementation.** Have a qualified reviewer specify the
   exact SPDX/copyright/notice/change and combined-distribution presentation.
3. **Replace the lineage-sensitive core.** Preserve only independently written
   file/lifetime/API/VRAM glue and replace parser, bitstream, maps/pointers,
   glyph/metrics, RLE/orientation, pixel, blend, and ABI/error paths. A
   conservative estimate is roughly 300-325 of the current 451 physical
   `pgf.c` lines, depending on whether ABI writers are independently respecified.
4. **Perform a genuinely independent replacement.** Write a behavioral
   specification from permitted public/hardware evidence, use source-owned
   tests, record source restrictions and any AI prompts/context, and review the
   result for structural similarity. Given prior exposure, do not describe this
   retroactively as a clean-room process.

An external or optional backend is another packaging boundary if the current
implementation is not shipped. Host-font substitution is not behaviorally
equivalent to PSP PGF metrics, coverage, placement, shadow behavior, and raster
output.

## Residual gates

The source archaeology, function-level comparison, factual-versus-expressive
separation, and recoverable upstream-history tasks are substantially complete.
PGF license review remains open for exactly these gates:

1. qualified determination of protectable expression and any intraFont
   downstream effect;
2. retained-versus-exclude-versus-replace decision;
3. final SPDX, copyright, notice, attribution, and change presentation;
4. consistency across the exact intended source and binary distribution; and
5. qualified human review of the actual candidate.

The smallest remaining factual unknowns are the absent sal063 PPSSPP checkout,
file-specific Claude records, early Skylark/FreePlay source, pre-import
sal063-to-Nakagawa development records, and hardware evidence for ambiguous
behaviors needed by an independent replacement. None justifies representing
`4e109dd6` as an exact proven ancestor.

## Publication-lane update — 2026-08-06

Current-head facts for the reviewer, verified at export commit `dd0bcaea` during the
2026-08-06 publication-lane pass:

- `src/rt/pgf.c`/`pgf.h` remain labelled `GPL-2.0-or-later` and are **excluded from the
  `public-safe-v1` candidate** by `assets/public_source_profile.json`; the export build uses the
  fail-closed `pgf_unavailable.c` stub (`PUBLIC_SAFE=1`) and compiles cleanly. No SPDX expression was
  changed and no retaining configuration is proposed.
- `NOTICE.md` (post-IND-6) records the PGF row in the PPSSPP-derived inventory
  as `GPL-2.0-or-later / GPL-3.0-or-later (PPSSPP / JPCSP contributors)` and states the PPSSPP
  `PGF.cpp` GPLv3 warning explicitly, without resolving it.
- The four `font/*.pgf` binaries are outside this packet's source-code scope but remain excluded from
  the candidate pending replacement font review; a reviewer should not infer font redistribution rights from source
  treatment.
- The fresh public export materialized 608 audited files with 0 publication-audit findings; the
  candidate-tree manifest gate is the engineering artifact to review, not this packet alone.

The packet's original decision request (retain vs replace, license expression, notices, clean-room
record) remains open under PGF license review. The engineering posture for the initial public release is exclusion,
not retention.

### Supporting research-corpus context (PSPRecompWiki, sections 06/07)

Methodology and prior-art framing from the project's research corpus that a reviewer may want
alongside the pinned source references above:

- Wiki doc 61 (Prior Art and Source Atlas) indexes the exact citation family this packet relies on —
  PPSSPP, JPCSP, and intraFont are all catalogued as prior-art/source entities with scope and license
  notes — and warns that PSP-Archive mirrors are preservation sources, not original upstream
  provenance. The packet's candidate lineage `Nakagawa → PPSSPP → JPCSP → intraFont` matches that
  atlas's "source lineage graph" recommendation (immediate vs ultimate provenance, recorded as a
  chain rather than a single winner).
- Wiki doc 64 (Source Preservation) prescribes pinning exact revisions/blob IDs and phrasing
  provenance findings as "first visible in available repository history" rather than "created on this
  date." The packet's 2026-08-04 follow-up already uses that discipline (`7ac90b25…` root commit,
  PPSSPP blob IDs, "not recoverable from currently available repository evidence"); doc 64 confirms the
  methodology and suggests the search-recovery order (downstream forks retaining attribution, archived
  mirrors, old forums) if the exact PPSSPP import revision is ever sought again.
- Wiki doc 63 (Evidence Conflict Registry) records the epistemic rule that hardware measurement does
  not retroactively change source provenance (C-017) and that source similarity metrics do not decide
  protectability (C-012/§26 of doc 60) — both already reflected in this packet's "source correspondence,
  not a legal finding" language.
- Wiki doc 60 §30 documents why the publication audit reads the exact indexed Git blob rather than the
  working-tree file; a reviewer verifying this packet's claims should pin the same committed revision
  it cites.
