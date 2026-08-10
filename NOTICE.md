# Notices, provenance, and disclosures

The repository-level project declaration is **GPL-3.0-or-later**, and the canonical GPLv3 text is in [LICENSE](LICENSE). Many individual source files and inherited components retain GPL-2.0-or-later or other upstream-specific terms. This notice records origin and redistribution considerations; it is not a legal determination that every tracked component or combined configuration is presently cleared for redistribution. Specific inherited components remain under pre-publication provenance review as described below.

## Project lineage

- **PSP Recompilation Project** by sal063 — GPL-2.0-or-later — <https://github.com/sal063/PSP-recompilation-project>. Nakagawa Recomp began as a fork of this toolkit. That inheritance is **substantial and current**: measured against upstream `da17b0e`, 11,310 of sal063's 13,482 normalized code lines are still present across 44 common source files — 83.9% of the upstream — and 40.1% of the current code in those files is shared with it. The upstream's own attribution document is reproduced verbatim at [THIRD_PARTY_LICENSES/SAL063_CREDITS.txt](THIRD_PARTY_LICENSES/SAL063_CREDITS.txt); paths inside that file refer to the **upstream** tree, not this one. Per-file figures and method: [docs/provenance/SAL063_RETENTION_2026-08-06.md](docs/provenance/SAL063_RETENTION_2026-08-06.md).
- **PPSSPP** — GPL-2.0-or-later, with additional third-party notices in its `LICENSE.TXT` — <https://github.com/hrydgard/ppsspp>. Nakagawa Recomp ports or adapts PPSSPP behavior in HLE, MPEG/PSMF, GE, PGF, and VFPU code, and uses PPSSPP-origin runtime data described below. PPSSPP is also used externally to produce optional oracle traces; its source is not vendored or linked.

Most PPSSPP-derived material reached this project **through sal063**, which is the immediate upstream for every file in both inventories below. Nakagawa added the standardized `Derived from` source-header form to most of these files; that is not the same as originating every attribution. In particular, sal063's `pgf.c`/`pgf.h` bodies and `CREDITS.md` already expressly identify those files as a C port of PPSSPP `Core/Font/PGF.cpp`.

### Upstream source file inventory (PPSSPP-derived modules)

The following C runtime files in `src/rt/` materially incorporate translated or adapted algorithms, state structures, tables, or HLE logic derived from PPSSPP:

