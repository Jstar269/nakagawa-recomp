# Clean-room behaviour specification — runtime PSP PGF reader

Status: spec — no code in this document. Written under the campaign protocol
(`../INDEPENDENCE_CAMPAIGN.md` §5) as a **spec-session** output for rewrite unit
**G4** (verdict (a), campaign §4). The spec session read the excluded derived
reader and its internal header as a behaviour reference, plus the public sources
in §9; nothing from those derived files is reproduced here beyond the spellings
`src/rt/pgf_api.h` fixes and independently supported format facts.

## 0. Scope, workspace, and reading rule

**In scope.** The public runtime reader that opens a user-owned PGF from a host
path or an in-memory copy; validates its bounded file layout; decodes its
character, pointer, metric, shadow, and bitmap data; and supplies the
guest-visible font, character, and glyph-image results promised by
`src/rt/pgf_api.h` [C1]. Pixels are placed into the caller's guest buffer with
the documented sub-pixel rule, clipped without touching memory outside the
image, and reported to the existing VRAM-dirty seam after a successful attempt
[C8], [C9].

**Out of scope.** Converting outlines into PGF (#313); acquiring, licensing,
redistributing, or admitting firmware font payloads (#300); guest `libfont.prx`
startup and its other HLE behaviour (#299); text shaping, font fallback policy,
bidirectional layout, and game-specific typography; VRAM ownership and the
meaning of guest error values owned by the HLE layer. Consumer routing remains
in the HLE layer: a missing or unsupported font must remain visible there and
must not be replaced by a synthetic success in the reader (project decision:
the reader has no title policy and cannot manufacture a valid font).

**Reading rule.** Every numbered behaviour cites `[Cn]` from §9 or is marked
`project decision` with its rationale. A fact with neither is a defect in this
spec — resolve it before implementing.

**Workspace allow-list** (campaign §5.2; export exactly these, no `.git`):
this spec; `src/rt/pgf_api.h`; the unchanged guest-memory and dirty-region
interfaces `src/rt/recomp.h` and `src/rt/ge_shared.h`; source-owned synthetic
fixture and test files authored from §8; and the minimum build files needed to
compile the reader and those tests. The implementation workspace must not
include repository history, private paths, retail fonts, converter inputs,
excluded readers, or upstream source. Public URLs needed to interpret this
spec are the citations in §9 only.

**Deletion list.** No derived implementation is in the allow-list. If the
export contains an excluded `src/rt/pgf.c` or internal PGF header, or anything
under `build/`, `logs/`, `oracle/`, `place_game_here/`, delete it and record the
deletion before implementation. The excluded reader may later be used only as
a black-box oracle outside the clean workspace under campaign §5.3; its text
must never enter that workspace.

## 1. Reader role

The reader is the public-safe boundary between an untrusted font file and the
PSP font API. It owns one immutable, fully bounds-checked in-memory image for
the lifetime of an open handle, validates all structural relationships before
returning that handle, and answers character and drawing requests without
consulting host-global state [C1]–[C4]. A successful open means every glyph
record addressed by the font has already been located and its metric-table
references checked; bitmap bytes may remain packed until a draw request
(§3.7, project decision: eager record validation turns a later malformed
metric reference into a visible open failure instead of a delayed decode
failure).

The reader supplies facts, not font policy. It does not choose a substitute
font, infer a language from a filename, relax a malformed input, or return a
partially decoded glyph. A format feature outside §§3.1–3.11, an unsupported
revision, a truncated table, or an inconsistent pointer, metric reference, or
shadow reference is named as unsupported or invalid at the API boundary and
fails closed [C1]–[C4]
(project decision: the current public seam has no error-string accessor; the
owner of that ABI extension is listed in §7).

## 2. Normative interface (`SEAM:` identifiers)

The spellings in this section are fixed by `src/rt/pgf_api.h` and are the only
reader-interface identifiers the implementation needs [C1]. Each project seam
is marked below; the opaque `SEAM: PGF` handle is non-null exactly while an
image is open. `SEAM: pgf_open_w` exists only on Windows, matching the header's
`_WIN32` guard.

| Operation | Normative result |
| --- | --- |
| `SEAM: pgf_open` | Opens the named host file through the platform's ordinary path interface. Success returns one owned handle; null path, open failure, size outside §3.7, or format failure returns null and leaves no reader-owned allocation. |
| `SEAM: pgf_open_w` | On Windows, has the same ownership, validation, and failure semantics as `pgf_open`, expressed with the wide host path supplied by the caller. Null or unopenable path returns null. |
| `SEAM: pgf_open_memory` | Validates and opens a caller image without retaining the caller's storage. Success owns a byte-for-byte copy that remains valid for the handle lifetime; null data, zero size, size outside §3.7, or validation failure returns null. Mutating the caller's buffer after return cannot change the handle. |
| `SEAM: pgf_close` | Releases exactly one non-null handle and all storage derived from it. Null is a no-op. A handle must not be used after close. Double close through a stale non-null value is undefined because the interface cannot distinguish it from reuse. |
| `SEAM: pgf_has_char` | Returns 1 only for a non-null handle and a character that resolves to a present positive-area glyph; otherwise returns 0. It has no destination side effects. |
| `SEAM: pgf_get_font_info` | For a non-null handle, writes the complete 0x108-byte `SceFontInfo` image described in §3.8. Null handle or invalid guest destination is a no-op. |
| `SEAM: pgf_get_char_info` | With a valid destination, clears all 60 bytes before lookup; a resolved glyph replaces that zero record and returns 1, while a null handle or failed lookup returns 0 and leaves the zeros. An invalid destination receives no write and returns 0. |
| `SEAM: pgf_draw_glyph` | Resolves the primary character, applies the fallback rule in §3.9, and performs the §3.11 draw. It returns 1 when the draw request is valid, even if clipping leaves no changed pixel, and 0 for every failure named in §§3.7, 3.9, or 3.11. |
| `SEAM: pgf_draw_glyph_by_id` | Performs the same validated draw directly for a zero-based glyph identifier in the parsed glyph table. It does not consult the character map. |

The header fixes types and names, not ownership or detailed failure outcomes.
The handle, copy, complete-record, and return conventions in the table are
project decisions: they make every failure visible and prevent malformed
input from exposing partially initialized state, as required by [C2].

The integer destinations and pointers inside guest records are PSP guest

addresses, not host addresses. Each record is read or written as one complete,
bounds-checked guest span through the unchanged project memory seam [C8]; the
reader never forms a host pointer from unchecked guest arithmetic. `SceFontInfo`
is 0x108 bytes, `SceFontCharInfo` is 0x3C bytes, and `SceFontGlyphImage` is
0x18 bytes [C5]. Their field semantics are normative in §§3.8–3.11.

For a drawing request, an inaccessible or truncated `SceFontGlyphImage` fails
with return 0 before any pixel write. `bufferPtr` is likewise a guest address.
A valid request writes only the image's clipped in-range bytes and reports
those spans through the unchanged dirty-region interface [C8], [C9].
Destination-validation failure is a project decision: the seam has no error
return for `pgf_get_font_info`, so that operation is a no-op, while the integer
returning queries return 0.

## 3. File layout and decoding contract

### 3.1 Header and section directory

All PGF multibyte fields are little-endian. The 392-byte base header begins at
file offset zero. Revision 3 uses a 412-byte header whose final 20 bytes declare
two additional record tables; revisions 0, 1, and 2 use the base header [C3],
[C5], [C11]. The following are structural offsets, not copied font data:

| Offset | Width | Header fact |
| --- | --- | --- |
| `0x0000` | 2 | Header offset; must be zero. |
| `0x0002` | 2 | Header size; 392 for revisions 0–2, 412 for revision 3. |
| `0x0004` | 4 | ASCII magic `PGF0`. |
| `0x0008` | 4 | Signed revision; supported values are 0–3. |
| `0x000C` | 4 | Signed version; must be nonnegative but does not select decoding. |
| `0x0010` | 4 | Character-map entry count. |
| `0x0014` | 4 | Character-pointer entry count. |
| `0x0018` | 4 | Character-map bits per entry. |
| `0x001C` | 4 | Character-pointer bits per entry. |
| `0x0020` | 2 | Reserved bytes. |
| `0x0022` | 1 | Reported source bits per pixel. |
| `0x0023` | 1 | Reserved byte. |
| `0x0024` | 4 each | Horizontal size, vertical size, horizontal resolution, and vertical resolution, in that order. |
| `0x0034` | 1 | Reserved byte. |
| `0x0035` | 64 | Raw font-name field. |
| `0x0075` | 64 | Raw font-type field. |
| `0x00B5` | 1 | Reserved byte. |
| `0x00B6` | 2 each | First and last character-glyph indices, in that order. |
| `0x00BA` | 26 | Reserved bytes. |
| `0x00D4` | 4 each | Maximum ascender, descender, left X adjustment, base Y adjustment, minimum center X adjustment, and top Y adjustment, in that order. |
| `0x00EC` | 4 each | Maximum horizontal and vertical advances, in that order. |
| `0x00F4` | 4 each | Maximum horizontal and vertical sizes, in that order. |
| `0x00FC` | 2 each | Maximum glyph width and height, in that order. |
| `0x00FE` | 2 | Reserved bytes. |
| `0x0102` | 1 each | Counts of dimension, X-adjustment, Y-adjustment, and advance metric records, in that order. |
| `0x0106` | 102 | Reserved bytes. |
| `0x016C` | 4 | Shadow character-map entry count. |
| `0x0170` | 4 | Shadow character-map bits per entry; 16 for the supported 16-bit entries. |
| `0x0174` | 4 | Unassigned fixed-point field; ignored by this reader. |
| `0x0178` | 4 each | Two signed 26.6 fixed-point shadow-scale values, in order. |
| `0x0180` | 8 | Reserved bytes, ending the 392-byte base header. |

The field names and metric-name meanings in this table follow the PSP SDK
font interface; the complete byte placement is corroborated by
the public PGF format description [C3], [C5]. The byte at `0x0022` is reported
as the source bits per pixel but is not used to choose a decoder (project
decision with rationale: the bitmap stream is independently specified as
4-bit samples; the field's exact historical meaning remains open question O-8).

For revision 3, offsets `0x0188` and `0x0190` each contain a 32-bit
bits-per-entry value; `0x018C` and `0x0194` each contain a 16-bit entry count;
`0x018E`–`0x018F`, `0x0196`–`0x0197`, and `0x0198`–`0x019B` are reserved
[C3]. Each declared revision-3 table entry occupies 4 bytes. The two tables
are opaque to this reader: their contents are neither interpreted nor exposed.
A nonzero count still consumes its complete section and is bounds-checked.

After the applicable header, sections occur in this exact order [C3]:

1. dimension records;
2. X-adjustment records;
3. Y-adjustment records;
4. advance records;
5. shadow character map;
6. for revision 3 only, the two opaque record tables in their header order;
7. packed character map;
8. packed character-pointer table;
9. glyph data, which occupies the remainder of the file.

A packed section containing *n* entries of *b* bits occupies the checked
number of whole 32-bit words needed for those entries: `4 × ceil(n × b ÷ 32)`
bytes. A metric section contains exactly 8 bytes per entry, and each revision-3
table contains exactly 4 bytes per entry [C3]. Rounding packed sections to a
whole 32-bit word rather than merely a whole byte is a project decision: the
public description's size expression is ambiguous at this boundary, observed
16-bit shadow entries expose the odd-count case, and O-5 keeps a hardware probe
open. The next section begins immediately after the rounded bytes; no extra
inter-section alignment is applied. All count products and derived end offsets
are checked before an offset is formed.

Validation at this stage is fail-closed. The 16 MiB image ceiling and
1,048,576 count ceilings are project decisions: they bound untrusted work while
covering the documented format ranges, and open question O-1 retains real-font
coverage evidence. Total image size is from 392 through 16 MiB inclusive; both
base packed widths are 1–32; the shadow width is 0 when
its count is zero and must be 16 when its count is nonzero; the first glyph is
no greater than the last; and both character-map and glyph counts are at most
1,048,576 [C2]–[C5]. The character map count is the number of character-map
entries, not a glyph count (project decision: PSP SDK documents that meaning
[C5], while open question O-3 records that a source-owned PSP probe should
confirm it). The character-pointer count must equal the inclusive glyph count
`last − first + 1` (project decision: otherwise two header facts disagree about
the table cardinality). Negative revision or version, a nonzero header offset,
the wrong header size, bad magic, an unsupported revision above 3, any invalid
width/count, or any section that does not fit causes open failure without
publishing partial state [C2]–[C5]. Other shadow widths and unproven revision-3 table
meanings remain named unsupported boundaries O-2 and O-5.

### 3.2 Packed character map

The character map is a packed array read low bit first, and then low
nibble first as fields cross byte boundaries [C3]. Its entry width is the
header's character-map width. Character lookup first forms the mathematical
index `char_code − firstGlyph` with checked signed arithmetic. A negative result,
or a nonnegative result not below the header
character-map count, is absent; `lastGlyph` is not an additional lookup bound.
The equality `firstGlyph + charMapLength − 1 = lastGlyph` is not required and
remains open question O-9. A zero-length map therefore represents a font with
no character-map entries rather than a structural failure.

For an in-range index, the reader extracts exactly that entry's width without
reading past the map section. An entry whose width is all ones is the absence
sentinel [C3]. Any other value is a zero-based glyph-table index. A value not
below the inclusive glyph count is treated as absent rather than wrapped,
aliased, or used as a host offset (project decision: it is a lookup miss, while
the pointer table itself remains validated at open). This makes malformed map
entries fail visibly as “character unavailable” without weakening structural
validation of the map bytes.

The lookup has no dependency on host locale or Unicode normalization. `int`
inputs outside the PSP character range and negative inputs follow the signed
subtraction rule above; no host signed-overflow operation is permitted.

### 3.3 Shadow character map and revision extensions

The shadow character map is a packed array of unsigned 16-bit character codes,
not another glyph-id table [C3]. Its supported width is therefore 16 bits per
entry; its data occupies two bytes per code and the section is rounded to a
whole 32-bit word, leaving two padding bytes after an odd count. Zero entries
consume no bytes; a nonzero count with any other width is a named unsupported
format and causes open failure under §3.7 (open question O-5 asks whether a
non-16 historical
encoding exists). The array is neither required to be sorted nor NUL
terminated. Bounds are the only validation at this stage; duplicate codes are
preserved in file order.

A primary glyph record can name one shadow. Its shadow identifier zero means
no shadow; a nonzero identifier selects the corresponding one-based entry in
the shadow character map (project decision: this reserves zero as the absence
value while allowing every declared map entry to be named). The selected code
is then resolved through the ordinary character map in §3.2. The resulting
shadow glyph's 14-bit byte offset locates its shadow metric prefix relative to
the start of the primary glyph record; that prefix is bounds-checked at open
(project decision: the 14-bit range and documented record adjacency make a
glyph-local offset representable without a font-sized absolute offset). The
shadow bitmap remains packed because the current seam has no shadow-image draw
call. A nonzero shadow identifier that is out of range, resolves to
an absent character, or names an invalid shadow record causes open failure. The
resulting shadow's row-order bits and the one-based raw identifier populate the
shadow fields in §3.9; this public seam does not expose a separate shadow-draw
operation.

Revision-3 header counts describe two consecutive arrays of 4-byte records,
each record containing two little-endian 16-bit values [C3]. This reader skips
both arrays after checking their byte sizes; it does not use their start/length
pairs to reinterpret the ordinary character map. Their declared bits-per-entry
values are retained as header facts but have no decoding effect. This keeps an
otherwise structurally valid revision-3 font on the same direct-map path while
making the uncompressed-map limitation explicit: a revision-3 font whose
ordinary map is absent or incomplete reports unavailable characters rather than
pretending the opaque tables were understood (project decision; open questions
O-1 and O-2 keep real-font revision and alternate-map coverage visible).

### 3.4 Character pointer table

The character-pointer table is a packed array in the same low-bit-first order
as the character map [C3]. Its entries are ordered by zero-based glyph
identifier, not by character code. For glyph identifier *g*, the extracted
unsigned value is multiplied by 4; the product is the byte offset of that
glyph's metric record from the first byte of glyph data. The factor 4 is part
of the format, not a host alignment choice [C3], [C11].

There is no per-entry absence sentinel in this table. Zero is a valid pointer
when a record begins at glyph-data offset zero, and duplicate pointers are
allowed when the format intentionally aliases records. The reader must not
infer record order, record length, or non-overlap from adjacent pointers.
Instead, every table entry is extracted with checked arithmetic, multiplied by
4 in a wide intermediate, and bounded against glyph-data size. A pointer whose
record prefix or variable metric groups do not fit completely causes open
failure. This stricter rule prevents a merely reachable start byte from hiding
a truncated record; bitmap extent is checked separately when pixels are
requested.

The pointer-width value may be 1–32, although 20 is the documented common
value [C3], [C11]. An all-ones pointer is not special: if its multiplied
offset cannot contain a complete record, open fails. No pointer is repaired,
clamped, rounded to a nearby record, or interpreted as a signed value.

### 3.5 Glyph metric record

A primary glyph record is a little-bit-order bit field. Bit 0 is the least
significant bit of its first byte; fields continue through low nibbles before
high nibbles [C3]. The fixed prefix is:

| Bit range | Width | Meaning |
| --- | --- | --- |
| 0–13 | 14 | Byte offset from this record's first byte to its shadow record; zero when no shadow offset is usable. |
| 14–20 | 7 | Unsigned bitmap width in pixels. |
| 21–27 | 7 | Unsigned bitmap height in pixels. |
| 28–34 | 7 | Signed 7-bit horizontal bitmap adjustment, range −64 through 63. |
| 35–41 | 7 | Signed 7-bit vertical bitmap adjustment, range −64 through 63. |
| 42–43 | 2 | Bitmap row order: 1 horizontal rows, 2 vertical rows, 3 three-character composite payload, 0 unsupported/unknown. |
| 44 | 1 | Metric-role bit retained by the format but not used to select the primary record layout; see open question O-4. |
| 45 | 1 | When 1, the dimension group is an 8-bit table index; when 0, it is an inline 64-bit pair. |
| 46 | 1 | When 1, the X-adjustment group is an 8-bit table index; when 0, it is an inline 64-bit pair. |
| 47 | 1 | When 1, the Y-adjustment group is an 8-bit table index; when 0, it is an inline 64-bit pair. |
| 48–54 | 7 | Unassigned bits; ignored. |
| 55–63 | 9 | Shadow identifier interpreted by §3.3. |

Bits 64 onward contain exactly three metric groups in dimension, X-adjustment,
Y-adjustment order, followed by one 8-bit advance-table index. Each of the
first three groups is either an 8-bit index into its corresponding two-column
metric table or an inline pair of signed 26.6 32-bit values [C3]. In low-bit
order, the first 32 bits are the horizontal/baseline value and the second 32
bits are the vertical/centre-or-top value:

| Group | Indexed values | Inline values |
| --- | --- | --- |
| dimension | width and height in 26.6 points | same |
| X adjustment | left-aligned and centre-aligned horizontal adjustment in 26.6 points | same |
| Y adjustment | baseline and top-aligned vertical adjustment in 26.6 points | same |
| advance | horizontal and vertical advance from the advance table in 26.6 points | no inline form |

The advance group is always the 8-bit index; a value at or above the declared
advance-table count causes open failure. An index for any other group at or
above its declared table count also causes open failure (project decision: a
missing metric is a structural font error, not a zero-filled measurement). The
record is 96 bits
when all three groups are indexed, then 56 bits larger for each inline group;
all four possible lengths are whole bytes. The primary bitmap starts at the
immediately following byte. A referenced shadow record has the same 48-bit
fixed prefix, is byte-aligned at the primary record's declared offset, and is
followed directly by its shadow bitmap; no primary-only extension is read from
that record.

The three index flags and fixed advance index follow the public format
description's bit-44/45–47 reading [C3]. Treating bit 44 as non-layout-bearing
and bits 45–47 as those three flags is a project decision where it diverges
from the excluded implementation; synthetic tests in §8 pin this reading and
a source-owned PSP probe remains open under O-4. Regardless of bit 44, a
record reached through the main pointer table is primary and a record reached
through a nonzero primary shadow offset is shadow. A shadow offset that is
zero, points before the end of the primary record, wraps, or leaves fewer than
six bytes for its prefix causes open failure when a nonzero shadow identifier
names it. Character information remains available for mapped records with row
order 0 or 3, but the current draw seam cannot rasterize either and does not
silently change them to rows 1 or 2 (§3.11).

### 3.6 Glyph bitmap stream

A raster glyph begins at the first byte after its complete metric record. Its
payload is a sequence of 4-bit alpha samples encoded as nibbles, with each
byte's low nibble preceding its high nibble [C3], [C4]. The decoder is
restarted for every draw; it carries no state from a previous glyph.

For each run, the control nibble is read first:

| Control | Meaning |
| --- | --- |
| 0–7 | Read one sample nibble and emit that sample `control + 1` times. |
| 8–15 | Emit the following `16 − control` sample nibbles literally, without replication. |

Decoding stops after exactly `width × height` samples. A run that would exceed
that count is truncated to the remaining pixels, and no later run is read. A
literal run reads only the samples needed to fill the remaining output. If the
glyph-data end arrives while a complete control-plus-required-sample sequence
is unavailable, the remaining samples are zero and decoding stops; trailing
bits too short to form a run are ignored (project decision: a bounded
transparent tail preserves deterministic output for a truncated final glyph
without reading beyond the owned image). This rule never reads outside the
font image and never loops waiting for more data. There is no per-glyph
compressed length in the file.

For row order 1, sample *i* belongs at `x = i mod width`, `y = i ÷ width`. For
row order 2, it belongs at `x = i ÷ height`, `y = i mod height`. Alpha is the
unsigned nibble 0–15 and is not rescaled by this step. Width or height zero
produces no samples. Their maximum is 127, so a format-valid record cannot
exceed 16,129 samples; §3.11 retains a 65,536-sample draw ceiling as a
defensive bound.

Row order 3 begins with three little-endian 16-bit character codes rather than
an RLE bitmap, representing a composite glyph [C3]. The current public draw
seam has no overlay operation, so `pgf_draw_glyph` and
`pgf_draw_glyph_by_id` reject row order 3 with return 0 and write nothing
(project decision; open question O-11 keeps composite rendering in the works
under #349). Character queries still return the record's dimensions and
metrics; primary row order remains internal because the ABI has no field for
it. Row order 0 is handled the same way for drawing. A shadow record's bitmap
uses the same RLE and row rules at its byte-aligned location, although the
current seam exposes only its row-order bits and identifier.

### 3.7 Open and validation

Opening is transactional. The reader first obtains an immutable owned image,
whether from a host file or a caller copy, and then derives every section and
glyph record from that image. No partially validated handle becomes visible,
no caller buffer is retained, and no parse failure writes to stderr [C1], [C2]
(project decision: the reader has no diagnostics channel in this seam and must
not turn invalid input into uncontrolled host output). An allocation failure is
an open failure, not a reduced-capacity handle.

Validation proceeds in this order:

1. Reject null path/data, a file that cannot be read, and an image outside 392
   bytes through 16 MiB inclusive.
2. Validate every fixed header field and derive section offsets using §3.1.
   All additions, multiplications, and rounded section sizes are checked before
   use.
3. Validate all table entries and metric references needed for complete open.
   The implementation chooses its bounded representation, but every allocation
   and any retained derived table or glyph metric is checked, and allocation
   failure returns null (project decision: representation is implementation
   detail; complete pre-publication validation is guest-visible).
4. Decode the direct character map according to §3.2. Values outside the
   glyph range remain lookup misses, not allocation-sizing inputs.
5. For every glyph identifier, decode its pointer and the complete primary
   record according to §§3.4–3.5. Every indexed metric must exist. Resolve and
   validate every nonzero shadow identifier and its shadow-record prefix
   according to §3.3.
6. Publish the handle only after all preceding checks pass.

A bitmap tail may be incomplete because the primary record itself is complete;
it follows the deterministic zero-tail rule in §3.6 when drawn. Row order 0 or
3 and a zero width or height do not by themselves prevent open: they produce
the query and draw results in §§3.9 and 3.11. Conversely, a bad pointer, a
record that crosses glyph-data end, a missing metric-table index, or an invalid
shadow reference prevents open even if no currently mapped character would
have used that glyph (project decision: eager validation keeps a valid-looking
lookup from becoming a later memory fault).

For path opens, the handle stores only the final path component as its display
file name, using the host path abstraction's separator rules; it never exposes
the containing directory. `pgf_open_w` applies the same rule to its wide path.
A memory open stores an empty file name. This basename-only,
empty-memory-open policy is a project decision: the ABI exposes a file-name
field, but guest memory has no source path and exposing a parent directory
would disclose host context. The name is bounded and NUL-padded for §3.8; it
does not affect parsing or lookup.

Every malformed-input, ceiling, unsupported-revision, and allocation failure
returns null from the corresponding open operation. Query and draw failures
return 0 as specified later. The present API has no reason-string accessor, so
the owning product boundary must name the failed semantic class rather than
report a generic crash: header, section bounds, packed width, revision,
allocation, glyph-record metric, shadow reference, guest-record, glyph
resolution, row order/composite, pixel format, and destination geometry are the
required classes; §7 tracks the missing per-open diagnostic accessor under
issue #349.

### 3.8 Font information

For a valid handle, `pgf_get_font_info` constructs a zero-filled 0x108-byte
`SceFontInfo` in host storage, fills every field below, and then writes the
whole record through one validated guest-memory operation. A null handle or
invalid destination performs no write [C1], [C8]. Multi-byte values are
little-endian, matching the PSP ABI [C5].

| Offset | Width | Result |
| --- | --- | --- |
| `0x00`–`0x27` | ten signed 32-bit words | Raw header values in this order: maximum horizontal size, maximum vertical size, ascender, descender, left X adjustment, base Y adjustment, minimum centre X adjustment, top Y adjustment, horizontal advance, vertical advance. |
| `0x28`–`0x4F` | ten IEEE-754 binary32 words | The same ten values divided by 64 and encoded with the required IEEE-754 rounding to binary32, in the same order. No decimal conversion or host-locale operation is permitted. |
| `0x50` | unsigned 16-bit | Header maximum glyph width. |
| `0x52` | unsigned 16-bit | Header maximum glyph height. |
| `0x54` | unsigned 32-bit | Header character-map entry count, as defined in §3.1 rather than a glyph count. |
| `0x58` | unsigned 32-bit | Header shadow-map entry count. |
| `0x5C`–`0x68` | four binary32 words | Header horizontal size, vertical size, horizontal resolution, and vertical resolution, each divided by 64, in that order. |
| `0x6C` | binary32 | Positive zero weight. |
| `0x70` | unsigned 16-bit | Font family 1, sans-serif. |
| `0x72` | unsigned 16-bit | Font style 1, regular. |
| `0x74` | unsigned 16-bit | Style subdivision 0. |
| `0x76` | unsigned 16-bit | Language 1, Japanese, when character U+3042 resolves in the direct map; otherwise language 2, Latin. |
| `0x78` | unsigned 16-bit | Region 0. |
| `0x7A` | unsigned 16-bit | Country 1. |
| `0x7C` | 64 bytes | Header font-name field copied byte-for-byte, including its NUL padding. |
| `0xBC` | 64 bytes | Final opened-path component, or 64 zero bytes for a memory open. |
| `0xFC` | unsigned 32-bit | Zero font attributes. |
| `0x100` | unsigned 32-bit | Zero expiry value. |
| `0x104` | unsigned 8-bit | Header byte `0x22`. |
| `0x105`–`0x107` | 3 bytes | Zero padding. |

Family 1, regular style 1, Japanese language 1, Latin language 2, region 0,
and country 1 are the public PSP SDK style values [C5]. Their constant use
here, the U+3042 language probe, zero weight, attributes, and expiry are
project decisions: the seam requires a complete deterministic style record,
while the PGF header has no authoritative fields for those choices. The probe
tests map resolution, not positive glyph area, so a present zero-area entry can
still select Japanese; non-Japanese and Korean classification is not claimed.

For a narrow-path open, the final component's host path bytes are copied
unchanged up to 63 bytes and then NUL-padded; a multibyte narrow path may
therefore be truncated at a byte boundary. For a wide-path open, each Unicode
scalar is encoded as UTF-8, an unpaired surrogate is replaced by U+FFFD, and
the result is truncated to at most 63 bytes only between complete scalar
encodings before NUL padding. These are project decisions: the guest ABI field
is a byte array, narrow host paths need not be UTF-8, and the conversion must
be finite and independent of the host locale. The host path abstraction defines
the final component and its native separators. This field never contains a
directory. It is informational and cannot change opening, lookup, or language
selection.

### 3.9 Character information and presence

A primary character resolves only through the direct character map in §3.2.
`pgf_has_char` returns 1 only when that map yields an existing glyph whose
width and height are both positive; it does not use glyph ID, area, shadow
state, or a substitute font as a substitute for presence. A null handle,
unmapped character, out-of-range map value, or zero width/height returns 0
[project decision: positive-area presence is the public seam's observable
meaning and has a synthetic regression in §8].

`pgf_get_char_info` first validates its complete destination span, writes 60
zero bytes to it, and then resolves a glyph. A valid direct-map entry wins and
the alternate code is ignored. If and only if the primary code is at least the
header's first-glyph value but has no usable direct entry, the reader tries the
alternate code through the same map. A below-first primary code does not fall
back; the rationale is that it has no character-map index at all, whereas an
in-range missing entry does [project decision]. A mapped zero-area glyph still
returns character information successfully even though `pgf_has_char` is 0.
A null handle, invalid destination, or exhausted lookup returns 0 and leaves
the destination zero when the destination itself was valid.

The resulting record is [C5]:

| Offset | Width | Result |
| --- | --- | --- |
| `0x00` | unsigned 32-bit | Glyph bitmap width. |
| `0x04` | unsigned 32-bit | Glyph bitmap height. |
| `0x08` | 32-bit two's-complement encoding | Signed horizontal bitmap adjustment from metric bits 28–34. |
| `0x0C` | 32-bit two's-complement encoding | Signed vertical bitmap adjustment from metric bits 35–41. |
| `0x10` | signed 32-bit | Width in 26.6 from the dimension group. |
| `0x14` | signed 32-bit | Height in 26.6 from the dimension group. |
| `0x18` | signed 32-bit | Baseline Y adjustment in 26.6, also used as horizontal-text ascender. |
| `0x1C` | signed 32-bit | Baseline Y adjustment minus 26.6 glyph height, the horizontal descender. |
| `0x20` | signed 32-bit | Left-aligned X adjustment in 26.6. |
| `0x24` | signed 32-bit | Baseline Y adjustment in 26.6, also used as horizontal-text bearing Y. |
| `0x28` | signed 32-bit | Centre-aligned X adjustment in 26.6, also used as vertical bearing X. |
| `0x2C` | signed 32-bit | Top-aligned Y adjustment in 26.6, also used as vertical bearing Y. |
| `0x30` | signed 32-bit | Horizontal advance in 26.6. |
| `0x34` | signed 32-bit | Vertical advance in 26.6. |
| `0x38` | unsigned 16-bit | Referenced shadow row-order bits, or 0 when no shadow. |
| `0x3A` | unsigned 16-bit | Raw one-based shadow identifier, or 0. |

Descender subtraction is checked. A primary record for which this derived
value is not representable as signed 32-bit causes open failure under §3.7 (project
decision: a wrapped font metric would be guest-visible corruption). The two
bitmap-adjustment fields use the same bit pattern as a signed value even though
the PSP SDK declaration names their storage unsigned; the format defines the
source fields as signed 7-bit quantities [C3], [C5].

`pgf_draw_glyph` uses the same direct-then-conditional-alternate resolution
as `pgf_get_char_info`. `pgf_draw_glyph_by_id` accepts only a `glyph_id` below
the inclusive glyph count; it never reads the character or shadow maps. Both
draw operations return 0 after a resolution failure and write nothing.

### 3.10 Kerning, advance, and placement

`SceFontGlyphImage` is a 0x18-byte guest record with this ABI layout [C5]:

| Offset | Width | Field |
| --- | --- | --- |
| `0x00` | 32-bit enum | Destination pixel format. |
| `0x04` | signed 32-bit | Horizontal position in 26.6 pixels. |
| `0x08` | signed 32-bit | Vertical position in 26.6 pixels. |
| `0x0C` | unsigned 16-bit | Destination width in pixels. |
| `0x0E` | unsigned 16-bit | Destination height in pixels. |
| `0x10` | unsigned 16-bit | Destination bytes per row. |
| `0x12` | unsigned 16-bit | Reserved; ignored. |
| `0x14` | unsigned 32-bit | Guest address of the first destination byte. |

The reader renders at the requested position but does not advance that position;
the consumer obtains the 26.6 horizontal and vertical advances from §3.9 and
owns cursor movement. PGF and this seam carry no pairwise-kerning table or
pair-adjustment input. Therefore “PGF kerning” means only the two per-glyph
advance values, and no pair adjustment is fabricated (named boundary: guest
text layout and any pairwise kerning remain with `libfont` integration, #299).

For a positive-area row-order-1 or row-order-2 glyph of *w* by *h* samples,
let *x* be mathematical floor division of `xPos64` by 64 and *fx* the
nonnegative remainder; define *y* and *fy* identically [project decision: the
following sub-pixel rule is the complete project behaviour and is pinned by
source-owned vectors in §8]. Let *S(ox,oy)* be source sample *ox,oy*, or zero
outside the source rectangle. The interpolated output footprint is *ow* by
*oh*, where *ow* is *w* plus one when *fx* is nonzero and *oh* is *h* plus one
when *fy* is nonzero. For every footprint coordinate *ox,oy*:

| Quantity | Mathematical value |
| --- | --- |
| upper blend | `fx × S(ox−1,oy−1) + (64−fx) × S(ox,oy−1)` |
| lower blend | `fx × S(ox−1,oy) + (64−fx) × S(ox,oy)` |
| interpolated sample | floor of `[upper × fy + lower × (64−fy)] ÷ 4096` |

The result is placed at integer pixel `x + ox`, `y + oy`. All intermediate
products use a checked or sufficiently wide signed host type. At zero
fraction the formula is exactly the original sample; at a nonzero fraction it
uses the one extra source row and column. Source coordinates are determined by
the decoded row order in §3.6. Negative `xPos64` or `yPos64` uses floor, not
truncation toward zero, so the fractional remainder is always 0–63.

The footprint can reach 128 by 128, still below §3.11's 65,536-sample ceiling.
The advance values do not scale, round, or otherwise alter this bitmap
placement; their fixed-point representation is queried independently.

### 3.11 Pixel output and dirty notification

A draw is all-or-nothing with respect to validation. Before the first pixel or
dirty-region side effect, the reader verifies the handle and glyph resolution,
the complete guest image record, a nonzero guest buffer address, nonzero width,
height, and row byte count, a known pixel format, positive source dimensions,
a source area no greater than 65,536 samples, row order 1 or 2, all checked
address arithmetic, every distinct writable destination byte, and a nonempty
intersection of the source footprint with the horizontally addressable buffer
range. Any failure returns 0, leaves the image record and buffer unchanged,
and sends no dirty notification [C1], [C2], [C8]. A valid request whose
vertical interval lies wholly outside the buffer returns 1 with no pixel and
no dirty-row effect; the asymmetric horizontal check is a project decision
that prevents an empty range from being treated as a successful raster. Except
for ABI field meanings, this section's preflight, raw-sample write, and dirty
rules are project decisions: they make hostile geometry fail before guest
memory changes and keep all platform implementations on one PSP-visible rule
[C2].

Let *bpl* be `bytesPerLine`. For pixel formats 0 and 1, the maximum
addressable horizontal coordinate supplied by one row is `2 × bpl`; for formats
2, 3, and 4 it is `bpl` divided by 2, 3, or 4 respectively. Integer division
floors; a zero result makes the horizontal intersection empty and fails the
request.
Pixel writes require both *y* in `[0, bufHeight)` and *x* in
`[0, min(bufWidth, addressableWidth))`. Negative coordinates are clipped, and
no address is formed until these checks pass. For formats 0 and 1, the byte
component is `floor(x ÷ 2)`; for formats 2, 3, and 4 it is respectively `x`,
`3 × x`, and `4 × x`. The complete byte address is `bufferPtr + y × bpl` plus
that component, with all arithmetic checked [project decision: these explicit
integer rules avoid host-layout, C-language evaluation-order, and overflow
dependence].

| Format | Addressable unit | Byte/nibble order and write |
| --- | --- | --- |
| 0: packed 4-bit | 2 pixels/byte | Even *x* uses the low nibble; odd *x* uses the high nibble. |
| 1: reversed packed 4-bit | 2 pixels/byte | Even *x* uses the high nibble; odd *x* uses the low nibble. |
| 2: 8-bit | 1 byte/pixel | One byte equal to the interpolated sample. |
| 3: 24-bit | 3 bytes/pixel | Three bytes all equal to the interpolated sample. |
| 4: 32-bit | 4 bytes/pixel | Four bytes all equal to the interpolated sample. |

Packed 4-bit writes are read-modify-write operations and preserve the partner
nibble, including on the first and last clipped pixel. The interpolated sample
is the unsigned 4-bit result from §3.10 and is not scaled to 8-bit or 32-bit
alpha. The 24-bit and 32-bit forms repeat that raw value in every component;
the latter is not an RGBA blend operation. The reserved record word at image
offset `0x12` has no effect.

After all in-range pixels are written, dirty notification occurs row by row
through the unchanged VRAM seam [C9]. The row interval is the intersection of
the source footprint with `[0, bufHeight)`. Let its integer destination base
be *bx*, and let *aw* be the addressable width derived from *bpl*. Within each
intersecting row, the dirty interval has exclusive endpoints
`max(0,bx)` and `min(aw,bx+ow)`; the early check guarantees it is nonempty.
This clips only to *aw*, not to `bufWidth`, intentionally permitting
notification of row padding that received no pixel. Let the addressable-unit
size in bytes be 1 for formats 0 and 1, and respectively 1, 3, and 4 for
formats 2–4. The reported row span starts at
`bufferPtr + y × bpl + floor(start ÷ unit)` and has
`(ceil(end ÷ unit) − floor(start ÷ unit)) × unit` bytes. A valid request with
no intersecting vertical rows sends no notification. No notification is sent
for a failed request.

Guest addresses throughout this section use 64-bit intermediate arithmetic and
must lie wholly in writable guest memory after 32-bit wrap is ruled out. The
reader must not use the dirty seam to probe or authorize an invalid write:
pixel-span validation happens first. This preserves the PSP-visible rule
that a successful call describes an already validated destination, not merely
an address that a later notification backend might reject.

## 4. Non-requirements and named boundaries

This document specifies a reader, not a font acquisition or text-layout
system. The product must use the following named boundaries verbatim enough to
make “in the works” and its tracker visible; none may be hidden behind a
generic font error or an apparently successful blank render.

| Named boundary | Required product meaning | Tracking |
| --- | --- | --- |
| Public PGF runtime reader | “PSP PGF reader support is in the works.” This document is a draft specification, not shipped capability. | #349 |
| Authentic user-owned fonts | “Authentic PSP typography needs a user-owned firmware PGF; none is bundled.” The reader never acquires or redistributes one. | #300 |
| Generated PGF fixtures | “Synthetic and generated-font coverage is in the works.” A generated fixture is evidence, not an authenticity claim. | #313 |
| Guest font integration | “Guest `libfont.prx` font loading and text layout are in the works.” | #299 |
| Pairwise kerning | The seam exposes per-glyph advances only; “pairwise kerning is in the works with guest text layout.” | #299 |
| Composite row-order-3 glyphs | “Composite PGF glyph rendering is in the works”; character metrics may be available while draw fails closed. | #349 |
| Revision and shadow-width extensions | “Additional PGF revision/map variants are in the works”; unsupported variants are rejected by name. | #349 |
| Shadow drawing | This seam reports shadow identity/row state but exposes no shadow-image draw call; “shadow-image drawing is in the works with guest font integration.” | #299 |
| Per-open reason reporting | The current open API returns only null; “named PGF rejection diagnostics are in the works.” §3.7 fixes the semantic classes meanwhile. | #349 |
| Metric hardware confirmation | Source-owned tests establish this contract; “physical-PSP PGF metric confirmation is in the works.” | #349 |

Outline conversion, hinting, Unicode shaping, bidirectional layout, font
matching/fallback policy, colour palettes, path security policy, and
redistribution decisions are outside this reader. Vertical advance and
adjustment fields are decoded because the ABI exposes them, but vertical
typesetting remains the consumer's responsibility. The reader never modifies
the input image, consults a title name, or chooses a different file after open.

## 5. Clean-room implementation constraints

- The implementation session receives only the §0 allow-list, this
  specification, and the public citations needed to interpret it. It must not
  read repository history, the excluded reader or internal header, any
  emulator's font implementation, a system PGF converter, or a firmware PGF.
- The spec session did read the excluded reader and internal header to observe
  which behaviours mattered. A public JPCSP format description was consulted
  only while testing one metric-record ambiguity; it is not cited, normative,
  or present in the implementation allow-list. A public search also surfaced
  a small PPSSPP source fragment while checking the same ambiguity; it was not
  opened as an implementation source, is not cited, and contributes no name,
  expression, decomposition, table ordering, or behavioural claim here. Its
  exact URL/commit identity is retained only in the maintainer-held admission
  provenance record, not exported to the implementation workspace. No
  expression, internal name, helper decomposition, or table ordering from any
  consulted implementation source is reproduced. This statement and the
  inventory required by campaign §5.2 must be attached to the implementation
  provenance record.
- Public format descriptions establish container facts; PSP SDK declarations
  establish ABI names, sizes, enum values, and error ownership. Project
  decisions in §§3–4 are normative for this reader and must remain visibly
  marked as such when rationale or tests are updated.
- Internal structure, field ordering in host records, helper names, bit-reader
  representation, allocation strategy, and error categorization are the
  implementer's own. Reusing the reference's loop nesting, table sequence, or
  renamed helper graph fails review even if a token score passes.
- The reader is portable PSP/runtime semantics over narrow host abstractions.
  Path input, file-size access, memory ownership, wide paths, and dirty
  notification may have Windows and POSIX implementations, but format and
  guest-visible rules do not branch by host OS. Linux warning-clean C99/C11
  compilation and Windows UCRT64 compilation are both required.
- No retail or generated font payload is an implementation input. Synthetic
  fixtures are constructed solely from §8 and remain source-owned test data.
  No upstream code or comments may be pasted, translated, or lightly renamed.

## 6. Similarity admission procedure

At admission, compare the complete new reader translation unit, any new public
declaration, and mechanically relevant build-file delta against the excluded
reader and its internal header at the recorded base. The comparison runs
outside the clean implementation workspace. Apply the campaign §5.1 candidate
threshold: normalized token-trigram overlap below 0.15 after exempting include
lines, the `pgf_api.h` spellings, public PSP ABI names/layout facts, and the
unchanged project memory/dirty seams; no identical run of six non-boilerplate
lines; and no unjustified identical ordering of more than eight constants or
field groups. The report must include the exemptions, tool/version, base
commit, and both numerator/denominator values. A score is review evidence, not
an authorship or classification decision.

The excluded reader may also be run as a black-box oracle on the same
synthetic fonts. Compare only observable outputs: open success, query records,
presence/fallback, decoded pixel buffers, return values, and dirty spans. It
must not be inspected to explain code or used in the clean workspace. Resolve
every difference from this specification and §9, including the intentional
ones: whole-record and metric-index validation at open; memory-open file name;
path-derived file name; atomic guest-range checks; and return 0 for each
unsupported draw argument or geometry named in §§3.9–3.11. Destination
pre-zeroing and reserved-byte differences are not evidence when the public ABI
already requires the complete record described here.

A difference cannot be admitted by saying the old implementation is “more
compatible.” Hardware evidence outranks both implementations; absent hardware
evidence, this spec plus its project decisions is normative and the test in
§8 defines the admitted result. Attach the similarity and differential reports,
source inventories, exact base/head commits, and test results to the provenance
record. Maintainer attestation remains human-only.

## 7. Provenance and open questions

Classification proposed at implementation admission:
`project-authored-independent`. Behaviour sources are this specification and
[C1]–[C11] by section. The spec session consulted the excluded derived reader
and its internal header; the implementation session must not. The spec session
also consulted the non-normative JPCSP description and incidental PPSSPP
fragment disclosed in §5. Their exact identities belong only in the
maintainer-held admission provenance record and are deliberately absent from
the implementation workspace. The implementation inventory, public citations,
similarity report, black-box
results, hardware-envelope IDs, and exact base/head commits are required record
fields. Evidence claimed by this draft is S for public container/ABI facts and
`project decision` for §§3–4; no PGF hardware-measured claim is made yet. The
maintainer alone supplies any provenance/legal attestation.

The deterministic documentation path is covered by the documentation
classification and public-source profile once the orchestrator records the new
path. The future implementation-bearing reader path still requires the normal
path-specific classifier/provenance process before merge; this specification
does not pre-empt that human decision.

Open questions are not permission to guess during implementation:

1. **O-1 — observed revisions and header sizes.** Which revisions and exact
   header sizes occur in user-owned firmware fonts (#300) and generated
   fixtures (#313)? Revisions above 3 remain named unsupported meanwhile.
2. **O-2 — revision-3 alternate maps.** Do real revision-3 fonts require the
   two opaque tables to interpret their direct character map, and what are
   their bits-per-entry values? The direct-map-only limit remains visible.
3. **O-3 — character-map count.** Does `charMapLength` always mean the direct
   map's entry count as PSP SDK states, or can it equal glyph cardinality in a
   firmware variant? A source-owned PSP probe should compare both counts.
4. **O-4 — metric flag positions.** Are bits 44–47 always role/three group
   flags, and is the advance index always 8 bits? A black-box/hardware probe
   must distinguish this reading from the excluded backend's shifted reading.
5. **O-5 — shadow-map width/padding.** Is 16 bits per shadow-map entry with
   exactly two bytes per entry universal, or do odd counts/non-16 widths use a
   different packed boundary? Unsupported values are rejected meanwhile.
6. **O-6 — invalid metric indices.** Must a missing table entry reject open,
   or may firmware render a zero/default metric? This spec requires open
   failure; §8 pins the decision until hardware evidence supersedes it.
7. **O-7 — file-name field.** Should a memory-open expose an empty file name,
   and should a path-open expose only its final component, as specified? Probe
   both through the public `sceFont` information route.
8. **O-8 — header `0x20`–`0x34`.** What are the exact reserved/alignment
   fields, and is byte `0x22` truly the source BPP? The reader exposes it
   without using it to decode.
9. **O-9 — first/last/map relationship.** Must
   `firstGlyph + charMapLength − 1 = lastGlyph`? This reader does not assume
   it and uses the map count as the direct lookup bound.
10. **O-10 — draw rejection surface.** Should null buffer, unknown format,
    zero dimensions/row width, and nonpositive glyph area all return failure
    before writes, and should the three byte-per-pixel formats repeat the raw
    4-bit value in every byte? Hardware/probe agreement is still required.
11. **O-11 — composite glyphs.** What is the exact overlay, positioning, and
    clipping order for a revision-3 row-order-3 record? Metrics remain queryable;
    draw stays closed under #349.
12. **O-12 — shadow selection.** Confirm the one-based shadow identifier,
    glyph-local shadow-record offset, and shadow row-order reporting against a
    source-owned PSP probe.
13. **O-13 — sub-pixel placement.** Confirm floor behaviour for negative
    26.6 positions and the one-extra-row/column bilinear rule for both row
    orders. Until then §3.10 is an explicit project decision, not hardware
    truth.
14. **O-14 — style/language.** Can the U+3042 probe misclassify an authentic
    font, and which region/country/weight values should a future hardware probe
    establish for Korean or Chinese fonts?
15. **O-15 — pointer cardinality.** Must `charPointerLength` always equal
    `lastGlyph − firstGlyph + 1`, or do firmware fonts use a larger/sparser
    table? This reader requires equality.
16. **O-16 — dirty extent.** Should notification include row bytes between
    `bufWidth` and the width implied by `bytesPerLine`, as specified, or only
    the declared image width? A homebrew VRAM-write probe can compare the
    host-visible dirty contract without retaining font bytes.
17. **O-17 — per-open diagnostics.** What ABI-compatible, non-breaking
    accessor should expose the semantic rejection classes in §3.7 without
    changing the null-returning open functions? Named diagnostics remain in
    the works under #349 until the maintainer and callers approve an extension.

Required future hardware evidence is homebrew-only under campaign §8: a
resident-runner probe may open user-supplied fonts and record ABI records,
fractional/negative placement, format writes, and dirty extents, but fonts,
traces, derived bytes, and paths remain outside the public tree. No physical
PSP action was taken for this specification.

## 8. Black-box test plan

The implementation session authors a deterministic synthetic-font builder and
black-box tests from this section. The builder is test code, not a converter:
it accepts structural test parameters, writes only deliberately tiny or sparse
synthetic images, and never reads a font. Every test constructs its expected
records from stated constants and the rules in §3, not from the excluded
reader. Temporary generated files are deleted by the test harness. A base
valid revision-2 fixture should contain three glyph records, a three-entry
identity character map, three 20-bit pointers, one entry in each metric table,
and no shadow; mutations then vary one fact at a time.

**Header and directory vectors.**

1. Revisions 0, 1, and 2 with a 392-byte header and revision 3 with a
   412-byte header reach later validation; revision 4, negative revision,
   negative version, nonzero header offset, wrong header size, and bad magic
   return null.
2. Images of 391 bytes and 16 MiB plus one byte return null. A minimal image
   at the lower bound must still fail later when its declared sections or
   glyph records cannot fit.
3. `firstGlyph > lastGlyph` fails. Counts above 1,048,576 fail before an
   allocation based on the count. Equality between pointer count and
   `last − first + 1` is both accepted and, when violated, rejected.
4. Packed widths 1 and 32 are accepted where a section can fit; 0 and 33 fail
   for the character and pointer maps. Shadow count zero accepts width zero;
   nonzero shadow count accepts only width 16.
5. Place shadow-map counts 1, 2, and 3 before known character-map bytes. Odd
   counts consume two trailing padding bytes so the following section begins
   on the next 4-byte boundary; padding can have any value and is not an entry.
6. For every declared section, remove one byte at a time from the end and
   assert null, including each revision-3 table independently. Checked-size
   tests also use counts that would overflow a 32-bit product.
7. A revision-3 fixture with both opaque table counts and deliberately
   non-identical table bytes proves they are skipped and do not alter the
   direct map; either declared table truncated causes open failure.

**Packed map and pointer vectors.**

1. Use a map width that forces entries across byte boundaries. Values
   0, 1, and the maximum non-sentinel value must extract in low-bit order;
   an all-ones entry is absent. This catches both bit-order and big-endian
   extraction regressions.
2. Probe indices immediately below first, at first, at map-count minus one,
   at map count, and above. Assert absent, present, present, absent, absent.
   `lastGlyph` is not substituted for map count.
3. Map values equal to and above glyph count are lookup misses, never aliases.
4. Pointer width 20 with values 0, 1, and a large checked value proves
   byte-offset multiplication by 4. Zero is a valid record start, duplicate
   pointers alias valid records, and an all-ones/out-of-range pointer causes
   open failure when no complete record fits there.

**Metric-record vectors.**

1. Build one record for each combination of the three index flags. All indexed
   is a 12-byte record; one, two, and three inline groups are respectively
   19, 26, and 33 bytes. Each following byte begins the bitmap, proving the
   variable length.
2. Put known pairs in the dimension, X-adjustment, Y-adjustment, and advance
   tables. An index equal to each table count causes open failure; one below succeeds.
3. Sweep the 7-bit horizontal and vertical adjustments through −64, −1, 0, 1,
   and 63 and compare the exact signed results. Sweep width and height through
   0, 1, 126, and 127.
4. Build row orders 0, 1, 2, and 3. Character information succeeds for all
   mapped records, while drawing succeeds only for 1 and 2.
5. Toggle bit 44 while keeping bits 45–47 and payload groups fixed. Under this
   specification the decoded primary layout and query result are identical,
   pinning O-4's project decision.
6. Truncate a record at every byte of its fixed prefix and every variable
   group. Each mutation causes open failure before bitmap bytes are consulted.
7. Set the 7 unassigned bits and the full 9-bit shadow field to boundary
   patterns and assert that only the shadow identifier is observable.

**Shadow vectors.**

1. Give the shadow map two character codes. Primary records with identifier 0,
   1, and 2 must report no shadow, the first, and the second respectively;
   identifier 3 causes open failure.
2. Make an in-range shadow identifier select an absent direct-map code and
   separately select a code whose glyph is out of range. Both cause open failure.
3. Point valid and invalid primary records at a complete six-byte shadow
   prefix. Zero, a target before the primary record's end, a wrapped target,
   and a target with fewer than six bytes fail; a valid target succeeds.
4. Give valid and invalid shadow records row orders 1, 2, 3, and 0. The
   identifier and row bits in `SceFontCharInfo` follow §3.9 even though no
   shadow draw call exists.

**RLE and row-order vectors.**

1. Encode a 4-by-3 row-order-1 glyph with control/sample nibbles
   `1,1,1,2,0,3,2,0,2,F,0,5`. Its six synthetic bytes are
   `0x11,0x21,0x30,0x02,0xF2,0x50`, and its samples are
   `1,1,2,2,3,3,3,3,3,3,3,3`; the trailing encoded nibbles are not consumed.
   A column-order twin proves the same samples map
   to transposed coordinates.
2. Cover control 0, 7, 8, and 15. Control 0 repeats eight times, control 7
   repeats once, control 8 emits eight literals, and control 15 emits one.
3. A final run that has a control but lacks its required sample nibble fills
   only the remaining output with zero and performs no read beyond the image.
   Adding the missing sample completes the run. A suffix of one through eight
   unusable bits is ignored.
4. Make a run request more literals or repeats than remain in the output and
   assert it is truncated to the footprint without consuming another run.
5. Encode zero-area, 1-by-1, 127-by-1, 1-by-127, and 127-by-127 records. The
   last has 16,129 samples and remains below the defensive 65,536 ceiling.
6. A row-order-3 record starts with three known 16-bit character codes.
   Character query returns 1; both draw APIs return 0 and leave the buffer
   unchanged.

**Font-information vectors.**

1. Compare all 0x108 bytes for a known header, including zeros in every
   reserved field and bitwise-correct binary32 values for all fourteen
   divided metrics. Use values requiring normal binary32 rounding as well as
   exact powers-of-two cases.
2. Build maps with and without U+3042 and assert Japanese versus Latin.
   Presence of an out-of-range map value does not select Japanese.
3. Open a wide path with directory components and a multibyte final component;
   assert only a code-point-bounded 63-byte UTF-8 basename plus NUL padding.
   Open a narrow path whose basename exceeds 63 bytes and assert raw-byte
   truncation. Open memory and assert 64 zero bytes. Separately assert the full
   directory never appears in the guest record.
4. Null handles are no-ops. Guard-filled guest spans crossed by the end of a
   0x108-byte destination remain unchanged.

**Presence, fallback, and character-information vectors.**

1. Identity-map all three base characters. Assert `pgf_has_char` and
   `pgf_get_char_info` return 1 and compare all 0x3C output bytes.
2. Make primary mapping absent at a code below first with a valid alternate;
   assert no fallback. At first and inside map count, assert the alternate is
   used. Make the primary present and the alternate different; assert primary
   wins.
3. Exercise sentinel and map value at/above glyph count as misses. Direct
   glyph-ID draw succeeds for IDs 0 and glyph-count minus one independently of
   the character map; the bit pattern for −1, the maximum unsigned value,
   glyph count, and larger values fail.
4. Map a zero-width or zero-height glyph. Presence is 0, character info is 1
   with exact zeros, and draw is 0. This pins the documented asymmetry.
5. Compare every metric and shadow field, including negative bitmap adjustment
   bit patterns and the checked descender subtraction. A construction that
   would overflow descender causes open failure.
6. A null handle with a valid destination writes 60 zeros and returns 0. An
   unmapped character does the same. An invalid destination returns 0 with no
   partial write.

**Placement and pixel-format vectors.**

1. At `fx = fy = 0`, every 4-by-4 source sample reaches the same integer
   destination sample for both row orders.
2. In 8-bit format, place a horizontal source `[10,5,0,0]` at `fx = 32` and
   `fy = 0`; the five destination samples are `[5,7,2,0,0]`, including the
   added left and right footprint samples. Repeat with `fy = 32`, both
   fractions, row order 2, and
   a negative coordinate whose floor and remainder are checked explicitly.
3. For format 0, prefill each destination byte with distinguishable partner
   nibbles and overwrite even then odd pixels. Assert low/high order and
   partner preservation. Repeat reversed for format 1.
4. In formats 2, 3, and 4, assert one, three, and four identical bytes equal
   to the raw interpolated 4-bit sample. Pixel-format value 5 fails.
5. Cover negative x/y, exact first/last edges, full horizontal clipping, and
   a valid footprint wholly above/below the buffer. The latter returns 1 with
   no write or dirty call. An empty horizontal intersection returns 0.
6. Null font/buffer, zero width/height/row bytes, unmapped glyph, row order
   0/3, and zero-area source all return 0 with the destination and its guard
   pages unchanged. Include destination spans that cross a guest allocation
   boundary to prove preflight write atomicity.

**Dirty-notification vectors.**

1. Use a recording dirty seam. For every supported format and row order, assert
   one exact whole-byte span for each vertically intersecting row and no call
   for other rows.
2. Make `bytesPerLine` describe more pixels than `bufWidth`. Pixel comparison
   must show no write in that row padding, while the recorded dirty span must
   include the full addressable footprint, pinning O-16.
3. Half-pixel clipping in packed formats must round the dirty interval outward
   to whole bytes. Early-failure, empty-horizontal, and wholly-outside-vertical
   cases record zero calls.

**Lifetime, hostile input, and build gates.**

1. Mutate an owned memory image after `pgf_open_memory`; query and draw must
   be unchanged. Repeat after the caller releases its original storage.
2. `pgf_close(NULL)` is a no-op. Open/close thousands of mixed valid and
   invalid handles under allocation-failure injection leaks neither handle
   state nor owned bytes.
3. Run at least 10,000 deterministic mutations of the base fixture under a
   wall-clock bound. Every case must return through the API without crash,
   hang, out-of-bounds read/write, unsigned wrap, double free, or partial guest
   write. Repeat under available address/undefined-behaviour sanitizers.
4. Build the reader and tests with Linux GCC in C99 and C11 modes under
   `-Wall -Wextra -Werror`, and with UCRT64 GCC under the repository's warning
   policy. The test binary must not link a Windows-only library on Linux.
5. Every valid synthetic fixture must also pass the public structural
   `validate_pgf_data` check in `tools/nk_core/fonts.py` [C7]. A mismatch is a
   fixture or spec defect, not permission to weaken the reader.
6. Run the repository's focused docs, public-link, and pre-commit gates for
   the final spec/implementation diff. No test uses a retail font or title
   input.

Before the reader implementation exists, its production-path test target must
fail to link or must fail its first behavioural assertion for the intended
missing-reader reason. After implementation, the same unmodified vectors must
pass. The documentation change's independent failing-before check is the
`tools.test_lint_docs` index-completeness test: adding the new spec without
its `docs/README.md` index/status entries fails, and adding those entries makes
it pass.

## 9. Citation catalog

- [C1] `src/rt/pgf_api.h` — the unchanged public reader seam, return-bearing
  operations, opaque handle, and guest-address argument types.
- [C2] `AGENTS.md` §§3–6 and 9 — fail-closed malformed-input policy, private
  boundary, correctness evidence, and documentation gate routing.
- [C3] BenHur, *PGF Binary Format*, archived 24 November 2007 —
  <http://web.archive.org/web/20071124080708/http://www.psp-programming.com/benhur/pgf_binary_format_doc_0.9934.htm>
  (header offsets and widths, section order and sizes, packed character and
  pointer tables, shadow map, revision-3 tables, glyph metric fields and
  optional groups, pointer-to-byte rule, 4-bit RLE, and composite marker).
- [C4] yetiPSP, *Chapter 26: Font System*, §26.9 —
  <https://hitmen.c02.at/files/yapspd/psp_doc/chap26.html> (low-nibble-first
  bit/nibble order, 4-bit RLE controls, and horizontal/vertical row order).
- [C5] PSP2SDK `include/psp2/pgf.h` generated source —
  <https://psp2sdk.github.io/pgf_8h_source.html> (`SceFontInfo`,
  `SceFontStyle`, `SceFontCharInfo`, `SceFontGlyphImage`, `SceFontImageRect`,
  family/style/language/pixel-format enum values, and public error ownership).
  The rendered `SceFontInfo`, `SceFontStyle`, `SceFontCharInfo`, and
  `SceFontGlyphImage` reference pages under the same site were cross-checked.
- [C6] `docs/provenance/FONT_ORIGINS.md` — project-owned font provenance,
  converter/payload boundary, metric-history cautions, and the open Korean
  composite-glyph question. It is not authority for PGF byte layout.
- [C7] `tools/nk_core/fonts.py` — the public structural PGF validator used as
  an independent fixture sanity gate.
- [C8] `src/rt/recomp.h` — the unchanged guest-memory span/accessor contract
  used to read and write ABI records without unchecked host pointers.
- [C9] `src/rt/ge_shared.h` — the unchanged VRAM dirty-region seam used after
  validated pixel writes.
- [C10] `docs/INDEPENDENCE_CAMPAIGN.md` §§4–5 and 8 — G4 disposition,
  spec/implementation session separation, citation and similarity protocol,
  and homebrew-only hardware-probe boundary.
- [C11] XentaxWiki, *PGF Font* —
  <https://wiki.xentax.spektr.name/index.php/PGF_Font> (independent
  corroboration of header offset, magic, version fields, and map/pointer width
  locations).

## Sources read

Every source consulted by either spec session is listed here. “Reference only”
means the item informed behavioural scoping but is not a citable authority for
the implementation; the implementation session receives neither it nor the
excluded files.

- `AGENTS.md`, especially §§3–6 and 9.
- `docs/INDEPENDENCE_CAMPAIGN.md`, especially §§4–5 and 8.
- `docs/cleanroom/SCHED_SPEC.md` and `docs/cleanroom/PRX_LOADER_SPEC.md` for
  document structure, citation discipline, and admission tone.
- `docs/README.md` and `.markdownlint-cli2.jsonc` for index and Markdown
  requirements.
- `docs/provenance/FONT_ORIGINS.md`, especially the metric, converter, payload,
  and open-question sections.
- `src/rt/pgf_api.h` and `src/rt/pgf_unavailable.c` for the public seam and
  current public-build refusal.
- `src/rt/hle.c` font call sites, `src/rt/recomp.h` guest-memory helpers,
  `src/rt/ge_shared.h` dirty-region interface, and the relevant `Makefile`
  backend-selection lines.
- `tools/nk_core/fonts.py`, specifically its public PGF structural validator.
- Excluded reference-only `src/rt/pgf.c` and its internal `pgf.h`, in the
  maintainer-held history. They were read by the spec session as permitted
  behaviour references and are neither cited nor implementation inputs.
- The archived BenHur format document, yetiPSP chapter 26, XentaxWiki PGF
  page, and PSP2SDK PGF header/reference pages listed in §9.
- An archived PSP Developer Wiki PGF overview and the PSP Developer Notes font
  page were consulted only as terminology/layout cross-checks; no fact unique
  to either page is normative here.
- JPCSP's public PGF format description and a PPSSPP source fragment surfaced
  during ambiguity checking are reference-only as disclosed in §5; neither is
  cited and neither enters the implementation allow-list.
- GitHub issue #349, including its comments, for live campaign scope and
  acceptance criteria.
- The campaign working notes for lane S-pgf, the prior spec session's research
  record. They guided source reuse but are not authority over live sources or
  this completed specification.
