# Project status dashboard

**Dashboard reconciled:** 2026-08-11

> [!IMPORTANT]
> Nakagawa Recomp is an experimental compatibility/static-recompilation project, not an end-user release.
> Accepted development evidence reaches the title, main menu, 3D lobby, active tennis match, and returns to the club.
> PSMF video, remaining HLE/kernel semantics, archive/VFS fidelity, portability, performance, generalization,
> and publication/legal work remain open.
>
> Exact-head local verification (`hst_manager.ps1 -Action Verify`, focused selftests, publication audits, and pre-commit) is the current evidence standard.

## Tracking model

Public GitHub Issues are canonical for actionable work where a curated public issue exists. During ongoing public issue curation, this file serves as the concise status map linking to curated issues, open PRs, merged implementation evidence, and domain reference documents. Superseded investigations belong in [`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md) or the issue/PR discussion that produced the evidence. Strategy lives in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## At a glance

| Priority | State | Canonical work item | Type |
| --- | --- | --- | --- |
| P0 | Open | [Callable/resume continuation semantics](docs/issue-51-entry-semantics.md) | REFERENCE DOCUMENT |
| P1 | Open | [PSMF player video/audio output](docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md) | REFERENCE DOCUMENT |
| P1 | Closed | [Music/resource audio resolution](docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md) | REFERENCE DOCUMENT |
| P1 | Closed | [ATRAC3+ decoder architecture & provenance](src/rt/atrac3p/PROVENANCE.md) | REFERENCE DOCUMENT |
| P1 | Open | [sceSasCore state & mix semantics](docs/SAS_NID_SIGNATURES.md) ([sceSasCore PR #20](https://github.com/Jstar269/nakagawa-recomp/pull/20)) | MERGED PR / REF DOC |
| P1 | Open | [Direct XB archive VFS](docs/ISSUE196_DIRECT_XB.md) & [Scorecard portraits](docs/issue-139-face-resource-semantics.md) | REFERENCE DOCUMENT |
| P1 | Open | [#23 — PSP DMA copy semantics: validation, overlap, and measured transfer ceiling](https://github.com/Jstar269/nakagawa-recomp/issues/23) | OPEN ISSUE |
| P1 | Open | [Unified PSP clock domains & interrupt delivery](docs/PSP_INTR_WAITS_MATRIX.md) | REFERENCE DOCUMENT |
| P1 | Open | [Versioned title manifest & general toolkit boundary](assets/titles/README.md) | REFERENCE DOCUMENT |
| P2 | Open | [HST analyzer span leakage](docs/STATUS_HISTORY.md) | HISTORICAL EVIDENCE |
| P2 | Open | [Gameplay performance baselines](docs/PERFORMANCE.md) | REFERENCE DOCUMENT |
| P1 | Open | [PGF/JPCSP/intraFont provenance review](docs/PGF_LICENSE_REVIEW_PACKET.md) | REVIEW PACKET |
| P1 | In Progress | [PGF replacement campaign](docs/PUBLIC_SOURCE_PROFILE.md) | PROFILE DOCUMENT |
| P1 | Open | [PGD/amctrl distribution posture](docs/PGD_AMCTRL_REVIEW_PACKET.md) | REVIEW PACKET |

## Recent audio closure

The old statement “background music does not play” is no longer current.

The resolved chain is:

1. transactional ATRAC parsing and streaming bookkeeping;
2. direct FFmpeg n4.4 ATRAC3+ decoder and project-authored bridge;
3. title-stream decoder acceptance;
4. logical FIFO/ring-wrap repair at the title BGM boundary;
5. end-to-end acceptance through guest mixer → SAS → Output2 → runtime ring → SDL callback/device.

The pinned final acceptance run recorded all 2,282 ATRAC frames nonzero, 6,139/6,502 Output2 submissions
nonzero, zero clamps/drops/put failures, and more than 4.7 million nonzero frames delivered to the device.
Playback remained 98–100% nonzero across sustained BGM windows.

Closing audio blockers does **not** mean the whole PSP audio API is complete. Keep these independent owners
open until their own acceptance criteria are met:

- ATRAC/query/unsupported-API truthfulness beyond the proven title route ([`src/rt/atrac3p/PROVENANCE.md`](src/rt/atrac3p/PROVENANCE.md));
- ATRAC context/reinit/allocation lifecycle contract ([`src/rt/atrac3p/PROVENANCE.md`](src/rt/atrac3p/PROVENANCE.md));
- regular audio-channel configuration/volume/queue state ([`src/rt/atrac3p/PROVENANCE.md`](src/rt/atrac3p/PROVENANCE.md));
- remaining SAS state/mix semantics ([`docs/SAS_NID_SIGNATURES.md`](docs/SAS_NID_SIGNATURES.md), [sceSasCore PR #20](https://github.com/Jstar269/nakagawa-recomp/pull/20));
- PSMF player video/audio integration ([`docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md`](docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md)).

## Continuation semantics status

Current `main` contains two bounded narrowing campaigns:

- **Direct `j`:** 53 candidates → 28 proven interior continuations, 0 callable promotions, 25 ambiguous retained.
- **Direct conditional branch:** 224 candidates → 12 proven continuations, 32 callable controls, 180 ambiguous; only 10 new callable→resume promotions because two overlapped the first slice.

Both campaigns preserved fallback count and used conservative proof rules rather than address-specific guesses.
The remaining continuation population is intentionally not bulk-classified.

## Archive/VFS and visible fidelity

In-match scorecard portraits construct a face path under directory `00` while character files exist under a different extracted archive directory ([`docs/issue-139-face-resource-semantics.md`](docs/issue-139-face-resource-semantics.md)). The current model is that `00` is a mounted/archive slot rather than a literal global path component. [`docs/ISSUE196_DIRECT_XB.md`](docs/ISSUE196_DIRECT_XB.md) owns proving or refuting that model and selecting extraction/index/direct-archive architecture.

## Generalization / toolkit boundary

Toolkit generalization has landed a versioned bounded manifest schema, a source-owned synthetic fixture, an HST public manifest, and deterministic read-only codegen/manager planning ([`assets/titles/README.md`](assets/titles/README.md)). Remaining work is actual build/manager consumption with equivalence proof, a second PSPDEV/PSPSDK-built source-owned title fixture, and retirement of duplicated HST constants.

`tools/analyze.py::exec_ranges()` still falls back to the HST-only span for an unrelated image unless the caller explicitly supplies/clears `HST_EXTRA_SPANS`.

## Publication / governance status

This repository (`Jstar269/nakagawa-recomp`) is established as the sanitized public-safe repository export (`public-safe-v1`). The historical development repository remains separate outside the public boundary.

- **PGF Provenance ([`docs/PGF_LICENSE_REVIEW_PACKET.md`](docs/PGF_LICENSE_REVIEW_PACKET.md)):** PGF source archaeology is durable and function/block-level. Public-safe builds keep PGF excluded/fail-closed until the retained implementation receives qualified treatment or a clean replacement is proven.
- **PGD/amctrl Posture ([`docs/PGD_AMCTRL_REVIEW_PACKET.md`](docs/PGD_AMCTRL_REVIEW_PACKET.md)):** PGD/amctrl remains excluded from the public-safe profile. The PSP-specific flow is recorded as derived-translated, while AES and later hardening are independently expressed; the remaining blocker is qualified human legal review.
- **Upstream Credits ([`NOTICE.md`](NOTICE.md)):** sal063 and PPSSPP attribution engineering work has landed.
- **Contributor Rights ([`docs/DCO_POLICY.md`](docs/DCO_POLICY.md)):** Public-contributor rights attestation uses DCO 1.1 with explicit `Signed-off-by:` requirements.

A clean publication audit is technical evidence about the candidate tree; it is never legal clearance.

## Current public tracker

- [Issue #23 — PSP DMA copy semantics: validation, overlap, and measured transfer ceiling](https://github.com/Jstar269/nakagawa-recomp/issues/23) [OPEN ISSUE]
- [PR #1 — Bump ruff-pre-commit from v0.16.0 to 0.16.2](https://github.com/Jstar269/nakagawa-recomp/pull/1) [OPEN PR]

Recently merged/closed public work is preserved in the relevant implementation/reference documents and GitHub history rather than duplicated here. For example, inherited-file modification notices landed through merged PR #27 and closed issue #26.

## Updating this dashboard

- Update/create the canonical public GitHub issue first when a curated issue is appropriate.
- Prefer `unknown` or `open` over carrying forward a superseded claim.
- Keep current state here; preserve detailed historical narratives in issue/PR discussions or `STATUS_HISTORY.md`.
- Never call local-only checks CI-green.
- Never call a GUI defect fixed without rendered evidence.
- Never treat a successful build, root license, clean provenance ledger, or clean publication audit as legal clearance.
