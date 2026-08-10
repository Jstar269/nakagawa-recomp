# PGF implementation source archaeology

**Status: technical provenance record, not legal advice or publication
clearance.** This record fixes the public-source findings for Nakagawa commit
[`77c3aba7a66be84a12ff507e35630644aaf89bd8`](https://github.com/Jstar269/nakagawa-recomp/commit/77c3aba7a66be84a12ff507e35630644aaf89bd8)
and supports [issue #98](https://github.com/Jstar269/nakagawa-recomp/issues/98).
It distinguishes source correspondence from any conclusion about protectable
expression. The technical archaeology is complete to the limit of presently
recoverable public evidence; the qualified determinations listed at the end
remain open.

## Result

Nakagawa's PGF parser/rasterizer is a materially translated implementation,
not an independently organized implementation of PGF format facts:

- sal063 introduced `src/rt/pgf.c` and `pgf.h` as a C port of PPSSPP
  `Core/Font/PGF.cpp`; Nakagawa retains 277 of sal063's 298 normalized
  `pgf.c` lines (93.0% of sal063 and 70.1% of current Nakagawa `pgf.c`).
- The bit reader, eager table/glyph construction, glyph field extraction,
  lookup/fallback ordering, metric writers, pixel writer, nibble-RLE loop, and
  subpixel blend correspond line by line or structurally to PPSSPP.
- PPSSPP introduced its implementation as an adaptation of JPCSP. Its warning
  that parts were copied from JPCSP and that the file was therefore regarded
  as GPLv3 is present in PPSSPP's first PGF commit. No recoverable PPSSPP PGF
  implementation predates that warning.
- JPCSP's principal glyph implementation is `SceFontInfo.java`, together with
  `PGF.java`, `Debug.java`, and `sceFont.java`; limiting the chain to
  `PGF.java` omits the bit, glyph, RLE, pixel, and ABI-writer ancestors.
- The older JPCSP core follows intraFont's bit reader, table extraction, glyph
  field sequence, and RLE organization. Public PGF descriptions independently
  establish many of the underlying format facts, so correspondence does not by
  itself determine which portions are protectable expression.
- Later PPSSPP work, rather than JPCSP, supplies Nakagawa's two-axis subpixel
  blend and invalid-pixel-format guard.
- Nakagawa-first-visible and later Nakagawa work adds ownership and bounds
  hardening, memory/file wrappers, cleanup, HST style integration, by-ID
  drawing, exact VRAM dirty ranges, wide-path support, and the public-safe
  fail-closed seam.

Fixed artifact identities:

| Artifact | Git object |
| --- | --- |
| Nakagawa `src/rt/pgf.c` at `77c3aba7` | `0f000e3e72383991f33b83259ea33ad10de506ca` |
| Nakagawa `src/rt/pgf.h` at `77c3aba7` | `8fae4fc7bcea3ceed210cdc496d4f2a359b6f5df` |
| sal063 initial `src/rt/pgf.c` | `d0e3cba302f9505f874986ef43bd0ab4eaf3792f` |
| sal063 initial `src/rt/pgf.h` | `ffce85acb6d55624e909600168b9775a135c514d` |

The relevant upstream license and attribution documents inspected were
sal063's [LICENSE](https://github.com/sal063/PSP-recompilation-project/blob/da17b0e1db209206a407d097d132201e516e3855/LICENSE)
and [CREDITS.md](https://github.com/sal063/PSP-recompilation-project/blob/496b8856f500b4a24242ccfe2d00141383478b61/CREDITS.md),
PPSSPP's [LICENSE.TXT](https://github.com/hrydgard/ppsspp/blob/4e109dd6ae34cbcb39751bb7647d345569700161/LICENSE.TXT),
JPCSP's [COPYING](https://github.com/jpcsp/jpcsp/blob/cd20cf312b358b4260f26f6754f9c62926c70ba6/COPYING),
and intraFont's [LICENSE](https://github.com/tpimh/intraFont/blob/5164d99ccb14041e2837f79877c34b2a50b65021/LICENSE).

## Reconstructed lineage

### Public PGF facts and intraFont

- A [2006-2007 PSP development thread](https://forums.ps2dev.org/viewtopic.php?t=5248)
  credits Skylark and FreePlay with early PGF decoding and records the header,
  maps, pointer table, glyph bit widths, signed bearings, row orientation, and
  RLE organization before or contemporaneously with intraFont.
- [YAPSPD section 26.9](https://hitmen.c02.at/files/yapspd/psp_doc/chap26.html#sec26.9)
  independently documents the 4-bpp stream, low-bit-first fields, horizontal
  and vertical row order, and the two nibble-RLE cases.
- [`fe79dcf62e242c5a9e346c9745a15f77f951f166`](https://github.com/tpimh/intraFont/commit/fe79dcf62e242c5a9e346c9745a15f77f951f166)
  (2007-11-13) introduced intraFont 0.1. Its
  [`intraFont.c`](https://github.com/tpimh/intraFont/blob/fe79dcf62e242c5a9e346c9745a15f77f951f166/intraFont.c)
  already contains the LSB-first bit loop, packed-table reads, charmap/pointer
  loading, 14/7/7/7/7 glyph prelude, signed bearings, shadow ID, advance,
  glyph pointer calculation, and the same two-branch nibble-RLE loop.
- [`5164d99ccb14041e2837f79877c34b2a50b65021`](https://github.com/tpimh/intraFont/commit/5164d99ccb14041e2837f79877c34b2a50b65021)
  (2009-03-19) is the latest recoverable intraFont revision before JPCSP's PGF
  implementation. Its [README](https://github.com/tpimh/intraFont/blob/5164d99ccb14041e2837f79877c34b2a50b65021/README#L125-L153)
  credits Skylark/FreePlay and pgeFont; its
  [license](https://github.com/tpimh/intraFont/blob/5164d99ccb14041e2837f79877c34b2a50b65021/LICENSE)
  states CC BY-SA 3.0.
- The accessible [pgeFont source](https://github.com/KapLex/PGE/blob/453368809c11d4b3ff485bd1337a1b8282332ef5/pgeFont.c)
  contains FreeType, texture, cache, and swizzle machinery, not the PGF bit
  parser, glyph-field decoder, or RLE core. No pgeFont-specific PGF block was
  identified in Nakagawa's retained core.

### JPCSP

- [`900dcea943579f35d65f4e4b336f4c7f78478652`](https://github.com/jpcsp/jpcsp/commit/900dcea943579f35d65f4e4b336f4c7f78478652)
  (2010-05-20) first added `PGF.java`.
- [`f82a15e3f50dd82c87acec2c05c5c12a8ef64d3f`](https://github.com/jpcsp/jpcsp/commit/f82a15e3f50dd82c87acec2c05c5c12a8ef64d3f)
  (2010-05-26) first added `SceFontInfo.java`.
- [`5fc8b3961f98e3a021af065f1b443eba91e9d752`](https://github.com/jpcsp/jpcsp/commit/5fc8b3961f98e3a021af065f1b443eba91e9d752)
  (2011-03-25) materially expanded the PGF findings and rendering path.
- [`e7c70edf49cad2e4663298e3d59d4b61938b7ab7`](https://github.com/jpcsp/jpcsp/commit/e7c70edf49cad2e4663298e3d59d4b61938b7ab7)
  (2012-12-04) is the closest complete JPCSP snapshot before PPSSPP's PGF
  work. At that revision,
  [`SceFontInfo.java`](https://github.com/jpcsp/jpcsp/blob/e7c70edf49cad2e4663298e3d59d4b61938b7ab7/src/jpcsp/HLE/kernel/types/SceFontInfo.java)
  contains the bit reader, table/glyph construction, lookup, glyph parser, RLE
  drawing, and char-info organization, while
  [`PGF.java`](https://github.com/jpcsp/jpcsp/blob/e7c70edf49cad2e4663298e3d59d4b61938b7ab7/src/jpcsp/format/PGF.java)
  supplies the header and table parser. `SceFontInfo.java` expressly credits
  BenHur's intraFont structure/findings. `Debug.java` supplies the pixel-writer
  organization and `sceFont.java` the font-info guest layout.
- Later JPCSP commits added the index-versus-inline dimension/bearing branches
  ([`d9d33419`](https://github.com/jpcsp/jpcsp/commit/d9d33419ab7f9ffd758011bc1d3576b6ec213c2d)),
  advance handling
  ([`4f2b105f`](https://github.com/jpcsp/jpcsp/commit/4f2b105fbdcc1b4d0ba62946f398b1480d5b17f8)),
  and subpixel handling
  ([`c058f11d`](https://github.com/jpcsp/jpcsp/commit/c058f11d9529d3bf105fe4f222f0ac04bd4a645d)).
  JPCSP's subpixel path ignores the vertical fraction and is not the ancestor
  of Nakagawa's two-axis formula.
- The relevant JPCSP files carry GPL-3.0-or-later headers; the repository's
  pinned [COPYING](https://github.com/jpcsp/jpcsp/blob/cd20cf312b358b4260f26f6754f9c62926c70ba6/COPYING)
  contains GPLv3.

### PPSSPP

- [`ba0362d817130f68cfdc33f9558ece78a4347a53`](https://github.com/hrydgard/ppsspp/commit/ba0362d817130f68cfdc33f9558ece78a4347a53)
  (authored 2013-02-22, committed 2013-02-27) first added `Core/Font/PGF.cpp`
  and `PGF.h`. The initial
  [`PGF.cpp`](https://github.com/hrydgard/ppsspp/blob/ba0362d817130f68cfdc33f9558ece78a4347a53/Core/Font/PGF.cpp#L18-L42)
  already contains the JPCSP-copying/GPLv3 warning. Git history and blame place
  the warning in this first commit; no earlier PPSSPP PGF implementation exists.
- Later commits introduced inherited blocks still visible in Nakagawa: indexed
  metrics ([`2428d3f5`](https://github.com/hrydgard/ppsspp/commit/2428d3f5c584e222c6938e8f9c46dca97ac736dc)),
  descender/glyph-count/advance/bit-reader/consume/ascender/shadow/fallback work
  ([`68af1ea6`](https://github.com/hrydgard/ppsspp/commit/68af1ea6f8e53ab26bf0d7343493eb280ab322c7),
  [`2d7741a4`](https://github.com/hrydgard/ppsspp/commit/2d7741a433efdbeadf6c83e447b28046250a5cad),
  [`e77b8bc5`](https://github.com/hrydgard/ppsspp/commit/e77b8bc5b56909b5e2b875e3b5fbbb0ef7b17547),
  [`d446659b`](https://github.com/hrydgard/ppsspp/commit/d446659b2ab17dcf94098a4d194d44d3180955c1),
  [`bb94f31e`](https://github.com/hrydgard/ppsspp/commit/bb94f31ec75ae7206f51833ce3181a7514f5efbc),
  [`4505cb4b`](https://github.com/hrydgard/ppsspp/commit/4505cb4b3ad06844ecaf23ac729c5a2876b68923),
  [`769ffbd3`](https://github.com/hrydgard/ppsspp/commit/769ffbd33b1d565a1fdc82893a7c644fc309d3a2),
  [`8f6315e3`](https://github.com/hrydgard/ppsspp/commit/8f6315e3750c765575b6004d553014c5f8c22bf4)),
  and the char/shadow split with the current three-part shadow-flags expression
  ([`131cbc07`](https://github.com/hrydgard/ppsspp/commit/131cbc073c3f43ed821eaef3aacd7dbe3a787446)).
- [`1860180c`](https://github.com/hrydgard/ppsspp/commit/1860180c2e870b2b4f6a080ca89631d966dc1878)
  and [`621f86c7`](https://github.com/hrydgard/ppsspp/commit/621f86c7f1feb816256baa000fec970b6ec6b359)
  (2016-01-23) introduced the decoded 8-bit buffer and two-axis blend ending in
  `>> 12`. [`eacebd4c`](https://github.com/hrydgard/ppsspp/commit/eacebd4c4106c364aeb7bd87549c3a68e8e176e8)
  added the no-draw-before-first-glyph behavior, and
  [`5abec6ca`](https://github.com/hrydgard/ppsspp/commit/5abec6ca18c3fc328d04f903b4a21c2f9326df56)
  added parser bounds work.
- [`ca4a0a848a1fc2896236a49f3e8f046eef73cff1`](https://github.com/hrydgard/ppsspp/commit/ca4a0a848a1fc2896236a49f3e8f046eef73cff1)
  (2022-08-13) added the invalid-pixel-format guard retained by sal063 and
  Nakagawa. It is therefore the earliest unconditional PPSSPP content revision
  consistent with every identified inherited feature.

### sal063 and Nakagawa

- sal063 root commit
  [`f3123f30fc422e9567fd7a538723a610d5c8f5f6`](https://github.com/sal063/PSP-recompilation-project/commit/f3123f30fc422e9567fd7a538723a610d5c8f5f6)
  (2026-06-14) introduced `pgf.c` and `pgf.h`; both expressly identify
  `Core/Font/PGF.cpp` and the C-port relationship.
- [`496b8856f500b4a24242ccfe2d00141383478b61`](https://github.com/sal063/PSP-recompilation-project/commit/496b8856f500b4a24242ccfe2d00141383478b61)
  added sal063's attribution and AI disclosure without changing PGF. Its
  [`CREDITS.md`](https://github.com/sal063/PSP-recompilation-project/blob/496b8856f500b4a24242ccfe2d00141383478b61/CREDITS.md#L19-L36)
  states that Anthropic Claude was used substantially across runtime/HLE work,
  including PPSSPP C++-to-C translation, and its
  [PGF row](https://github.com/sal063/PSP-recompilation-project/blob/496b8856f500b4a24242ccfe2d00141383478b61/CREDITS.md#L71-L85)
  itemizes `pgf.c`/`pgf.h` as a direct port. This establishes a disclosed
  AI-assisted translation campaign that included the PGF unit; public history
  does not contain file-specific prompts, transcripts, or model patches.
- sal063's PGF files did not change through its current
  [`da17b0e1db209206a407d097d132201e516e3855`](https://github.com/sal063/PSP-recompilation-project/commit/da17b0e1db209206a407d097d132201e516e3855).
- Nakagawa's first recoverable commit is the parentless/squashed
  [`7ac90b25ca0f5bc790df424eb54b7fdfdb0e2830`](https://github.com/Jstar269/nakagawa-recomp/commit/7ac90b25ca0f5bc790df424eb54b7fdfdb0e2830)
  (2026-07-19). It already includes the parser refactor/hardening, memory API,
  cleanup, style tail, draw-helper split, and by-ID path; Git cannot identify
  individual authorship inside that pre-import interval.
- Later recoverable Nakagawa work added exact VRAM dirty ranges
  ([`5db5cc89`](https://github.com/Jstar269/nakagawa-recomp/commit/5db5cc89aecb234c73aa8ec23a8c6467639207f3)),
  the public-safe API seam and fail-closed backend
  ([`279370ef`](https://github.com/Jstar269/nakagawa-recomp/commit/279370ef9982c10487407ec1a95008bbbcdbe9e0)),
  and shared/wide-path file opening
  ([`dcf0a524`](https://github.com/Jstar269/nakagawa-recomp/commit/dcf0a52423bee9ab02d5740c905643a99cd4c264)).

## PPSSPP revision finding

The PGF source revision is narrowed but not proven exactly:

1. **Inherited-content floor:** `ca4a0a848a1fc2896236a49f3e8f046eef73cff1`
   contains every identifiable inherited feature, including the last-required
   invalid-format guard.
2. **Strongest exact-checkout candidate:**
   [`4e109dd6ae34cbcb39751bb7647d345569700161`](https://github.com/hrydgard/ppsspp/commit/4e109dd6ae34cbcb39751bb7647d345569700161)
   (2026-05-20), whose `PGF.cpp` blob is
   `b3503a727f7e19bb3f2df7847c3315a2955c6b45`. sal063 pins `mpeg.c` to this
   same full-repository revision, says a full PPSSPP tree existed under
   `third_party/ppsspp`, and its 1,619-entry generated NID table exactly matches
   generation from `Core/HLE/*.cpp` at this revision. The PGF content matches
   and lacks the later five-line PPSSPP GPU flush, as does sal063.
3. **Public-history upper endpoint:**
   [`72fdcb25d96039cbda314ec08728ee8f782b10a6`](https://github.com/hrydgard/ppsspp/commit/72fdcb25d96039cbda314ec08728ee8f782b10a6)
   is the last PPSSPP master revision before sal063's root timestamp. The NID
   output remained identical from `4e109dd6` through this revision. Between
   those endpoints the only `PGF.cpp` difference is the five-line GPU flush
   from [`e5689d1a`](https://github.com/hrydgard/ppsspp/commit/e5689d1adc544168b4cbd910bd8a83d8c820ca57),
   merged by [`b65bbaf9`](https://github.com/hrydgard/ppsspp/commit/b65bbaf92c3776afdb6bf821a9f0cb23b782f591).

The PGF row in sal063's credits does not name a revision, and the untracked
checkout is absent from public Git. Therefore `4e109dd6` is the most probable
repository snapshot, not a proven exact ancestor. The defensible public result
is a content floor at `ca4a0a8`, a strongly corroborated May-June 2026 checkout
family, and an upper public-history endpoint at `72fdcb25`.

## Function and block matrix

Revision shorthand:

- `S`: [sal063 `f3123f30` `pgf.c`](https://github.com/sal063/PSP-recompilation-project/blob/f3123f30fc422e9567fd7a538723a610d5c8f5f6/src/rt/pgf.c)
- `P0`: [PPSSPP initial `ba0362d8`](https://github.com/hrydgard/ppsspp/blob/ba0362d817130f68cfdc33f9558ece78a4347a53/Core/Font/PGF.cpp)
- `Pcand`: [PPSSPP candidate `4e109dd6`](https://github.com/hrydgard/ppsspp/blob/4e109dd6ae34cbcb39751bb7647d345569700161/Core/Font/PGF.cpp)
- `J12`: [JPCSP `e7c70edf` `SceFontInfo.java`](https://github.com/jpcsp/jpcsp/blob/e7c70edf49cad2e4663298e3d59d4b61938b7ab7/src/jpcsp/HLE/kernel/types/SceFontInfo.java)
- `I07`: [intraFont `fe79dcf6`](https://github.com/tpimh/intraFont/blob/fe79dcf62e242c5a9e346c9745a15f77f951f166/intraFont.c)

Here, "near-verbatim" means textual retention or close line-by-line language
translation. It is a source-correspondence description, not a conclusion about
protectability.

| Nakagawa range at `77c3aba7` | sal063 ancestor | PPSSPP ancestor/revision | JPCSP/intraFont ancestor | Classification and evidence | Confidence |
| --- | --- | --- | --- | --- | --- |
| `pgf.c:20-30` flags | `S:14-24`, identical | `PGF.h` at `Pcand`; base constants in `P0` | `J12` constants | Near-verbatim declarations mixed with format facts; same names, values, order, and dual `0x20` use | High |
| `32-56` `PGFHeader` | `S:26-50`, identical | `PGFHeader` at `Pcand` | `PGF.java` fields | Structural layout port mixed with binary-format facts; same packed order and padding | High |
| `58-78` glyph/font state | `S:52-72`, identical | `Glyph`, `PGF`, and `ReadPtr` at `Pcand` | `J12` fields/constructor | Selective C translation and sal storage glue; same paired tables, maps, glyph array, first glyph, and font data | High |
| `82-90` `pgf_getBits` | `S:76-84`, identical | Simple loop in `P0`; optimized at `d446659b` | `J12 getBits`; `I07 intraFontGetV` | Near-verbatim cross-language structure; same LSB-first loop and result-bit placement | High |
| `91-95` `pgf_consume` | `S:85-89`, identical | `consumeBits`, `bb94f31e` | Increment performed inline | Translated PPSSPP helper: read, advance by width, return | High |
| `96-98` `rd32` | `S:90-92`, identical | Endian-aware types, no matching helper | JPCSP `readWord` | sal063 C portability glue implementing a little-endian format fact differently | High |
| `100-134` `read_char_glyph` | `S:94-128`, identical | `ReadCharGlyph` at `Pcand`; `131cbc07`, `2428d3f5` | `J12 getGlyph`; later JPCSP metric commits; `I07` core prelude | Near-verbatim structural translation: exact field order, signed correction, shadow assembly, four metric branches, and pointer result | High |
| `136-149` parse ownership/header/validation | `S:130-143` has file allocation/header/magic | `ReadPtr` prelude; later PPSSPP bounds work | `PGF.java` header parser | Mixed inherited parse prelude and Nakagawa-first-visible ownership/caps/error handling | High; individual pre-import author unknown |
| `150-181` rev3/tables/shadow skip | `S:144-165` | `ReadPtr` at `Pcand` | `PGF.java`, `J12`, intraFont loader | Structurally ported parse order; sal macro plus Nakagawa bounds | High |
| `183-211` maps/pointers/glyph construction | `S:167-190` | `ReadPtr` at `Pcand` | `J12` constructor; intraFont loader | Structurally ported: aligned tables, invalid-to-65535, glyph count, `*4*8`, eager glyph generation | High |
| `218-224` `pgf_open_memory` | Absent | No matching surface | No matching surface | Nakagawa-first-visible lifetime/hardening: private copy, size cap, safety padding | High |
| `226-246` file/open/wide open | File prologue in `S` | Generic loading only | Generic loading only | Mixed generic inherited I/O and Nakagawa refactor; `_wfopen` added at `dcf0a524` | High |
| `248-253` `pgf_close` | Absent | Destructor behavior only | Object lifetime only | Nakagawa-first-visible explicit C resource cleanup | High |
| `255-262` `get_char_glyph` | `S:194-201`, identical | `GetCharGlyph`; `eacebd4c` guard | `J12 getCharGlyph` | Near-verbatim structural port, trimmed of PPSSPP compressed/shadow path | High |
| `264-269` `pgf_has_char` | `S:203-208`, identical | No exact helper | No exact helper | sal063 API glue over derived lookup; thin width/height predicate | High |
| `271-294` font-info core | `S:210-233`, nearly exact | `GetFontInfo` at `Pcand` | `sceFont.java` writer | Structurally ported ABI writer mixed with layout facts; same offsets, ten `/64` floats, dimensions, and glyph count | High |
| `295-310` style/name tail | Only sal bpp write | `PGFFontStyle` layout | JPCSP style writer | Nakagawa-first-visible HST/style glue: hardcoded family/style, U+3042 test, selected filename, bpp | Medium-high |
| `312-337` `pgf_get_char_info` | `S:237-262`, identical | `GetCharInfo`; 2013 metric/shadow/fallback commits | `J12 getCharInfo` and HLE type | Near-verbatim structural port with guest-memory syntax; same zero/fallback/order/offset calculations | High |
| `339-359` `set_font_pixel` | `S:264-284`, identical | `SetFontPixel`; guard at `ca4a0a8` | `Debug.setFontPixel` | Near-verbatim translation: same size array, width calculation, nibble condition, and 8/24/32-bit writes | High |
| `361-382` draw setup | `S:286-310` | `DrawCharacter`, `GlyphImage` | JPCSP texture generation | Structural port plus Nakagawa helper split/allocation checks | High |
| `383-394` RLE decoder | `S:310-321`, identical | `DrawCharacter` at `Pcand` | JPCSP render loop; `I07:82-106` | Near-verbatim structural lineage over independently documented RLE facts; same two branches, nested loop, and 4-to-8-bit expansion | High |
| `396-410` sample/subpixel | `S:323-338`, identical | `1860180c`, `621f86c7` | JPCSP `c058f11d` differs and ignores vertical fraction | Near-verbatim later PPSSPP expression: same orientation index, edge pixels, two horizontal blends, vertical blend, `>>12` | High |
| `414-434` VRAM dirty ranges | Absent | Whole-buffer invalidation/later GPU flush only | No matching block | Later Nakagawa integration with materially different row/byte clipping; added at `5db5cc89` | High |
| `438-446` public draw wrapper | Fallback/setup in `S` | `DrawCharacter` fallback | JPCSP alternate-character path | Ported fallback structure around Nakagawa's split helper | High |
| `448-451` draw by ID | Absent | No matching wrapper | No matching wrapper | Nakagawa-first-visible API glue | High |
| `pgf.h:1-35` | sal header comments largely retained | Names `Core/Font/PGF.cpp` and PPSSPP layouts | No direct header ancestor | Inherited sal063 provenance/feature narrative plus later API-seam move | High |
| `pgf_api.h`, `pgf_unavailable.c` | Absent | Absent | Absent | Nakagawa-authored independent release seam; all unavailable operations fail closed | High |

The lineage-sensitive current ranges are `pgf.c:20-78`, `82-134`, structural
parts of `143-211`, `255-262`, `271-294`, `312-410`, and `438-446`. The
clearest later integration expression is `96-98`, the added ownership and
validation paths, `218-253`, `264-269`, `295-310`, `414-434`, and `448-451`.

## Facts versus current expression

Public sources can support an independent requirements document for the
`PGF0` signature, observable header/table fields, field widths and signedness,
little-endian storage and 32-bit padding, charmap/pointer roles, LSB-first bits,
the two RLE cases, bitmap orientation, PSP font/pixel/guest structures, and
fixed-point `/64` scaling. Public declarations and tests include the forum and
YAPSPD sources above and pinned PSPAutotests
[`libfont.h`](https://github.com/hrydgard/pspautotests/blob/1dcefebeb0e9f3b3597ce1c97c0eac92088d8e6b/tests/font/libfont.h),
[`charinfo.cpp`](https://github.com/hrydgard/pspautotests/blob/1dcefebeb0e9f3b3597ce1c97c0eac92088d8e6b/tests/font/charinfo.cpp),
and [`shadowinfo.cpp`](https://github.com/hrydgard/pspautotests/blob/1dcefebeb0e9f3b3597ce1c97c0eac92088d8e6b/tests/font/shadowinfo.cpp).

The present implementation correspondence is broader than those facts: it uses
the same eager state/table/glyph decomposition, field sequence, four repeated
metric branches, three-expression shadow-flags assembly, lookup/fallback
ordering, decoded-buffer/RLE/sample organization, two-axis blend formula, and
pixel-size-array/switch organization.

## Smallest remaining factual unknowns

- The missing sal063 `third_party/ppsspp` checkout and its Git metadata.
- File-specific Claude prompts, transcripts, context, or patches for PGF.
- Recoverable Skylark/FreePlay source behind the early `ttf2pgf` work, beyond
  the public format descriptions.
- Pre-import author/development records for changes first visible in Nakagawa's
  squashed `7ac90b25` commit.
- Hardware-backed answers for ambiguous rev3, shadow, clipping, subpixel, and
  malformed-input behavior needed for a genuinely independent replacement.

These are not remaining source-archaeology tasks: whether any corresponding
material is protectable expression, whether intraFont terms have a downstream
effect, the retained-versus-replace choice, final SPDX/copyright/notice/change
presentation, consistency of the exact source and binary distribution, and
publication clearance. Those remain for qualified human review of the actual
candidate.
