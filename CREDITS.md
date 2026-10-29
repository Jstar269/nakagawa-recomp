# Credits and upstream lineage

Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit. That project is the immediate upstream for substantial portions of Nakagawa's recompiler, runtime, renderer, reference interpreter, and tooling.

Nakagawa has since substantially extended and modified that codebase. This repository does **not** claim clean-room or independently originated recompiler lineage.

## Primary upstreams

- **PSP Recompilation Project — sal063 / psp-recomp authors** — GPL-2.0-or-later. Immediate upstream and original basis of this repository. The upstream attribution document is preserved at [`THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`](THIRD_PARTY_LICENSES/SAL063_CREDITS.txt).
- **PPSSPP — Henrik Rydgård and contributors** — GPL-2.0-or-later plus its own third-party notices. A significant part of the immediate upstream itself derives from, ports, or models PPSSPP subsystems; Nakagawa also retains and extends some of that downstream lineage. The per-file lineage is in [`src/rt/PROVENANCE.md`](src/rt/PROVENANCE.md).

## Other upstreams

- **FFmpeg n4.4 — the FFmpeg developers** — LGPL-2.1-or-later. The ATRAC3 and ATRAC3+ decoder subsets in [`src/rt/atrac3p/`](src/rt/atrac3p/) and the H.264 video decoder subset in [`src/rt/h264_ffmpeg_sub/`](src/rt/h264_ffmpeg_sub/); two imported ATRAC files also carry a MIPS Technologies, Inc. BSD-3-Clause notice.
- **pspdecrypt — John Kelley (John-K) and contributors** — GNU GPL version 3, pinned at
  `c156627db7634d395c380c0a9589130f603307fc`. The ported `~PSP` container, KIRK, AES, SHA-1 and KL4E/KL3E code of the
  built-in game decryption in [`src/core/`](src/core/PROVENANCE.md); its KIRK engine is Draan's KIRK-ENGINE
  (GPL-3.0-or-later).
- **PPSSPP game key tables** — the built-in game-decryption tables in
  [`src/core/nk_psp_builtin_keys.c`](src/core/PROVENANCE.md) are taken from PPSSPP at
  `b4386554795949491a26e51b7d74082bb9b2470f` (`Core/ELF/PrxDecrypter.cpp`, GPL-2.0-or-later, and
  `ext/libkirk/kirk_engine.c`, KIRK-ENGINE by Draan, GPL-3.0-or-later), firmware entries left out.
- **PPSSPP PSN image reader** — the PSN UMD image (NPUMDIMG) reader in [`src/core/`](src/core/PROVENANCE.md) is
  adapted from PPSSPP at `b4386554795949491a26e51b7d74082bb9b2470f`: the DRM BB MAC and cipher from
  `ext/libkirk/amctrl.c` (amctrl.prx reverse engineering by tpu), the LZRC decoder from
  `Core/FileSystems/tlzrc.cpp` (based on benhur's code, rewritten by tpu; Copyright (c) 2012- PPSSPP Project,
  GPL-2.0-or-later) and the block reader from `Core/FileSystems/BlockDevices.cpp` (GPL-2.0-or-later).
- **minimp3 — lieff (Lion) and contributors** — CC0-1.0 public-domain dedication. The MPEG audio decoder header in [`src/rt/minimp3/`](src/rt/minimp3/), pinned to upstream commit `ea99364f61c14656440e8d77e9c233ccf3124633` and kept byte-identical; it backs the `sceMp3` library.
- **Clay — Nic Barker** — zlib. The layout library in [`third_party/clay/`](third_party/clay/).
- **Portable Snippets — Evan Nemerson** — CC0-1.0 (public domain). The overflow-checked arithmetic header in [`third_party/portable-snippets/`](third_party/portable-snippets/).
- **dr_libs (dr_wav) — David Reid** — public domain (Unlicense), elected from its Unlicense or MIT-0 choice. The WAV writer selftests use, in [`third_party/dr_libs/`](third_party/dr_libs/).
- **libchdr — Romain Tisserand and contributors** — BSD-3-Clause, pinned at
  `47d794151b2b64f6aba60304e6480f4f5a2bc24b`, with the codecs it carries: miniz 3.1.2 (RAD Game Tools, Valve Software,
  Rich Geldreich; MIT), the Zstandard 1.5.7 decompressor (Meta Platforms; BSD-3-Clause), the LZMA SDK decoder
  (Igor Pavlov; public domain) and dr_flac (David Reid; public domain, Unlicense elected). It reads CHD disc images
  when the player expands them at import, in [`third_party/libchdr/`](third_party/libchdr/PROVENANCE.md).
- **Monocypher 4.0.3 — Loup Vaillant, Michael Savage, Fabio Scotoni and contributors** — dual BSD-2-Clause or
  CC0-1.0; the project uses it under CC0-1.0. The Ed25519 signature check in
  [`third_party/monocypher/`](third_party/monocypher/).
- **PSPSDK / PSPDEV contributors** — BSD-family notices for public ABI definitions and source-owned fixtures ([`THIRD_PARTY_LICENSES/PSPSDK.txt`](THIRD_PARTY_LICENSES/PSPSDK.txt)).
- **SDL, SDL_ttf and their dependencies** — staged beside the player with their licence texts (listed in [`NOTICE.md`](NOTICE.md)).
- **stb_image 2.30 — Sean Barrett** — public domain (Unlicense), the alternative to MIT that upstream offers. The PNG
  decoder in [`third_party/stb/`](third_party/stb/), for the player's prompt glyphs and the disc art read from your own disc.
- **Kenney "Input Prompts" and "Interface Sounds" — Kenney Vleugels ([kenney.nl](https://kenney.nl))** — CC0-1.0. The
  next UI's button prompt glyphs and its optional launcher sounds, in [`third_party/kenney/`](third_party/kenney/).
- **Font fixtures** — Gudea (Agustina Mingote), Gowun Dodum (The Gowun Dodum Project Authors) and M+ 1p (The M+ Project Authors), all SIL Open Font License 1.1, used only by tests ([`fixtures/fonts/`](fixtures/fonts/)).

The authoritative, component-by-component provenance records are [`NOTICE.md`](NOTICE.md) and the
[`public provenance ledger`](assets/public_provenance_ledger.json).

This credits file is a discoverability aid, not a replacement for `LICENSE`, `NOTICE.md`, per-file license notices, or the third-party license files that govern redistribution. The project is dedicated to Blitzen; see the [Dedication](README.md#dedication) in `README.md`.

## Project UI fonts

- **Chakra Petch** — Cadson Demak — SIL Open Font License 1.1. Static 600 and 700 files come from the pinned [Google Fonts source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/chakrapetch).
- **Geist and Geist Mono** — Andrés Briganti, Mateo Zaragoza, Guillermo Rauch, Evil Rabbit, José Rago, and Facundo Santana — SIL Open Font License 1.1. The shipped static weights are instances of the pinned [Google Fonts Geist source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/geist) and [Geist Mono source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/geistmono).
- **Noto Sans JP** — Google; upstream copyright notice credits Adobe — SIL Open Font License 1.1. The static 400 file is an instance of the pinned [Google Fonts source](https://github.com/google/fonts/tree/45e3d5269e4a17d823f6d84470bc084dc5ae46f6/ofl/notosansjp). The upstream Reserved Font Name `Source` is not used as the shipped family name.

The exact file and license hashes are in [`docs/provenance/FONT_ORIGINS.md`](docs/provenance/FONT_ORIGINS.md). These fonts are project UI assets, not game data.
