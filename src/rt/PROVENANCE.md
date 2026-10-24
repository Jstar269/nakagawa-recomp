# PROVENANCE.md — `src/rt/` and `src/ref/` (PPSSPP and sal063 lineage)

Runtime and reference-interpreter source that this project inherited from the
sal063 PSP Recompilation Project, a fork whose code in turn derives from PPSSPP,
plus files the project created from those. This manifest covers the 34 files
under `src/` that are marked GPL-2.0-or-later. It records what the project can
establish about their origin and states plainly what it cannot.

Related but separate manifests: `src/rt/atrac3p/PROVENANCE.md` (FFmpeg
subset), `src/rt/minimp3/PROVENANCE.md` (minimp3), `third_party/clay/PROVENANCE.md` (Clay), and
`assets/vfpu/PROVENANCE.json` (PPSSPP VFPU tables).

## Known gap (read this first)

The exact PPSSPP revision of every inherited file is **unknown**. These files
reached this repository through the sal063 lineage (sal063/PSP-recompilation-project
commit `da17b0e1db209206a407d097d132201e516e3855`, 2026-06-14), the first public
Nakagawa history begins with the sanitized export `bf21bdec` (2026-08-11), and
neither sal063's tree nor its credits file records which PPSSPP revision each
file was translated from. The pin for those files is therefore recorded as
"unknown, inherited via sal063 `da17b0e`". This is not a pin and does not meet
the exact-revision rule in `AGENTS.md` section 4 for new imports. It is recorded so
that the lineage is discoverable, not to claim that the rule was met.

The one partial pin that exists is described under "Pins" below.

## Upstream sources

- Project: PPSSPP
- Repository: `https://github.com/hrydgard/ppsspp`
- Copyright: Copyright (c) 2012- PPSSPP Project and contributors
- Licence: GPL-2.0-or-later, plus PPSSPP's own third-party notices. The
  project conveys its combined work under GPL-3.0-or-later, which
  GPL-2.0-or-later permits through its "or any later version" clause. Per-file
  GPL-2.0-or-later identifiers are kept unchanged (`THIRD_PARTY_LICENSES/GPL-2.0.txt`,
  `LICENSE`, `NOTICE.md`).
- Intermediate upstream: sal063/PSP-recompilation-project, GPL-2.0-or-later,
  public revision `da17b0e1db209206a407d097d132201e516e3855`. Its attribution
  document is preserved verbatim, with Nakagawa corrections, at
  `THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`. No PPSSPP source tree is bundled in
  this repository.
- Import method: none by this project. The files were inherited with the
  repository. The sal063 base blob SHA of each file is recorded in the manifest so a
  reader can compare against the sal063 tree.

## Pins

| Item | Pin | Evidence |
| --- | --- | --- |
| sal063 base for all inherited files | `da17b0e1db209206a407d097d132201e516e3855` (2026-06-14, "Update README.md") | `NOTICE.md`; `THIRD_PARTY_LICENSES/SAL063_CREDITS.txt`; the commit and its tree exist on GitHub and list the files marked "sal063 base blob" below. |
| PPSSPP source of the sceMpeg port in `rt/mpeg.c` (and the mpeg wrappers in `rt/hle.c`) | `4e109dd6ae34cbcb39751bb7647d345569700161` (PPSSPP commit of 2026-05-20), file `Core/HLE/sceMpeg.cpp` | sal063's own `src/rt/mpeg.c` at `da17b0e` begins "ported from PPSSPP (Core/HLE/sceMpeg.cpp, rev 4e109dd6, GPLv2+)", and `SAL063_CREDITS.txt` repeats "rev 4e109dd6". The abbreviation was expanded to the full commit with the GitHub API; it resolves to exactly one PPSSPP commit that predates the sal063 base. This is a claim made by sal063 and has not been re-verified against the translated code. |
| PPSSPP source of the sceMp3 adaptation in `rt/hle.c` (added 2026-10-24) | `0028f30da6784570264a41ef6cce1c3963692ea7` (PPSSPP `master` when adapted), files `Core/HLE/sceMp3.cpp` and `Core/HW/SimpleAudioDec.cpp` (`AuCtx`) | Adapted by this project under the 2026-10-20 import rule; the sceMp3 block comment in `rt/hle.c` names the pin, keeps the PPSSPP copyright line, lists the modifications and labels the block derived-translated. The decoder behind it is minimp3 (`src/rt/minimp3/PROVENANCE.md`), not PPSSPP code. |
| Every other PPSSPP-derived file | unknown | No revision text exists in sal063's tree or in this repository's history. |

