# Notices, provenance, and disclosures

The project is GPL-3.0-or-later; the full text is in [LICENSE](LICENSE).
Individual files keep their own SPDX identifiers and upstream terms. This file
lists where code came from and the notices that go with it. It is not legal advice.

## Legal and contact

Nakagawa Recomp is an independent project. It is not affiliated with or endorsed by
Sony Interactive Entertainment, Clap Hanz, or any game publisher. PlayStation and
PSP are trademarks of Sony Interactive Entertainment Inc., and game titles are
trademarks of their owners; they are named only to say what the project works
with. The repository contains no game files, keys, firmware or system fonts, and
Nakagawa does not decrypt games: users must supply any external inputs from their
own copy.

Rights holders with a concern about anything in this repository can contact the
maintainer privately through
[GitHub private reporting](https://github.com/Jstar269/nakagawa-recomp/security/advisories/new)
or the contact listed on the [maintainer's GitHub profile](https://github.com/Jstar269).
We aim to respond promptly and will remove material where appropriate.

## Upstream families

- **sal063 PSP Recompilation Project** — GPL-2.0-or-later, public revision
  `da17b0e1db209206a407d097d132201e516e3855`. Nakagawa began from this toolkit
  and retains substantial modified runtime and pipeline code. See the
  path-hashed [public provenance ledger](assets/public_provenance_ledger.json).
- **PPSSPP** — Copyright (c) 2012- PPSSPP Project and contributors,
  GPL-2.0-or-later, with its own third-party notices. Several HLE, GE, VFPU and
  recompiler structures are translated or adapted through the sal063 lineage;
  the files that say so in their headers are `src/rt/hle.c`, `hle_power.c/.h`,
  `ge.c`, `mpeg.c`, `recomp.c/.h`, `savedata.c`, `vfpu_interp.c` and `evf.h`. PPSSPP-origin data is retained only where the profile records
  its exact path, revision and notice.
- **PSPSDK / PSPDEV** — permissive per-component declarations and BSD-family
  notices for public ABI definitions and source-owned fixtures. The applicable
  notices are in `THIRD_PARTY_LICENSES/PSPSDK.txt` and `assets/upstream/`.
- **FFmpeg n4.4 ATRAC3 and ATRAC3+ subsets** — LGPL-2.1-or-later. The imported
  decoders and their license/provenance file are under `src/rt/atrac3p/`; the project wrapper
  remains separately identified in the ledger. Two byte-identical imports,
  `libavcodec/fft_init_table.c` and `libavcodec/fft_table.h`, also carry a MIPS
  Technologies, Inc. BSD-3-Clause notice. Its verbatim text is in
  `THIRD_PARTY_LICENSES/FFMPEG_MIPS_FFT.txt` and is included in the generated
  native-package third-party notices bundle alongside the FFmpeg LGPL text.
- **Clay v0.14 layout library** — `Zlib` (the upstream `LICENSE.md` calls it
  the "zlib/libpng license"). The header (upstream plus one documented LOCAL DELTA),
  complete license, pinned source manifest, and SHA-256 values are recorded under
  `third_party/clay/`.
  Clay is compiled into the native layout selftest and the `nk_ui` core library
  tests only; it is not linked into the player yet.
- **Optional system FFmpeg/libavcodec H.264 backend** — `H264_FFMPEG=1` links
  host-provided `libavcodec` and `libavutil` dynamically. Their source is not
  vendored and the Windows package does not bundle them; anyone who bundles them
  must keep the notices that build requires ([FFmpeg licensing](https://www.ffmpeg.org/legal.html);
  build notes in `docs/PLATFORM_PORTABILITY.md`).
- **SDL3** — zlib; used by the optional host renderer/audio integration. The
  first public profile excludes the sal063-derived `src/rt/audio.c` backend and
  links `src/rt/audio_unavailable.c` instead.
- **Vulkan** — Apache-2.0 loader/header ecosystem; this repository does not
  redistribute a Vulkan SDK or loader binary.
- **VFPU tables** — PPSSPP-origin lookup data with the exact source and checksums
  recorded in `assets/vfpu/PROVENANCE.json`; inclusion is a provenance decision,
  not a claim about PSP firmware ownership.
- **Gudea font fixture (issue #313)** — SIL Open Font License 1.1,
  Copyright (c) 2011, Agustina Mingote, with Reserved Font Name "Gudea". The
  exact upstream bytes, the licence text, and the `font-pin/v1` provenance
  record are pinned in `fixtures/fonts/gudea/`; they are input to the
  deterministic converter `tools/ttf2pgf.py` for test fixtures only. The font
  is not packaged, generated PGFs stay in temporary or `build/` paths, and the
  output name is project-owned so the Reserved Font Name is never claimed.
- **Gowun Dodum and M+ 1p font fixtures** — SIL Open Font License 1.1,
  Copyright 2021 The Gowun Dodum Project Authors and Copyright 2016 The M+
  Project Authors. Pinned with their licence texts in `fixtures/fonts/gowundodum/`
  and `fixtures/fonts/mplus1p/`, for test fixtures only, on the same terms as Gudea.

## Generated third-party notices

Release notices are generated, never hand-written. One machine-readable source
of truth, `assets/third_party_components.json` (schema:
`assets/third_party_components.schema.json`), records every component that can
enter a release artifact: its SPDX identifier, its disposition, the packaging
route that copies it, and the license texts copied verbatim into
`third_party/licenses/` and `THIRD_PARTY_LICENSES/`. The native package, the
SBOM, and the release gate all read that one file.

- The native package route (`tools/package_notices.py`, invoked by the package
  build and by `tools/copy_build_assets.ps1`) emits `THIRD_PARTY_NOTICES.txt`,
  `THIRD_PARTY_NOTICES/index.json`, `SOURCE.txt`, and `RELINK.md` beside the
  package, and fails closed when a bundled binary has no license record or no
  license text.
- `tools/generate_sbom.py` lists the shipped DLLs and the recorded Python tool
  licenses from the same file, so the SBOM cannot drift from the notices.
- `tools/test_third_party_notices.py` is the gate: it fails when a DLL a
  packaging route can copy, or any component in the source of truth, lacks a
  license record or a license text.

## Staged host runtime libraries (#421)

The Windows player and every built package stage `SDL3.dll`, `SDL3_ttf.dll`,
and the mechanically resolved SDL3_ttf dependency closure beside the
executable (`tools/stage_runtime_dlls.py`, PE import-table walk stopping at
Windows system DLLs). No font file is bundled: the UI opens a system UI font
already present on the user's machine. Every staged DLL ships its upstream
license text, copied verbatim from the MSYS2 UCRT64 toolchain packages
(`ucrt64/share/licenses/`, read-only) and recorded with its SHA-256 in
`assets/third_party_components.json`:

| DLL | License | License text |
| --- | --- | --- |
| `SDL3.dll` | Zlib | `third_party/licenses/sdl3/LICENSE.txt` |
| `SDL3_ttf.dll` | Zlib | `THIRD_PARTY_LICENSES/SDL3_ttf.txt` |
| `libfreetype-6.dll` | FTL | `THIRD_PARTY_LICENSES/FreeType-FTL.txt` |
| `libharfbuzz-0.dll` | MIT | `THIRD_PARTY_LICENSES/HarfBuzz.txt` |
| `libgraphite2.dll` | MIT (chosen) | `THIRD_PARTY_LICENSES/Graphite2.txt` |
| `libpng16-16.dll` | libpng-2.0 | `THIRD_PARTY_LICENSES/libpng.txt` |
| `zlib1.dll` | Zlib | `THIRD_PARTY_LICENSES/zlib.txt` |
| `libbz2-1.dll` | bzip2-1.0.6 | `THIRD_PARTY_LICENSES/bzip2.txt` |
| `libbrotlidec.dll`, `libbrotlicommon.dll` | MIT | `THIRD_PARTY_LICENSES/Brotli.txt` |
| `libglib-2.0-0.dll` | LGPL-2.1-or-later | `THIRD_PARTY_LICENSES/GLib.txt` |
| `libintl-8.dll` | LGPL-2.1-or-later | `THIRD_PARTY_LICENSES/libintl.txt` |
| `libiconv-2.dll` | LGPL-2.1-or-later | `THIRD_PARTY_LICENSES/libiconv.txt` |
| `libpcre2-8-0.dll` | BSD-3-Clause WITH PCRE2-exception | `THIRD_PARTY_LICENSES/PCRE2.txt` |
| `libgcc_s_seh-1.dll`, `libstdc++-6.dll` | GPL-3.0-or-later WITH GCC-exception-3.1 | `third_party/licenses/gcc-libs/COPYING3.txt`, `third_party/licenses/gcc-libs/COPYING.RUNTIME.txt` |
| `libwinpthread-1.dll` | MIT AND BSD-3-Clause | `third_party/licenses/winpthreads/COPYING.txt` |

License choices and combination notes:

- **FreeType** is dual-licensed FTL or GPL-2.0-or-later; this distribution uses
  the **FTL** option, which the FSF lists as GPL-compatible.
- **Graphite2** is granted under four licenses (LGPL-2.1+, MPL-2.0, GPL-2.0+,
  or MIT) by its upstream `LICENSE`; this distribution uses the **MIT** option.
- **GLib, libintl and libiconv** are LGPL-2.1-or-later and are dynamically
  linked as separate replaceable DLLs. LGPL-2.1 section 3 permits use under
  GPL-3 terms, so they combine with this GPL-3 distribution.
- The **MinGW runtime** licenses (`gcc-libs`, `winpthreads`) were already
  covered by the existing records; no new text was needed for them.

The FFmpeg ATRAC3 and ATRAC3+ subsets (LGPL-2.1-or-later) are statically linked. The whole
program is distributed under the GNU GPL version 3 (the project's own files are
GPL-3.0-or-later) with its complete corresponding source, and `SOURCE.txt` names the exact repository commit or tag of each
package, which is what lets a user modify and relink it (issue #421).

## Public-safe exclusions

The public source profile excludes the legacy PGF parser, all firmware font data
and any PGD implementation; the project-authored PGF reader `src/rt/pgf_public.c`
is public and reads only fonts the user supplies. It also excludes the sal063-
derived ISO/VFS and SDL audio implementations until their separate public review
is complete. These exclusions are enforced by
`assets/public_source_profile.json`, not by a disclaimer. The unavailable seams
reject the capability and do not fabricate fonts, disc reads, playback, keys, or
success.

The HST-specific title manifest and private engineering/review documents are also
outside the active public source tree. Synthetic title manifests and generic schemas are
retained; no game executable, extracted asset, save, key, capture, oracle trace,
or generated retail translation unit is part of the public source boundary.

## Attribution contract

Preserve each file's SPDX header and any upstream notice when copying or
modifying source. The public provenance ledger assigns every included path an
explicit class, evidence reference, and content hash. A missing or unresolved
record is a publication failure. Generated files remain generated and must not be
hand-edited.

New project-authored source files use `SPDX-License-Identifier: GPL-3.0-or-later`.
Files classified as upstream-derived retain the SPDX identifier and notices
for their recorded source lineage.

## Upstream authors

Nakagawa Recomp is not endorsed by PPSSPP, sal063, FFmpeg, SDL, Khronos, or any
other upstream author.

## Private inputs and history

Users must supply any external inputs locally. Private inputs, keys, saves,
captures, extracted assets and generated output must not be committed, attached
to issues, or included in a release.

## Presentation screenshots of Hot Shots Tennis

The PNG file docs/media/hst-title-screen.png is a presentation capture of Hot Shots Tennis: Get a Grip
running in Nakagawa Recomp on Windows. The game, its logo, artwork and on-screen text belong to their
owners and are not licensed under this repository's GPL-3.0-or-later declaration; the image is shown
only to identify the game. The repository contains no game data files, and users need their own copy of
the game. The file is listed with the SHA-256 of its exact bytes in assets/release_manifest.json.

## Project-authored Nakagawa brand artwork

The files docs/images/brand/nakagawa-mark-light.svg and
docs/images/brand/nakagawa-mark-dark.svg are the project-authored N1 Block & Branch
mark, rendered in dark ink for light page themes and light ink for dark page themes.
The artwork is distributed under GPL-3.0-or-later and listed with the SHA-256 of
each file's exact bytes in assets/release_manifest.json.
