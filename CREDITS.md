# Credits and upstream lineage

Nakagawa Recomp began as a fork of [sal063's PSP Recompilation Project](https://github.com/sal063/PSP-recompilation-project), a GPL-2.0-or-later PSP static-recompiler toolkit. That project is the immediate upstream for substantial portions of Nakagawa's recompiler, runtime, renderer, reference interpreter, and tooling.

Nakagawa has since substantially extended and modified that codebase. This repository does **not** claim clean-room or independently originated recompiler lineage.

## Primary upstreams

- **PSP Recompilation Project — sal063 / psp-recomp authors** — GPL-2.0-or-later. Immediate upstream and original basis of this repository. The upstream attribution document is preserved at [`THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`](THIRD_PARTY_LICENSES/SAL063_CREDITS.txt).
- **PPSSPP — Henrik Rydgård and contributors** — GPL-2.0-or-later plus its own third-party notices. A significant part of the immediate upstream itself derives from, ports, or models PPSSPP subsystems; Nakagawa also retains and extends some of that downstream lineage.

## Other upstreams

- **FFmpeg n4.4 — the FFmpeg developers** — LGPL-2.1-or-later. The ATRAC3 and ATRAC3+ decoder subsets in [`src/rt/atrac3p/`](src/rt/atrac3p/); two imported files also carry a MIPS Technologies, Inc. BSD-3-Clause notice.
- **Clay — Nic Barker** — zlib. The layout library in [`third_party/clay/`](third_party/clay/).
- **PSPSDK / PSPDEV contributors** — BSD-family notices for public ABI definitions and source-owned fixtures ([`THIRD_PARTY_LICENSES/PSPSDK.txt`](THIRD_PARTY_LICENSES/PSPSDK.txt)).
- **SDL, SDL_ttf and their dependencies** — staged beside the player with their licence texts (listed in [`NOTICE.md`](NOTICE.md)).
- **Font fixtures** — Gudea (Agustina Mingote), Gowun Dodum (The Gowun Dodum Project Authors) and M+ 1p (The M+ Project Authors), all SIL Open Font License 1.1, used only by tests ([`fixtures/fonts/`](fixtures/fonts/)).

The authoritative, component-by-component provenance records are [`NOTICE.md`](NOTICE.md) and the
[`public provenance ledger`](assets/public_provenance_ledger.json).

This credits file is a discoverability aid, not a replacement for `LICENSE`, `NOTICE.md`, per-file license notices, or the third-party license files that govern redistribution. The project is dedicated to Blitzen; see the [Dedication](README.md#dedication) in `README.md`.