## Import-rule check (2026-10-20 and later)

The emulator import rule (`AGENTS.md` section 4, maintainer-approved 2026-10-20 and
extended 2026-10-22) requires an exact pin for new PPSSPP adaptations. A check
for later adaptation found none:

- `git log --since=2026-10-20 -p` over `src` (excluding the test
  `rt/hle_thread_selftest.c`) shows the only added lines that
  mention PPSSPP are the ten "Copyright (c) 2012- PPSSPP Project and
  contributors" notice lines and one licence sentence in `rt/mpeg.c`, all in
  `8ca38e63` (legal hygiene, #1187). That commit also removed text about a
  PPSSPP-derived decryption file, which was deleted.
- No file under `src`, `assets/vfpu`, `tools` or `third_party` that was added
  since 2026-10-20 contains the string "PPSSPP".
- The other commits since 2026-10-20 that touch the files listed below are
  PSP-measurement and bug-fix changes and add no PPSSPP text.

- 2026-10-24: the sceMp3 block in `rt/hle.c` adapts PPSSPP `Core/HLE/sceMp3.cpp`
  and `Core/HW/SimpleAudioDec.cpp` at the exact pin in the table above, and says so.

This is a text search of the history, not proof that no PPSSPP code was typed
in by hand without a mention. The 2026-10-20 rule did not change the status of
the inherited files above.

## Licence notes

- `rt/evf.h` is the only file in the manifest whose SPDX line is
  GPL-3.0-or-later while its header says it is derived from PPSSPP
  (GPL-2.0-or-later). It was relabelled in `9fcd3c59` (#419, maintainer
  decision). That is permitted, because GPL-2.0-or-later allows conveying under
  GPL-3.0-or-later, and this manifest leaves the licence line as it is. It is
  inconsistent with its siblings and is noted here so the difference is
  deliberate rather than silent.
- All other manifest files keep GPL-2.0-or-later. No file in the manifest is
  under a licence incompatible with GPL-3.0-or-later.
- `rt/mpeg.c` states in its header that its licence covers the runtime source
  and says nothing about a user's game.

## File manifest

Legend:

- `sal063 base blob`: Git blob SHA of the same path in sal063 `da17b0e`
  (shown as the first 12 hex digits). Line counts in "Local modifications"
  compare the sal063 file with the file in this tree after normalizing CRLF
  to LF; "+A/-D" is the number of lines added and deleted by a diff, which
  includes the added header lines. No inherited file is byte-identical to its
  sal063 base: every file here is a `LOCAL DELTA` of its sal063 base, because each
  gained at least the Nakagawa notice header. `rt/gpu_sdl3vk/shaders/psp.vert`
  differs only by those header lines.
- `Commits here`: number of commits in this repository's history that touch the
  file (`git log --follow`).
- Classification words follow `docs/provenance/INDEPENDENCE_MODEL.md`.
  `inherited` means the file came with the sal063 lineage and no finer
  statement about PPSSPP translation can be made from the evidence.
- The public provenance ledger (`assets/public_provenance_ledger.json`)
  records every file below as `upstream_derived` except `rt/evf.h`
  (`project_authored_attested`) and `rt/nid_names.h`
  (`generated_from_public_source`). This manifest does not change those records.

### A. Headers declare a PPSSPP derivation (derived-translated)

Each header carries the sal063 derivation line, the Nakagawa modification line and the PPSSPP derivation and copyright lines. The label is `derived-translated` (`docs/provenance/INDEPENDENCE_MODEL.md`); no independence is claimed. How much of each file is translated is not measured here.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `rt/ge.c` | sal063 `da17b0e` | derived-translated | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `e9a7203f74a8`: +2950/-249 lines (2440 -> 5141). Software GE rasterizer. SAL063_CREDITS names PPSSPP `GPU/Software/*`, `GPU/Common/VertexDecoder*` and `GPU/ge_constants.h` as the models. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10; PPSSPP derivation line; PPSSPP copyright line | 26 |
| `rt/hle.c` | sal063 `da17b0e` | derived-translated | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `01c5307f7fa0`: +23252/-885 lines (1999 -> 24366). Kernel/HLE core. Header and body name PPSSPP `Core/HLE/*` (sceAtrac, sceAudio, sceCtrl, sceGe, sceIo, sceKernelMutex, sceMpeg, ErrorCodes.h) and `Core/HW/Display.cpp` as models. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-11; PPSSPP derivation line; PPSSPP copyright line | 161 |
| `rt/hle_power.c` | sal063 `da17b0e`, via `hle.c` | derived-translated | unknown (inherited via sal063 `da17b0e`) | not in the sal063 tree. scePower domain. Extracted from hle.c in cb1ae535 (#241); body cites PPSSPP `Core/HLE/scePower.cpp`. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-11; PPSSPP derivation line; PPSSPP copyright line | 10 |
| `rt/hle_power.h` | sal063 `da17b0e`, via `hle.c` | derived-translated | unknown (inherited via sal063 `da17b0e`) | not in the sal063 tree. scePower domain header. Extracted from hle.c in cb1ae535 (#241). | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-11; PPSSPP derivation line; PPSSPP copyright line | 7 |
| `rt/mpeg.c` | sal063 `da17b0e` | derived-translated | `Core/HLE/sceMpeg.cpp` at `4e109dd6ae34cbcb39751bb7647d345569700161` per sal063 (see Pins); other inputs unknown | vs sal063 base blob `573cc4e76387`: +755/-83 lines (532 -> 1204). sceMpeg/PSMF port. Header: a faithful port of PPSSPP's sceMpeg control flow. Source `Core/HLE/sceMpeg.cpp`. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10; PPSSPP derivation line; PPSSPP copyright line | 14 |
| `rt/recomp.c` | sal063 `da17b0e` | derived-translated | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `3e4420d0a211`: +2307/-271 lines (503 -> 2539). CPU core runtime and VFPU prefix/transcendental kernels. SAL063_CREDITS names `Core/MIPS/MIPS.cpp` and PPSSPP's VFPU kernels. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10; PPSSPP derivation line; PPSSPP copyright line | 32 |
| `rt/recomp.h` | sal063 `da17b0e` | derived-translated | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `c13d993dfa94`: +1466/-51 lines (211 -> 1626). `CpuState` layout and semantics. SAL063_CREDITS names `Core/MIPS/MIPS*`. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-11; PPSSPP derivation line; PPSSPP copyright line | 51 |
| `rt/savedata.c` | sal063 `da17b0e` | derived-translated | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `7c58c7f1a6cf`: +666/-177 lines (622 -> 1111). sceUtilitySavedata. Header: field offsets and result codes follow PPSSPP `Core/Dialog/SavedataParam.h` (interface layout). | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10; PPSSPP derivation line; PPSSPP copyright line | 9 |
| `rt/vfpu_interp.c` | sal063 `da17b0e` | derived-translated | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `2b3c31156965`: +422/-36 lines (410 -> 796). VFPU interpreter. SAL063_CREDITS: exact ports of the table-based VFPU transcendentals and the `vcst` table from `Core/MIPS/MIPS.cpp`. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10; PPSSPP derivation line; PPSSPP copyright line | 13 |

### B. Header declares a PPSSPP derivation that the trusted ledger does not record

`evf.h` is not in the sal063 tree; it first appears in the Nakagawa export `bf21bdec`. Its header says it derives from PPSSPP's `Core/HLE/sceKernelEventFlag.cpp`, and it is licensed GPL-3.0-or-later (see Licence notes). The public provenance ledger records it as `project_authored_attested` (record `hle-event-flag-semantics`). The header and the ledger disagree, so the honest label is `unresolved` until the maintainer reconciles them.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `rt/evf.h` | none (new in `bf21bdec`) | unresolved (header: PPSSPP-derived; ledger: project-authored) | unknown; no revision is recorded anywhere | not in the sal063 tree. Pure sceKernelEventFlag pattern and mode helpers; behaviour is cross-checked against PPSSPP `Core/HLE/sceKernelEventFlag.cpp` | SPDX GPL-3.0-or-later; psp-recomp authors copyright; PPSSPP derivation line; PPSSPP copyright line | 3 |

### C. PPSSPP-seeded data

`nid_names.h` is generated by `tools/gen_nidnames.py` from `tools/nid_corpus.json`. Most NIDs are computed from their names (the file header gives the current split). The name list was seeded from PPSSPP's `Core/HLE` tables, and the file header says it remains PPSSPP-suggested with no independence claimed.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `rt/nid_names.h` | sal063 `da17b0e` (generated file) | derived-data (names) | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `d363335b15e1`: +25/-12 lines (1646 -> 1659). Regenerated by this project's generator; not edited by hand | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 1 |

### D. sal063 lineage, GE semantics reproduced from PPSSPP through ge.c (inherited)

SAL063_CREDITS.txt says the SDL3 + Vulkan renderer is not a port of PPSSPP code but reproduces PSP/GE pixel rules derived from PPSSPP, so it is a derivative work for licensing. The file notices say the same. Nothing in these files says a PPSSPP source file was translated, so the label is `inherited`, not `derived-translated`.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `rt/ge_shared.h` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `dde85846ce6c`: +196/-7 lines (123 -> 312). Shared GE state between ge.c and the GPU backend; comments cite PPSSPP flag and accessor behaviour. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 6 |
| `rt/gpu_sdl3vk/README.md` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `e99c5b222140`: +69/-9 lines (117 -> 177). Prose. States the backend is original but reproduces GE pixel semantics derived from PPSSPP through ge.c. | no SPDX line (prose file) | 4 |
| `rt/gpu_sdl3vk/ge_gpu.c` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `892107eea611`: +4209/-322 lines (1519 -> 5406). Header: original Vulkan backend, not a port of PPSSPP's GPU; GPLv2+ because it consumes ge.c. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-11 | 16 |
| `rt/gpu_sdl3vk/ge_gpu.h` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `a4efe959261c`: +186/-3 lines (29 -> 212). No PPSSPP text in the file; covered by the SAL063_CREDITS statement for the whole renderer directory. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 4 |
| `rt/gpu_sdl3vk/sdl3vk.c` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `1013be54e8bd`: +2246/-164 lines (493 -> 2575). Header: original implementation, GPLv2+ because it consumes ge.c. fbcap.c and fbcap.h were moved out of this file on 2026-10-08. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 18 |
| `rt/gpu_sdl3vk/sdl3vk.h` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `f733b795cc5c`: +156/-7 lines (54 -> 203). Header notes an earlier PPSSPP-GPU bridge that was removed. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 10 |
| `rt/gpu_sdl3vk/shaders/psp.frag` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `5843314d686e`: +105/-7 lines (103 -> 201). A fog-convention comment cites PPSSPP. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 2 |
| `rt/gpu_sdl3vk/shaders/psp.vert` | sal063 `da17b0e` | inherited (sal063); PPSSPP-derived semantics | unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `84c8cdbac995`: +5/-0 lines (27 -> 32). Identical to the sal063 file except for the added header lines. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line | 1 |

### E. sal063 lineage, PPSSPP only an oracle or comparison reference (inherited)

The sal063 derivation line is the only upstream declaration. PPSSPP is mentioned only as the trace oracle or as a behaviour comparison. No PPSSPP translation is claimed in these files. Whether sal063 itself translated anything in them is not established here.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `ref/cpu.h` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `222603df46a8`: +43/-6 lines (100 -> 137). Reference interpreter. PPSSPP is named as the trace oracle and for the r4k branch model. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 4 |
| `ref/interp.cpp` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `dc871393ab2c`: +205/-69 lines (411 -> 547). Reference interpreter. Comments say div and madd semantics match PPSSPP (behaviour comparison). | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 7 |
| `ref/interp.h` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `882d7680ba76`: +17/-1 lines (49 -> 65). No PPSSPP text. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 2 |
| `ref/run_elf.cpp` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `a1622e0fa7c0`: +290/-28 lines (138 -> 400). Replays the entry register state captured by the PPSSPP oracle. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 3 |
| `ref/selftest.cpp` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `810008215c67`: +543/-20 lines (218 -> 741). One comment cites PPSSPP Int_Break behaviour. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 4 |
| `rt/driver.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `7f4c661605b7`: +539/-36 lines (172 -> 675). Host shell. PPSSPP appears only as the reference trace source. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 16 |
| `rt/funcdiff.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `96a66c27a19a`: +59/-13 lines (117 -> 163). Function diff against the PPSSPP reference trace. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 4 |
| `rt/gui.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `cd51f0785c0a`: +638/-181 lines (316 -> 773). No PPSSPP text. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 19 |
| `rt/h264_mf.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `d24342a2ee7d`: +308/-129 lines (409 -> 588). Media Foundation H.264 path. No PPSSPP text. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 5 |
| `rt/iso.h` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `9dad400fabe2`: +20/-1 lines (13 -> 32). No PPSSPP text. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 1 |
| `rt/osk_win.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `4f72351ad3d9`: +31/-2 lines (93 -> 122). No PPSSPP text. A Win32 guard and comment were added after sal063, and the Nakagawa modification line was added with this manifest. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line | 2 |
| `rt/sched.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `be7d3785341e`: +4158/-195 lines (631 -> 4594). One comment cites a measurement against PPSSPP. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-11 | 41 |
| `rt/vfpu_fuzz.c` | sal063 `da17b0e` | inherited (sal063) | none declared; unknown (inherited via sal063 `da17b0e`) | vs sal063 base blob `9f7f33d4bd45`: +280/-10 lines (138 -> 408). No PPSSPP text. | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-08-10 | 5 |

### F. Project files that kept sal063's notices after a move

Created in this repository. The code moved out of a sal063-lineage file, so the new files keep that file's notices and its GPL-2.0-or-later identifier.

| Path (under `src/`) | Lineage | Classification | PPSSPP pin | Local modifications | Notice lines in the file header | Commits here |
| --- | --- | --- | --- | --- | --- | --- |
| `rt/fbcap.c` | moved from `gpu_sdl3vk/sdl3vk.c` (#769, 2026-10-08) | inherited (sal063), moved | none declared; unknown (inherited via sal063 `da17b0e`) | not in the sal063 tree. The file says it keeps the notices of the file it came from | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-10-08 | 1 |
| `rt/fbcap.h` | moved from `gpu_sdl3vk/sdl3vk.c` (#769, 2026-10-08) | inherited (sal063), moved | none declared; unknown (inherited via sal063 `da17b0e`) | not in the sal063 tree | SPDX GPL-2.0-or-later; psp-recomp authors copyright; sal063 derivation line; Nakagawa modification line 2026-10-08 | 1 |

## Related files that this manifest does not cover

- `assets/vfpu/*.dat` (15 files): PPSSPP data, byte-verified against PPSSPP commit
  `f0baf3ade7bcb6c86f0835962b36eb4e51559d8f` and recorded in
  `assets/vfpu/PROVENANCE.json` (licence field GPL-2.0-or-later). The same file
  lists each blob SHA. Classification `derived-data`.
- `rt/vfpu_tables.c` and `rt/vfpu_tables.h`: GPL-3.0-or-later, project-authored loaders
  for those tables. They contain no PPSSPP code.
- `rt/fp_convert.h`: GPL-3.0-or-later, project-authored. It cites PPSSPP commit
  `06d5847da382336dc93a73b5e5adcd23de2f305d` as behavioural precedent and says no
  implementation code was copied. Not an adaptation.
- `rt/atrac3p_bridge.c` and `rt/atrac3p_bridge.h`: say they are not a PPSSPP
  translation. FFmpeg provenance is in `src/rt/atrac3p/PROVENANCE.md`.
- `rt/mp3_bridge.c` and `rt/mp3_bridge.h`: GPL-3.0-or-later, project-authored frame
  wrapper over minimp3 (CC0, byte-identical, `src/rt/minimp3/PROVENANCE.md`). They contain
  no PPSSPP code.
- Tests and documents that compare behaviour against PPSSPP (for example
  `rt/hle_thread_selftest.c`, `rt/intr_conformance.h`, `rt/guest_printf.c`) cite
  it as an oracle and are GPL-3.0-or-later. They are not adaptations.
- sal063 files that are absent from this tree (for example `pgf.c`, `audio.c`,
  `iso.c`) are outside this manifest.
- `tools/*.py` files that carry GPL-2.0-or-later headers (the sal063 tool
  lineage) are outside this manifest, which covers `src/` only.

## Known gaps

1. The PPSSPP revision of every derived file is unknown, except the
   `sceMpeg.cpp` revision described under "Pins". It cannot be recovered from this
   repository or from the sal063 tree.
2. `rt/evf.h`: the header says PPSSPP-derived while the ledger says
   project-authored. Reconciling them needs the maintainer's trusted record.
3. `rt/osk_win.c` has a changed body (a Win32 guard and comment). It lacked the
   "Modified by Nakagawa Recomp contributors" line the `AGENTS.md` section 4
   modification-note rule asks for; this change adds it (resolved).
4. `rt/gpu_sdl3vk/README.md` is prose with no SPDX header; it is in the manifest
   because it states the renderer's GPL-2.0-or-later status.
5. No file-by-file measurement of how much of each file is translated from PPSSPP
   exists. Classifications rest on file headers, SAL063_CREDITS.txt and the ledger.
6. `rt/hle.c` is 24,000 lines, of which about 23,000 were added after the sal063
   base. How much of that is PPSSPP-derived rather than project-authored or
   PSP-measured is not established; the file-level label is conservative.

## How this manifest was produced

- The file list is `git grep -l "GPL-2.0-or-later" -- src` at the base commit
  of this change (34 files).
- Lineage and notice columns come from each file's first lines, from
  `git log --follow`, from sal063's tree at `da17b0e` (read with the GitHub
  API: tree listing, blob SHAs and file contents), and from
  `assets/public_provenance_ledger.json`.
- Not verified: PPSSPP upstream sources themselves were not compared against
  any file. No similarity analysis was run.
- Regenerate the per-file numbers by repeating the steps above; the narrative
  columns need a person to re-read the headers.
