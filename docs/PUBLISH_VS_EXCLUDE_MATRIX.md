# Publish-vs-exclude matrix — fresh public repository candidate

**Status: engineering evidence for qualified review, not legal advice or legal clearance.** This
matrix is the file/component disposition map for the conservative initial public-source plan: a
**fresh sanitized public repository** built from the current tree with the unresolved
`#98`/`#99`/`#104` components excluded. It is generated from and must stay consistent with the
machine-readable sources of truth listed below; do not hand-edit a disposition here without
changing the corresponding source of truth.

- Exclusion profile (machine): [`assets/public_source_profile.json`](../assets/public_source_profile.json) (`public-safe-v1`)
- Release component manifest: [`assets/release_manifest.json`](../assets/release_manifest.json)
- Publication audit (machine): `tools/publish_audit.py` FAST (`--tracked-only`) and EXHAUSTIVE (`--candidate-tree --public-scope`)
- History audit: `tools/history_audit.py`; key scrub reachability: `tools/verify_key_scrub.py`
- Fresh export generator: `tools/build_public_export.py --public-safe-profile`

Disposition vocabulary:

- **PUBLISH** — included in `public-safe-v1`; no unresolved provenance blocker; notices as recorded in
  `NOTICE.md` / `THIRD_PARTY_LICENSES/` must be preserved.
