# Project status dashboard

**Dashboard reconciled:** 2026-08-09

**Reconciliation baseline:** `d14933d1e0730525d13c0ac7aea856ab64e89312`

**Latest private-route evidence summarized here:** 2026-08-08

> [!IMPORTANT]
> Nakagawa Recomp is an experimental compatibility/static-recompilation project, not an end-user release.
> A deterministic private HST route reaches a live match, scores points, gives up, saves, returns to the
> club, and now has sustained audible title BGM. That does **not** establish complete PSP fidelity: PSMF
> video, remaining HLE/kernel semantics, archive/VFS fidelity, portability, performance, generalization,
> and publication/legal work remain open.
>
> Hosted GitHub Actions are intentionally unavailable for the remainder of the current quota period.
> Do not wait for hosted CI and do not describe local checks as CI-green. Exact-head local verification
> (`hst_manager.ps1 -Action Verify`, focused selftests, publication audits, and pre-commit) is the current
> evidence standard.

## Tracking model

GitHub Issues are canonical for actionable defects and acceptance criteria. This file is intentionally a
**short current map**, not a second issue archive. Superseded investigations belong in
[`docs/STATUS_HISTORY.md`](docs/STATUS_HISTORY.md) or the issue/PR discussion that produced the evidence.
Strategy lives in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## At a glance

