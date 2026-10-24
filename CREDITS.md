# Credits and upstream lineage

Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit. That project is the immediate upstream for substantial portions of Nakagawa's recompiler, runtime, renderer, reference interpreter, and tooling.

Nakagawa has since substantially extended and modified that codebase. This repository does **not** claim clean-room or independently originated recompiler lineage.

## Primary upstreams

- **PSP Recompilation Project — sal063 / psp-recomp authors** — GPL-2.0-or-later. Immediate upstream and original basis of this repository. The upstream attribution document is preserved at [`THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`](THIRD_PARTY_LICENSES/SAL063_CREDITS.txt).
- **PPSSPP — Henrik Rydgård and contributors** — GPL-2.0-or-later plus its own third-party notices. A significant part of the immediate upstream itself derives from, ports, or models PPSSPP subsystems; Nakagawa also retains and extends some of that downstream lineage. The per-file lineage is in [`src/rt/PROVENANCE.md`](src/rt/PROVENANCE.md).

## Other upstreams

- **FFmpeg n4.4 — the FFmpeg developers** — LGPL-2.1-or-later. The ATRAC3 and ATRAC3+ decoder subsets in [`src/rt/atrac3p/`](src/rt/atrac3p/) and the H.264 video decoder subset in [`src/rt/h264_ffmpeg_sub/`](src/rt/h264_ffmpeg_sub/); two imported ATRAC files also carry a MIPS Technologies, Inc. BSD-3-Clause notice.
- **Clay — Nic Barker** — zlib. The layout library in [`third_party/clay/`](third_party/clay/).
- **PSPSDK / PSPDEV contributors** — BSD-family notices for public ABI definitions and source-owned fixtures ([`THIRD_PARTY_LICENSES/PSPSDK.txt`](THIRD_PARTY_LICENSES/PSPSDK.txt)).
- **SDL, SDL_ttf and their dependencies** — staged beside the player with their licence texts (listed in [`NOTICE.md`](NOTICE.md)).
- **Font fixtures** — Gudea (Agustina Mingote), Gowun Dodum (The Gowun Dodum Project Authors) and M+ 1p (The M+ Project Authors), all SIL Open Font License 1.1, used only by tests ([`fixtures/fonts/`](fixtures/fonts/)).

The authoritative, component-by-component provenance records are [`NOTICE.md`](NOTICE.md) and the
[`public provenance ledger`](assets/public_provenance_ledger.json).

This credits file is a discoverability aid, not a replacement for `LICENSE`, `NOTICE.md`, per-file license notices, or the third-party license files that govern redistribution. The project is dedicated to Blitzen; see the [Dedication](README.md#dedication) in `README.md`.

## Project UI fonts

- **Chakra Petch** — Cadson Demak — SIL Open Font License 1.1. Static 600 and 700 files come from the pinned [Google Fonts source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/chakrapetch).
- **Geist and Geist Mono** — Andrés Briganti, Mateo Zaragoza, Guillermo Rauch, Evil Rabbit, José Rago, and Facundo Santana — SIL Open Font License 1.1. The shipped static weights are instances of the pinned [Google Fonts Geist source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/geist) and [Geist Mono source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/geistmono).
- **Noto Sans JP** — Google; upstream copyright notice credits Adobe — SIL Open Font License 1.1. The static 400 file is an instance of the pinned [Google Fonts source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/notosansjp). The upstream Reserved Font Name `Source` is not used as the shipped family name.

The exact file and license hashes are in [`docs/provenance/FONT_ORIGINS.md`](docs/provenance/FONT_ORIGINS.md). These fonts are project UI assets, not game data.
