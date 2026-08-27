# Direct XB archive prototype (historical tracker item #196)

> **Tracker numbering.** Bare `#N` references in this document are
> **pre-republication tracker numbers**. GitHub numbers issues and pull requests from one
> sequence, and the sanitized public repository restarted that sequence, so a number here
> may now resolve to an unrelated live public object. Read every bare number below as a
> historical identifier unless it is written as an explicit link.

This document records the source audit and the bounded probe added on
`codex/issue196-direct-xb`. The probe is investigation tooling only. It does
not change `host_data_lookup()`, the HLE filesystem, or archive selection in the
runtime.

## Upstream identity and provenance

The audit was performed on 2026-08-03 against the public upstream surfaces:

- The latest non-yanked PyPI artifact is **libxb 0.2.0**, uploaded 2025-08-12;
  PyPI classifies the project as Beta rather than Stable. PyPI also lists 1.0.0,
  but both of its files are yanked. The package declares Python >=3.10 and the
  MIT license, so 0.2.0 is the latest reproducible release identity, not a
  correctness oracle.
- The source distribution is
  [`libxb-0.2.0.tar.gz`](https://files.pythonhosted.org/packages/45/9d/0cdfb091f722e15a2b5506d8d9d26bb9af894e5e36e981baecca48c32fd9/libxb-0.2.0.tar.gz),
  size 20,582 bytes, SHA-256
  `2d0cacd9363358cfaaec31b6db5eb5c25ea410b77dfac7d01255e7305c546371`.
  The wheel is 20,430 bytes with SHA-256
  `90414b4b3205e199b86d5f736342b84c886de68a31e91f95b10e89438b4e4524`.
  PyPI reports no signature and no Trusted Publishing attestation for these
  files.
- GitHub has no release or tag for libxb. Its only branch at audit time was
  `main`, commit
  [`ce6df78e5ca99241dd2bbbd68ca485e34003d760`](https://github.com/kiwi515/libxb/commit/ce6df78e5ca99241dd2bbbd68ca485e34003d760),
  dated 2025-08-12. The checked-out source files and the 0.2.0 sdist source
  matched; the sdist additionally contains packaging test material. This is a
  source snapshot identity, not a signed release provenance claim.
- The upstream [`LICENSE`](https://github.com/kiwi515/libxb/blob/main/LICENSE)
  is MIT, copyright 2025 kiwi515. Nothing from that implementation is vendored
  by this prototype. If libxb is ever redistributed, retain that notice and
  pin the exact artifact hash rather than cloning moving `main`.
- The prototype is an independent, AI-assisted reimplementation of the
  bounded read path, not a copied libxb derivative. Its algorithmic reference
  is the public source at the commit above; that lineage is recorded here so
  the GPL-licensed project code and the MIT-licensed reference remain distinct.

## What libxb actually provides

`XBArchive` reads the `xe\0\x01` signature, a 32-bit file count, an 8-byte FST
row per file, a Shift-JIS string table, and file data. The FST stores the
compression tag in the high nibble and an offset divided by four in the low 28
bits. The four tags are DEFLATE (LZS + Huffman), HUFFMAN, LZS, and NONE. The
reader accepts an explicitly supplied little- or big-endian mode and loads the
entire archive into memory. See the upstream
[`implement.py`](https://github.com/kiwi515/libxb/blob/main/src/libxb/archives/implement.py)
and [`common.py`](https://github.com/kiwi515/libxb/blob/main/src/libxb/archives/common.py).

`MNTPArchive` is only a convenience subclass. It maps mode strings to
`XBOpenMode` and always passes `XBEndian.LITTLE` to `XBArchive`; it does not
implement archive mounting, locale selection, slot namespaces, duplicate
precedence, or PSP I/O semantics. The published presets are `MNG3`, `MNGO`,
`MNG4`, `MNGP`, `MNG5`, `MNT`, and `MNTP`; they are all the same `XBArchive`
reader with a mode/endian preset, and `MNG5` is the only listed big-endian
preset. The package documentation explicitly marks Minna no Golf 6 as not yet
supported. See the upstream
[`presets.py`](https://github.com/kiwi515/libxb/blob/main/src/libxb/archives/presets.py).

Important reference behavior found during the audit:

- path separators are changed to backslashes when creating an `XBFile`, but
  extraction only removes the literal substring `..\\`; it is not a general
  containment proof;
- duplicate names are retained as a list and are not rejected or assigned a
  deterministic lookup winner;
- FST offsets are checked only for alignment and being below the archive length;
  expanded sizes, compressed spans, overlaps, and all table arithmetic are not
  preflighted as a whole;
- the stream primitive truncates a short read rather than turning every short
  primitive into a structured archive error, and the reference reader reads
  compressed data from the shared archive stream without an entry-span bound;
- the package documents title presets, not `.xb`/`.xb0`/`.xb2`/`.xb3` locale or
  mount semantics. Those semantics belong to the game/runtime investigation.

## Variants and locale evidence

`tools/extract_xb.py` currently discovers `.xb`, `.xb0`, `.xb2`, and `.xb3` and
keeps their extraction directories distinct. The runtime's existing extracted
tree logic documents `data_00_USE`, `data_02_FRE`, and `data_03_SPA` as the
observed localized roots and selects the corresponding numeric archive variant.
There is no repository evidence that `.xb1` is shipped for the supported title;
the probe still labels any explicit `.xbN` filename so an unknown numeric
variant cannot silently collapse into the base archive. Locale hints are
metadata only, not a selection policy.

## Prototype capability

[`tools/xb_probe.py`](../tools/xb_probe.py) is a source-owned, read-only parser:

```powershell
python tools/xb_probe.py <private-file.xb> --lookup data/menu/text/CommonText_Acce.to
python tools/xb_probe.py <private-file.xb0> --json --limit 200
```

The default output is bounded metadata: archive variant, endian, data-section
start, entry count, normalized inner key, offset, expanded size, compression,
stored span, and whether the listing was truncated. It never prints entry bytes.
The Python API's `read_entry()` is the explicit opt-in byte-read seam for a
caller that already has an exact key.

The parser accepts both byte orders and all four XB compression tags. It applies
limits before allocation/decompression (archive 512 MiB, 100,000 entries,
16 MiB string table, 256 MiB entry, 512 MiB total expanded data by default;
all are configurable). It validates:

- signature, count arithmetic, section alignment, table cardinality, strict
  Shift-JIS decoding, path hash, and string-table truncation;
- separator normalization without case folding, absolute paths, drive paths,
  empty/`.`/`..` components, NULs, and control characters;
- duplicate canonical inner keys, duplicate FST offsets, and non-increasing
  data spans;
- data-section containment, four-byte alignment, non-overlapping spans, raw
  size, compressed headers, compressed payload spans, expanded-size agreement,
  bounded LZS/Huffman/DEFLATE decoding, and zero-only alignment tails;
- explicit variant labels without pretending that a filename proves a mounted
  slot or locale.

The test fixture suite is entirely synthetic and contains no retail names,
paths, bytes, inventories, or hashes. It covers little/big endian, base and
numeric variants, exact separator-normalized lookup, all four compression tags,
compressed string tables, compression-header raw fallback, traversal/absolute
paths, duplicate keys, truncation, bad counts, offsets, sizes, tags, and the
metadata-only CLI path.

## Relationship to #139 and #196

This primitive makes the next experiment possible: an operator can prove that
an exact inner key exists in a supplied archive and inspect its bounded metadata
without flattening the whole title or publishing private content.

Issue #196 has valid independent goals (direct archive inspection and optional future VFS backing), but the historical #139 mounted-slot hypothesis is superseded by newer evidence:

- `face/00` is a character-id formatted directory component;
- `100_f_face*` originates as a placeholder in shared `menu_motion/99` data;
- the scorecard uniquely leaves that placeholder rename disabled;
- no source archive exports the mixed `face/00/100_f_face*` key;
- flattening is not responsible for the #139 face-path mismatch;
- `MNTPArchive` is not hidden runtime mount semantics, but a title-oriented/little-endian convenience preset;
- remaining #139 work is portrait model loading/rendering behavior (including investigation around the guard near `0x002a83ec`).

PR #247 does not resolve #139, nor does it close #196. It provides source-owned XB archive inspection tooling that can serve as infrastructure for future direct-VFS experiments.

## Recommended production design (not implemented here)

1. Keep the parser as a pure read-only archive object with explicit source
   identity, endian, variant, and limits. Keep private archive caches keyed to
   the source ISO identity and Git-ignored.
2. Add a separate runtime `XBMount`/slot layer only after traces show the
   guest's mount, select, and unmount operations. A mount object should own an
   archive handle and resolve a slot-relative key; locale precedence should be
   an explicit table, not a filename heuristic.
3. Route `sceIoOpen`/`Getstat` through that mount layer only for a proven
   archive-backed request. Preserve the current extracted-tree path as a
   separately measured fallback during an A/B run; never globally flatten
   archives or rewrite `00` to a filename prefix.
4. Require production-dispatch tests for mount lifetime, duplicate/precedence
   behavior, failed unmounts, and exact guest-visible errors before removing
   `xbdata_extracted/` from setup. The prototype's synthetic tests are
   production-helper/white-box evidence, not PSP-runtime or framebuffer proof.