- **EXCLUDE** — excluded from the initial public source by `public-safe-v1` pending qualified review
  (#98/#99/#104). Absence is the default; do not explain away with disclaimers.
- **CAVEAT-PUBLISH** — included, but a documented attribution caveat remains (recorded in `NOTICE.md`);
  no action changes the disposition without an owner decision.
- **NEVER-SCOPE** — private/game/generated material that must never enter the public tree, history,
  issues, or releases; excluded by `.gitignore`, `publish_audit` forbidden-path rules, and the
  fresh-repository construction procedure.

## Core recompiler toolkit — PUBLISH

| Component | Paths | License / notice |
| --- | --- | --- |
| Codegen/analysis pipeline | `tools/codegen.py`, `tools/imports.py`, `tools/analyze.py`, `tools/codegen_gate.py`, `tools/microtest_gate.py`, `tools/gen_microtest.py`, `tools/prxload.py`, `tools/nidseq.py`, `tools/funcdiff_cmp.py`, `tools/elf_bounds.py`, `tools/psp_import_table.py`, `tools/gen_nidnames.py`, `tools/vfpu_synth_gen.py`, `tools/vfpu_fuzz_gen.py`, `tools/padscript_from_log.py`, `tools/tracediff.py`, … | GPL-2.0-or-later; sal063-derived (see `NOTICE.md` sal063 inventory) |
| Runtime core | `src/rt/recomp.c`, `src/rt/recomp.h`, `src/rt/sched.c`, `src/rt/sr_coro.c`, `src/rt/hle.c`, `src/rt/nid_names.h`, `src/rt/driver.c`, `src/rt/iso.c`, `src/rt/iso.h`, `src/rt/funcdiff.c`, `src/rt/savedata.c`, `src/rt/evf.h`, `src/rt/ge.c`, `src/rt/ge_shared.h`, `src/rt/gui.c`, `src/rt/vfpu_interp.c`, `src/rt/vfpu_tables.c`, `src/rt/vfpu_fuzz.c`, `src/rt/watchpoints_file.c`, `src/rt/debug.c`, … | GPL-2.0-or-later; PPSSPP-derived (see `NOTICE.md` PPSSPP inventory) |
| Renderer | `src/rt/gpu_sdl3vk/` (`sdl3vk.c/h`, `ge_gpu.c/h`, shaders `psp.vert`/`psp.frag` + embedded SPIR-V) | GPL-2.0-or-later; GLSL originates sal063/upstream |
| Reference interpreter | `src/ref/` (`interp.cpp`, `interp.h`, `cpu.h`, `run_elf.cpp`, `selftest.cpp`) | GPL-2.0-or-later; sal063-derived (69–94% shared) |
| Media/HLE support | `src/rt/mpeg.c`, `src/rt/h264_mf.c` (Windows Media Foundation system component; no MS redistributable included), `src/rt/h264_null.c`, `src/rt/osk_win.c` | GPL-2.0-or-later |
| VFPU lookup data | `assets/vfpu/*.dat` + `assets/vfpu/PROVENANCE.json` | PPSSPP-origin GPL-2.0-or-later data; byte-verified vs `f0baf3ad`; see `assets/README.md` |
| ATRAC3+ decoder | `src/rt/atrac3p/` — byte-exact FFmpeg n4.4 imports (`libavcodec/`, `libavutil/`) + Nakagawa wrapper/selftest | LGPL-2.1-or-later (FFmpeg-derived); per-file ledger in `src/rt/atrac3p/PROVENANCE.md`; byte-identity must not be broken by header edits |
| Fixtures | `fixtures/` (synthetic/homebrew only; no retail bytes) | GPL-2.0-or-later |
| Test/tooling | `tools/test_*.py`, `tools/hst_*.py/ps1`, `tools/publish_audit.py`, `tools/build_public_export.py`, `tools/public_candidate.py`, `tools/history_audit.py`, `tools/verify_key_scrub.py`, `tools/generate_sbom.py`, `tools/verify_sbom.py`, … | GPL-2.0-or-later |
| Dashboard | `interface/` (Next.js) | Project-authored; JS dependency licenses recorded by `interface/package-lock.json`; shadcn/ui MIT notice in `THIRD_PARTY_LICENSES/SHADCN_UI.txt` |
| Build/docs/governance | `Makefile`, `mk/`, `pyproject.toml`, `copy_build_assets.ps1`, `hst*.ps1`, `docs/`, `.github/workflows/` (full-SHA pinned), `AGENTS.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, `DEDICATION.md` | GPL-3.0-or-later project declaration; per-file SPDX as marked |
| Required publication files | `LICENSE`, `NOTICE.md`, `README.md`, `AGENTS.md`, `THIRD_PARTY_LICENSES/` (`PPSSPP_FONTS.txt`, `PSPSDK.txt`, `SAL063_CREDITS.txt`, `SHADCN_UI.txt`) | Repository-level GPL-3.0-or-later declaration is **not** component clearance |

## CAVEAT-PUBLISH

| Component | Paths | Caveat |
| --- | --- | --- |
| `src/rt/audio.c` | `src/rt/audio.c` | sal063 lineage in header; body cites no PPSSPP; sal063's own CREDITS does **not** list it, so the PPSSPP attribution is uncorroborated (see `NOTICE.md`). Retained deliberately; no disposition change without owner decision. |
| `src/rt/ge_shared.h` | `src/rt/ge_shared.h` | 100% sal063-derived and sal063's GE state is itself PPSSPP-derived — a chain, recorded in `NOTICE.md`. |
| `tools/nid_corpus.json` | `tools/nid_corpus.json` | Names PPSSPP-sourced; numeric NIDs recomputed (`sha1(name)[0:4]`) since IND-1; no independence claim. |

## EXCLUDE — `public-safe-v1` (pending qualified review)

Exact machine list in `assets/public_source_profile.json`:

| Path | Reason | Issue |
| --- | --- | --- |
| `font/jpn0.pgf`, `font/kr0.pgf`, `font/ltn0.pgf`, `font/ltn8.pgf` | Redistributed font binaries; exact source TTF/release and transformed-blob notices unproven | #98/#99 |
| `src/rt/pgf.c`, `src/rt/pgf.h` | PGF parser/rasterizer with PPSSPP/JPCSP/intraFont lineage dispute | #98 |
| `src/rt/pgd.c`, `src/rt/pgd.h` | PGD/amctrl implementation; qualified distribution review required | #104 |
| `tools/pgd_decrypt.py`, `tools/pgd_e2e_harness.c`, `tools/pgd_test_keys.py` | PGD standalone implementation/harness/key tooling | #104 |
| `tools/test_pgd_c.py`, `tools/test_pgd_decrypt.py`, `tools/test_pgd_hardening.py`, `tools/test_pgd_malformed.py` | PGD implementation tests | #104 |

The excluded backends are replaced by the fail-closed `pgf_unavailable.c`/`pgd_unavailable.c` stubs
(`PUBLIC_SAFE=1`); they fabricate nothing. The export build `public-safe-verify` compiles with these
stubs (validated 2026-08-06, exit 0).

## NEVER-SCOPE (private/game/generated — never in the public tree or history)

`build/`, `logs/`, `memstick/`, `fs/`, `oracle/`, `place_game_here/`, `original_game/`,
`docs/opengrip_ref/`, `third_party/ghidra/{exports,projects}/`, `keys/` (`pgd_keys.txt`),
`capture_branch.diff`-style local artifacts, and any file with a prohibited extension
(`.elf`, `.iso`, `.prx`, `.pbp`, `.bin`, `.gim`, `.at3`, `.pmf`, `.vag`, `.trace`, …) or name
(`reference_hashes.json`, `vfpu_words.txt`, `nidseq_mine.txt`, `EBOOT.BIN.dec*`, generated
`*_recomp_*.c`). These are enforced by `.gitignore`, `publish_audit` forbidden-path rules, and the
fresh-repository construction procedure. Old history still containing PSP KIRK/amctrl constants is
**not** part of the public repository (see below).

## History and refs — fresh repository only

- `tools/verify_key_scrub.py` currently exits **3** (known KIRK/amctrl constants still reachable in
  old history). The public repository must be constructed from an approved **sanitized tree** — never
  by changing this repository's visibility and never by pushing old refs.
- Local ref inventory (2026-08-06): 146 refs across local heads/remotes/tags/archive refs; the
  export generator materializes **one** single-commit history (`git archive HEAD` + fresh `git init`).
- `docs/KEY_HISTORY_SCRUB.md` remains the coordinated runbook for sanitizing the private archive
  itself; the fresh-public-repository architecture does not depend on it for public cleanliness.

## Reproducing the disposition (validated 2026-08-06)

```powershell
# FAST tripwire on the source tree
python tools/publish_audit.py --tracked-only

# Fresh public export: gates -> profile-filtered tree -> candidate-tree audit -> metadata
python tools/build_public_export.py --export-dir <staging>\public_repo --public-safe-profile

# Independent materialization (alternative path)
python tools/public_candidate.py <staging>\nakagawa-public --ref HEAD

# Build the exported generic source boundary (fails closed without PGF/PGD backends)
mingw32-make -C <staging>\public_repo public-safe-verify
```

Validated result (2026-08-06, export from `dd0bcaea`): 4/4 pre-publication gates pass; 15 paths
excluded; candidate-tree audit OK (608 files, 0 findings); `public-safe-verify` compiles exit 0.
`PUBLIC_EXPORT.json` records profile digest `90b28206…581` and the excluded path list in the export.

## Supporting research corpus

The project's local research corpus (PSPRecompWiki, sections 06/07 — docs 60, 61, 63, 64, 77, 78, 90)
was consulted for this matrix. Doc 60's publication framework and doc 90's protected-content
architecture informed the disposition vocabulary and the #98/#104 packet context; doc 78's
publication classification vocabulary (`public-source-owned`, `private-key-secret`,
`qualified-review-required`, `do-not-publish`) maps onto this matrix's disposition classes. Doc 63
entries C-020 (fresh sanitized repository) and C-028 (protected-content transformation vs
publication permission) are the evidence-registry rationale for the NEVER-SCOPE and EXCLUDE columns.
The wiki is research/editorial material, not legal advice and not a source of truth that overrides
`assets/public_source_profile.json` or `assets/release_manifest.json`.

## Claims boundary

This matrix is a component disposition for qualified review of the **actual candidate tree**. It is
not a determination that any component is license-clean, that the combined GPL presentation is
settled, that history is sanitized, or that source or binary distribution is cleared. Repository
visibility must not change on the strength of this matrix. DCO sign-off policy is out of scope for
this lane.