| Priority | State | Canonical work item |
| --- | --- | --- |
| P0 | Open | [#51 — callable/resume continuation semantics](https://github.com/Jstar269/nakagawa-recomp/issues/51) — PR #336 narrowed the direct-`j` slice and PR #337 narrowed the direct conditional-branch slice; the much larger no-direct-edge population remains open and must stay evidence-driven |
| P1 | Open | [#31 — real PSMF player video/audio output](https://github.com/Jstar269/nakagawa-recomp/issues/31) — intro/player state exists but real player data production remains the major media gap |
| P1 | Closed | [#32 — music/resource audio resolution](https://github.com/Jstar269/nakagawa-recomp/issues/32) — closed 2026-08-09 after the original unresolved-resource hypothesis was not reproduced on the bounded route and the actual ATRAC/parser/ring/decode/output blockers were resolved; sustained BGM is proven end-to-end by PR #335 and `docs/AUDIO_OUTPUT_ACCEPTANCE_20260807.md` |
| P1 | Closed | [#286 — ATRAC3+ decoder architecture/provenance/acceptance](https://github.com/Jstar269/nakagawa-recomp/issues/286) — closed 2026-08-09; direct FFmpeg n4.4 decoder provenance, HLE bridge, source-owned regressions, private title acceptance, and downstream audio acceptance are all landed |
| P1 | Open | [#75 — sceSasCore state/mix semantics](https://github.com/Jstar269/nakagawa-recomp/issues/75) — BGM is no longer blocked; remaining work is genuine SAS semantics such as volume/ADSR/effect/state validation and timing |
| P1 | Open | [#196 — direct XB archive VFS / mounted-slot semantics](https://github.com/Jstar269/nakagawa-recomp/issues/196) and [#139 — missing scorecard portraits](https://github.com/Jstar269/nakagawa-recomp/issues/139) — resolve whether flattened extraction loses archive-slot identity; no path aliases/fallback hacks |
| P1 | Open | [#87 — DMA copy/busy/timing semantics](https://github.com/Jstar269/nakagawa-recomp/issues/87) and [#328 — unresolved 0xC000 ceiling evidence](https://github.com/Jstar269/nakagawa-recomp/issues/328) — hardware evidence is still required for the disputed boundary/queue contract |
| P1 | Open | [#80 — unified PSP clock domains](https://github.com/Jstar269/nakagawa-recomp/issues/80) and [#88 — pending interrupt delivery](https://github.com/Jstar269/nakagawa-recomp/issues/88) — foundational timing/interrupt correctness; avoid overlapping implementation with other scheduler-heavy campaigns without isolated worktrees |
| P1 | Open | [#197 — versioned title manifest/general toolkit boundary](https://github.com/Jstar269/nakagawa-recomp/issues/197) — schema, HST manifest, and deterministic read-only planning have landed; manager/build consumption, second source-owned PSP fixture, and retirement of duplicated constants remain |
| P2 | Open | [#151 — HST analyzer span leakage](https://github.com/Jstar269/nakagawa-recomp/issues/151) — still open: `tools/analyze.py` retains a base-zero HST span default when `HST_EXTRA_SPANS` is absent, so generic analysis is not yet fully profile-owned |
| P2 | Open | [#33 — gameplay performance](https://github.com/Jstar269/nakagawa-recomp/issues/33) — several measured renderer/GE wins are landed; correctness/generalization work currently outranks further optimization |
| P1 | Open | [#98 — PGF/JPCSP/intraFont provenance/publication](https://github.com/Jstar269/nakagawa-recomp/issues/98) — technical source archaeology is complete to recoverable public evidence; retain/exclude/replace and qualified legal treatment remain unresolved |
| P1 | Draft PR | [#339 — independently reimplemented PGF replacement campaign](https://github.com/Jstar269/nakagawa-recomp/pull/339) — long-lived engineering escape route; public-safe configurations continue to fail closed until a replacement is proven |
| P1 | Open | [#104 — PGD/amctrl distribution posture](https://github.com/Jstar269/nakagawa-recomp/issues/104) — technical provenance archaeology is complete to recoverable evidence and public-safe exclusion exists; qualified licensing/legal/anti-circumvention review remains |

## Recent audio closure

The old statement “background music does not play” is no longer current.

The resolved chain is:

1. transactional ATRAC parsing and streaming bookkeeping (#283);
2. direct FFmpeg n4.4 ATRAC3+ decoder and project-authored bridge (#315 ancestry);
3. private title-stream decoder acceptance (#320);
4. logical FIFO/ring-wrap repair at the title BGM boundary (#334);
5. end-to-end acceptance through guest mixer → SAS → Output2 → runtime ring → SDL callback/device (#335).

The pinned final acceptance run recorded all 2,282 ATRAC frames nonzero, 6,139/6,502 Output2 submissions
nonzero, zero clamps/drops/put failures, and more than 4.7 million nonzero frames delivered to the device.
Playback remained 98–100% nonzero across sustained BGM windows. The uncapped call census also proved the
previously suspicious ATRAC/SAS APIs were not called on this title BGM route.

Closing #32 and #286 does **not** mean the whole PSP audio API is complete. Keep these independent owners
open until their own acceptance criteria are met:

- [#38](https://github.com/Jstar269/nakagawa-recomp/issues/38) — residual ATRAC/query/unsupported-API truthfulness beyond the proven title route;
- [#69](https://github.com/Jstar269/nakagawa-recomp/issues/69) — ATRAC context/reinit/allocation lifecycle contract;
- [#70](https://github.com/Jstar269/nakagawa-recomp/issues/70) — regular audio-channel configuration/volume/queue state;
- [#75](https://github.com/Jstar269/nakagawa-recomp/issues/75) — remaining SAS state/mix semantics;
- [#31](https://github.com/Jstar269/nakagawa-recomp/issues/31) — PSMF player video/audio integration.

## #51 continuation-semantics status

Current `main` contains two bounded narrowing campaigns:

- **Direct `j`:** 53 candidates → 28 proven interior continuations, 0 callable promotions, 25 ambiguous retained.
- **Direct conditional branch:** 224 candidates → 12 proven continuations, 32 callable controls, 180 ambiguous; only 10 new callable→resume promotions because two overlapped the first slice.

Both campaigns preserved fallback count and used conservative proof rules rather than address-specific guesses.
The remaining continuation population is intentionally not bulk-classified. Hardware/PSPLink evidence may
help validate representative contracts, but absence of a hardware hit is not permission to guess.

## Archive/VFS and visible fidelity

[#139](https://github.com/Jstar269/nakagawa-recomp/issues/139) remains a concrete visible defect: the game
requests a face path under directory `00` while the matching character-100 files exist under a different
extracted archive directory. The current hypothesis is that `00` is a mounted/archive slot rather than a
literal global path component. [#196](https://github.com/Jstar269/nakagawa-recomp/issues/196) owns proving or
refuting that model and selecting extraction/index/direct-archive architecture. Do not introduce
filename-prefix rewrites or basename aliases as a shortcut.

## Generalization / toolkit boundary

[#197](https://github.com/Jstar269/nakagawa-recomp/issues/197) has already landed more than its original
issue body suggests: a versioned bounded manifest schema, a source-owned synthetic fixture, an HST public
manifest, and deterministic read-only codegen/manager planning. Remaining work is actual build/manager
consumption with equivalence proof, a second PSPDEV/PSPSDK-built source-owned title fixture, and only then
retirement of duplicated HST constants.

[#151](https://github.com/Jstar269/nakagawa-recomp/issues/151) is **not** resolved by that progress yet:
`analyze.py::exec_ranges()` still falls back to the HST-only `0x00303194..0x00306e24` span for an unrelated
base-zero image unless the caller explicitly supplies/clears `HST_EXTRA_SPANS`.

## Publication / governance blockers

The conservative publication architecture remains a **fresh sanitized public repository**, not making this
historical private repository public in place.

- [#98](https://github.com/Jstar269/nakagawa-recomp/issues/98): PGF source archaeology is now durable and
  function/block-level; exact upstream checkout remains unprovable. Initial public-safe builds should keep
  PGF excluded/fail-closed unless the retained implementation receives qualified treatment or #339 replaces it.
- [#104](https://github.com/Jstar269/nakagawa-recomp/issues/104): PGD/amctrl remains excluded from the
  conservative public-safe profile. The PSP-specific flow is now recorded as derived-translated, while
  AES and later hardening are independently expressed; the remaining blocker is qualified human review,
  not more AI originality claims.
- [#304](https://github.com/Jstar269/nakagawa-recomp/issues/304): sal063 attribution/CREDITS engineering work
  has landed, but qualified notice presentation remains a publication-review question.
- [#152](https://github.com/Jstar269/nakagawa-recomp/issues/152): public-contributor rights attestation and
  future public-repository workflow remain separate from the maintainer's current DCO waiver.

A clean publication audit is technical evidence about the candidate tree; it is never legal clearance.

## Other high-value open owners

- [#1](https://github.com/Jstar269/nakagawa-recomp/issues/1), [#13](https://github.com/Jstar269/nakagawa-recomp/issues/13), [#14](https://github.com/Jstar269/nakagawa-recomp/issues/14), [#74](https://github.com/Jstar269/nakagawa-recomp/issues/74), [#93](https://github.com/Jstar269/nakagawa-recomp/issues/93): callback/wait and kernel-object transaction semantics.
- [#77](https://github.com/Jstar269/nakagawa-recomp/issues/77), [#78](https://github.com/Jstar269/nakagawa-recomp/issues/78), [#83](https://github.com/Jstar269/nakagawa-recomp/issues/83), [#86](https://github.com/Jstar269/nakagawa-recomp/issues/86), [#90](https://github.com/Jstar269/nakagawa-recomp/issues/90), [#91](https://github.com/Jstar269/nakagawa-recomp/issues/91): retained system/utility/display/power/registry state and dialog correctness.
- [#84](https://github.com/Jstar269/nakagawa-recomp/issues/84), [#92](https://github.com/Jstar269/nakagawa-recomp/issues/92): module and thread lifecycle truthfulness.
- [#148](https://github.com/Jstar269/nakagawa-recomp/issues/148), [#195](https://github.com/Jstar269/nakagawa-recomp/issues/195): writable VFS identity and lawful private-workspace/bootstrap correctness.
- [#54](https://github.com/Jstar269/nakagawa-recomp/issues/54): full Linux/Steam Deck runtime path.
- [#179](https://github.com/Jstar269/nakagawa-recomp/issues/179): exhaustive audit coverage ledger; continue incrementally rather than blocking focused correctness campaigns.

## Updating this dashboard

- Update/create the canonical GitHub issue first.
- Prefer `unknown` or `open` over carrying forward a superseded claim.
- Keep current state here; preserve detailed historical narratives in issue/PR discussions or `STATUS_HISTORY.md`.
- Never call local-only checks CI-green.
- Never call a GUI defect fixed without rendered evidence.
- Never treat a successful build, root license, clean provenance ledger, or clean publication audit as legal clearance.