| Nakagawa Path | Upstream PPSSPP Subsystem | Upstream License / Attribution |
| --- | --- | --- |
| `src/rt/ge.c`, `src/rt/ge_shared.h` | `Core/GE/`, `GPU/` (GE state & soft-rasterizer) | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/hle.c`, `src/rt/nid_names.h` | `Core/HLE/` (Kernel objects & HLE dispatch tables) | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/pgf.c`, `src/rt/pgf.h` | `Core/Font/PGF.cpp`/`PGF.h` (primary parser/rasterizer), `Core/HLE/sceFont.cpp` (HLE/API), JPCSP `SceFontInfo.java`/`PGF.java`; [source archaeology](docs/provenance/PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md) | GPL-2.0-or-later / GPL-3.0-or-later (PPSSPP / JPCSP contributors) |
| `src/rt/mpeg.c` | `Core/HLE/sceMpeg.cpp` (MPEG/PSMF demux & headers), upstream revision `4e109dd6` as recorded by sal063 | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/audio.c` | `Core/HLE/sceSasCore.cpp`, `Core/Audio/` — **see the caveat below** | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/vfpu_interp.c` | `Core/MIPS/VFPU/` (Allegrex VFPU operations) | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/recomp.c`, `src/rt/recomp.h` | `Core/MIPS/MIPS*` (CpuState layout, VFPU prefixes and transcendental kernels, unaligned access) | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/savedata.c` | `Core/Dialog/SavedataParam.{h,cpp}` (parameter block, result codes) | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/evf.h` | `Core/HLE/sceKernelEventFlag.cpp` (pattern/mode semantics) | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |
| `src/rt/gpu_sdl3vk/ge_gpu.c` | Not a port of PPSSPP's GPU; reproduces PSP/GE pixel rules derived from PPSSPP's software renderer via `ge.c`, so still a derivative work | GPL-2.0-or-later (Henrik Rydgård & PPSSPP contributors) |

Two entries need qualification rather than a flat claim.

**`src/rt/audio.c` is unresolved.** Its file header declares sal063 lineage, the body cites PPSSPP nowhere, and sal063's own `CREDITS.md` itemizes its PPSSPP translations file by file **without listing `audio.c`**. The `sceSasCore` attribution above is therefore uncorroborated by the immediate upstream's own record. It is retained deliberately — absence from an upstream list is evidence, not proof — pending a PPSSPP source comparison.

**`src/rt/ge_shared.h` is a chain, not a conflict.** Its header declares sal063 and this table declares PPSSPP; both are correct at different levels. 100% of it comes from sal063, and sal063's GE state structures are themselves PPSSPP-derived.

`src/rt/nid_names.h` and its corpus `tools/nid_corpus.json` retain their attribution: the **function names** were seeded from PPSSPP's `Core/HLE` tables and remain PPSSPP-sourced. Since the IND-1 work recorded in [docs/provenance/INDEPENDENCE_BACKLOG.md](docs/provenance/INDEPENDENCE_BACKLOG.md), 1463 of the 1615 **numeric NIDs** are no longer transcribed — they are recomputed from the name at generation time as `sha1(name)[0:4]` little-endian, the derivation `psp-build-exports` applies when building a PRX. This reduces what is copied; it does not change where the names came from, and no independence is claimed for the name list.

### Upstream source file inventory (sal063-derived modules)

sal063 is the **immediate** upstream for the files below. Percentages are the share of the current file that is shared with upstream `da17b0e`, measured as described in [docs/provenance/SAL063_RETENTION_2026-08-06.md](docs/provenance/SAL063_RETENTION_2026-08-06.md); they are close upper bounds, and textual retention is not a conclusion about what is protectable. All are GPL-2.0-or-later, © the psp-recomp authors.

| Nakagawa path | shared with sal063 | note |
| --- | --- | --- |
| `src/rt/osk_win.c` | 100% | unchanged apart from the added SPDX header |
| `src/rt/mpeg.c` | 82% | also PPSSPP-derived (above) |
| `src/rt/h264_mf.c` | 74% | Media Foundation H.264 backend |
| `src/rt/funcdiff.c` | 72% | |
| `src/rt/pgf.c` | 70% | also PPSSPP/JPCSP-derived (above) |
| `src/ref/interp.cpp`, `src/ref/interp.h`, `src/ref/cpu.h` | 69–94% | reference interpreter |
| `src/rt/vfpu_interp.c` | 63% | also PPSSPP-derived (above) |
| `src/rt/ge.c` | 62% | also PPSSPP-derived (above) |
| `src/rt/savedata.c` | 61% | also PPSSPP-derived (above) |
| `src/rt/gui.c` | 52% | |
| `src/rt/gpu_sdl3vk/sdl3vk.c`, `sdl3vk.h`, `ge_gpu.c`, `ge_gpu.h` | 11–82% | renderer; `ge_gpu.c` also PPSSPP-informed (above) |
| `src/rt/gpu_sdl3vk/shaders/psp.vert`, `psp.frag` and their embedded SPIR-V | GLSL originates upstream | `psp.vert` is 25 → 27 lines |
| `src/rt/ge_shared.h` | 46% | |
| `src/rt/vfpu_fuzz.c` | 46% | |
| `src/ref/run_elf.cpp`, `src/ref/selftest.cpp` | 32–46% | |
| `src/rt/iso.h`, `src/rt/iso.c` | 8–44% | |
| `tools/analyze.py` | 44% | |
| `tools/codegen.py` | 38% | 84% of upstream's codegen is retained; the file has since more than doubled |
| `tools/codegen_gate.py`, `tools/microtest_gate.py`, `tools/gen_microtest.py` | 36–39% | |
| `src/rt/audio.c` | 36% | PPSSPP attribution unresolved, see above |
| `src/rt/driver.c` | 31% | |
| `src/rt/recomp.h`, `src/rt/recomp.c` | 13–23% | also PPSSPP-derived (above) |
| `src/rt/hle.c` | 20% | also PPSSPP-derived (above) |
| `src/rt/sched.c` | 20% | |
| `tools/prxload.py`, `tools/imports.py` | 20–23% | |
| `tools/tracediff.py`, `tools/funcdiff_cmp.py`, `tools/ppm2png.py`, `tools/nidseq.py` | 70–100% | small utilities |
| `tools/ppmdiff.py`, `tools/vfpu_fuzz_gen.py`, `tools/gen_nidnames.py` | 6–18% | `gen_nidnames.py` was rewritten by IND-1 |

`tools/host_stubs.py`, `tools/elf_bounds.py`, `tools/discovery_contract.py` and `tools/psp_import_table.py` were verified **absent** from upstream `da17b0e` and are Nakagawa additions.

`src/rt/nid_names.h` and its corpus `tools/nid_corpus.json` retain that attribution: the **function names** were seeded from PPSSPP's `Core/HLE` tables and remain PPSSPP-sourced. Since the IND-1 work recorded in [docs/provenance/INDEPENDENCE_BACKLOG.md](docs/provenance/INDEPENDENCE_BACKLOG.md), 1463 of the 1615 **numeric NIDs** are no longer transcribed — they are recomputed from the name at generation time as `sha1(name)[0:4]` little-endian, the derivation `psp-build-exports` applies when building a PRX. This reduces what is copied; it does not change where the names came from, and no independence is claimed for the name list.

Do **not** infer from the repository-level GPL-3.0-or-later declaration that every tracked component or combined configuration is cleared for distribution. PPSSPP's own `Core/Font/PGF.cpp` records JPCSP lineage and warns that copied portions make that file effectively GPLv3; Nakagawa's corresponding PGF provenance is therefore an unresolved publication blocker ([#98](https://github.com/Jstar269/nakagawa-recomp/issues/98)). GPL-2.0-or-later material can be conveyed under GPLv3 when that option is needed, but the exact combined-work license presentation must be settled before publication. Binary distributors must satisfy the applicable corresponding-source and notice requirements for the exact source used to build their binary.

### PGD/amctrl provenance qualification

The private/full-source PGD implementation has mixed provenance. Recovered July 18 archive evidence
supports `tools/pgd_decrypt.py` first and `src/rt/pgd.c` as its later C port. The PSP-specific
BBMac/BBCipher/PGD flow is conservatively classified **derived-translated** from the public
tpu/Fake_NP, JPCSP, PPSSPP/libkirk, tpunix, and sign_np implementation family. The locally organized
AES primitives and later validation, overflow, allocation, streaming, cleanup, key-externalization,
and public-safe work are independently expressed. No substantial near-verbatim Nakagawa function
body was found; that does not make the translated PSP flow independent.

ProximaV/kirk-engine-full is separate KIRK-core lineage evidence. It does not contain a high-level
amctrl, BBMac, BBCipher, or PGD implementation and is not recorded as an ancestor of Nakagawa's
surrounding PGD layer. PSP platform data, the title-specific version key, standard AES facts, and
source-code expression are separate provenance categories.

The pinned history and function/block matrix are in
[docs/provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md](docs/provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md).
This engineering classification does not determine the applicable notice/license or
anti-circumvention treatment. [Issue #104](https://github.com/Jstar269/nakagawa-recomp/issues/104)
remains open, and `public-safe-v1` continues to exclude the implementation.

## Redistributed PPSSPP data

The contents of `font/` and `assets/vfpu/` were byte-for-byte verified on 2026-07-18 against PPSSPP commit `f0baf3ade7bcb6c86f0835962b36eb4e51559d8f`:

- `font/{jpn0,kr0,ltn0,ltn8}.pgf` match `assets/flash0/font/` in PPSSPP.
- Every checked-in `assets/vfpu/*.dat` file matches the same path under PPSSPP.

The exact Git blob IDs and repeatable verification commands are documented in
[font/README.md](font/README.md),
[assets/README.md](assets/README.md), and the machine-readable
[assets/vfpu/PROVENANCE.json](assets/vfpu/PROVENANCE.json). These are PPSSPP's
replacement data files, not files extracted from the user's game or PSP
firmware.

**The PGF fonts' redistribution terms are unresolved.** Byte identity to PPSSPP
is verified, but PPSSPP's GPL-2.0-or-later is the license of the PPSSPP
*program*; a font binary shipped inside it does not inherit that license.
Upstream history now supports family-level source metadata for the current
blobs: Source Han Sans for the current jpn0/kr0 line and Ume Hy Gothic for the
current even Latin ltn0/ltn8 line. The exact source TTF/release, all later
PGF edits, and the required notices remain unproven. The compatibility names
embedded by PPSSPP are not evidence of Sony or Fontworks outlines. These
files are not asserted to be GPL-licensed, and nothing here suggests they are
proprietary either. See
[THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt](THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt)
and publication blocker
[#99](https://github.com/Jstar269/nakagawa-recomp/issues/99). The `assets/vfpu/`
tables are a separate question and are not covered by that caveat.

PPSSPP's license also preserves a BSD-compatible PSPSDK notice for defines, constants, and headers. That notice is reproduced in [THIRD_PARTY_LICENSES/PSPSDK.txt](THIRD_PARTY_LICENSES/PSPSDK.txt).

## Build and runtime dependencies

- **libxb** — MIT — <https://github.com/kiwi515/libxb>. Optional local dependency of `tools/extract_xb.py`; ignored and not redistributed here.
- **SDL3** — zlib — <https://libsdl.org/>. Linked by the runtime. Local DLLs are ignored; binary release packaging must include SDL's license notice when SDL is redistributed.
- **Vulkan Loader and headers** — Apache-2.0 — <https://github.com/KhronosGroup/Vulkan-Loader>. Local loader DLLs and SDK files are ignored; binary release packaging must preserve applicable notices for any redistributed loader.
- **Microsoft Media Foundation** — Windows system component used by `src/rt/h264_mf.c`; no Microsoft source or redistributable is included.
- **shadcn/ui** — MIT — <https://github.com/shadcn-ui/ui>. The dashboard was scaffolded with shadcn/ui and contains adapted UI primitives. Its MIT notice is preserved in [THIRD_PARTY_LICENSES/SHADCN_UI.txt](THIRD_PARTY_LICENSES/SHADCN_UI.txt).
- The dashboard's JavaScript dependency licenses are recorded by its lockfile and must be reviewed when publishing a packaged dashboard.

## Game data and trademarks

`eboot.elf`, `game.iso`, `place_game_here/`, `original_game/`, decrypted PRXs,
extracted game assets, private savedata/install data, locally supplied
game-specific values, oracle traces, framebuffer dumps, and game-derived hash
manifests are excluded from Git. Users must supply their own legally obtained
inputs. Do not attach any of these files to issues, releases, or test fixtures.

This repository does not grant rights to the game, firmware, encryption keys,
or other third-party content. Copyright, anti-circumvention, contract, and
reverse-engineering rules vary by jurisdiction; users and redistributors are
responsible for obtaining advice appropriate to their facts.

The project ships **no decryption keys of any kind**. This covers both the
target-game version key and the PSP KIRK/amctrl console constants the PGD path
needs. Neither is present in the current Git tree, the build output, or any
release artifact; both are supplied locally by the user. (The PSP KIRK/amctrl
console constants do remain in pre-removal Git *history*; purging them from all
reachable history is a mandatory step before this repository is made public —
see [docs/KEY_HISTORY_SCRUB.md](docs/KEY_HISTORY_SCRUB.md).) The version key is read from
`HST_PGD_VKEY_HEX`, and the console constants from `$SR_PGD_KEYS` (default
`keys/pgd_keys.txt`, gitignored) — see [docs/PGD_KEYS.md](docs/PGD_KEYS.md).
Without them the PGD path simply reports itself unavailable and the rest of the
project builds, tests, and runs normally; the private PGD integration tests skip.

Distribution of the PGD compatibility code itself, and its intended
interoperability use, remains a narrow question for qualified human legal
review; this notice does not resolve it.

## Reporting a copyright or trademark concern

If you are a rights holder, or believe this repository contains material it
should not, contact the repository owner privately through the contact method on
their GitHub profile — the same private route described in
[SECURITY.md](SECURITY.md) and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Please
do not open a public issue for such a report.

Include the material at issue and where it appears (path and revision), the
right you are asserting, and how you would like it resolved. Reports are handled
on a best-effort basis by an individual maintainer; this project has no legal
department and no formal response-time commitment. Good-faith requests to remove
or attribute specific material will be acted on promptly.

*Hot Shots Tennis*, PSP, Sony, and Clap Hanz names and marks belong to their respective owners and are used only to identify compatibility. The project name references the in-game Nakagawa Tennis Club; it does not imply ownership of that game element or affiliation with its rights holders. This project is independent and is not endorsed by those owners or by PPSSPP.

## AI-assistance disclosure

Large-language-model tools were used substantially to draft, translate, review, debug, and document portions of the project, including the SDL3/Vulkan renderer and parts of the runtime/HLE work. Human review and repository tests remain necessary: AI involvement does not establish correctness, security, originality, or license compatibility. PPSSPP-derived translations remain subject to PPSSPP's license regardless of the tool used to assist the translation.
