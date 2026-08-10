# Publication-lane verification — 2026-08-06

**Status: engineering evidence for qualified review, not legal advice or legal clearance.** This
report verifies the completed #99/#102 work, records the fresh sanitized-public-repository
validation, and itemizes the export-tooling defects found and fixed during this pass. Repository
visibility is unchanged; no legal clearance is claimed.

## 1. Issue-state reconciliation (canonical GitHub, 2026-08-06)

| Issue | State | Notes |
| --- | --- | --- |
| #98 PGF/JPCSP GPLv3 provenance | OPEN | Implementation excluded by `public-safe-v1`; packet updated this pass |
| #99 Replacement PGF font licenses | **CLOSED** | Closed as documentation completed; all PGFs remain excluded from the public-safe profile |
| #102 Full-history secret/proprietary/privacy audit | **CLOSED** | Closed via PR #300 (merge `69a3c4f`); residual key scrub + history plan still mandatory before any historical exposure |
| #103 Upstream notice inventory | CLOSED | Evidence retained for final review |
| #104 PGD/amctrl distribution review | OPEN | Implementation excluded by `public-safe-v1`; packet updated this pass |
| #149 Reproducible SBOM/license manifest | **CLOSED** | SPDX 2.3 + SPDX 3.0.1 + CycloneDX 1.5 generator/verifier implemented |
| #154 Complete public-tree manifest gate | **CLOSED** | Exhaustive candidate-tree manifest gate implemented |
| #27 OSPS/GitHub governance | CLOSED | Owner/settings checks still require live repository verification |
| #304 sal063 CREDITS not carried (PROV-F6) | OPEN | Acceptance criteria closed by PR #306 merge `9f6f5b0`; issue closure is a maintainer decision |
| #306 IND-6 NOTICE inventories PR | **MERGED** | `THIRD_PARTY_LICENSES/SAL063_CREDITS.txt` + completed NOTICE inventories on `main` |

## 2. #99 verification — replacement PGF font licenses (CLOSED)

Closure evidence (issue comments + tree): byte identity of the four PGFs revalidated against PPSSPP
`f0baf3ad…` (`font/README.md` blob IDs); family-level source metadata established from PPSSPP history
(Source Han Sans for the jpn0/kr0 line, Ume Hy Gothic for even ltn0/ltn8); the compatibility names
embedded by PPSSPP are explicitly recorded as **not** evidence of Sony/Fontworks outlines; and every
`font/*.pgf` is excluded from the `public-safe-v1` candidate (`assets/public_source_profile.json`,
`exclude_globs: ["font/*.pgf"]`).

Residuals that remain true despite closure: the exact source TTF/release used to generate the current
blobs, the complete manual-edit chain, and a transformed-blob notice are **unknown**. Because the
fonts stay excluded from the initial public source, these residuals do not block the conservative
release plan; they block any later configuration that redistributes the PGF binaries. No font was
relabeled or cleared.

## 3. #102 verification — full-history audit (CLOSED)

Closure evidence: PR #300 (merge `69a3c4f`) recorded a non-destructive audit across 680 reachable
commits, 4,599 objects, and 121 refs with **0 sensitive findings** under measured scope, plus the
orphan `d9d5484` scan (Gitleaks 8.30.1, `--all --reflog`).

Re-measurement at this head (2026-08-06, local): `tools/history_audit.py --json` reports **0 findings
across 766 commits and 5,178 objects** (146 refs); the 12 largest blobs (>500 KB) are inventoried with
no findings. `tools/verify_key_scrub.py` still exits **3** — the known PSP KIRK/amctrl constants
remain reachable in old Git history. That is expected and documented: the public repository is built
as a **fresh sanitized single-commit export** (validated below), never by exposing this history.
`docs/KEY_HISTORY_SCRUB.md` remains the coordinated runbook for the private archive itself; it must
not be run standalone and was not run this pass.

## 4. Fresh sanitized-public-repository validation (end-to-end, 2026-08-06)

Full run of the documented lane with the corrected tooling:

1. `python tools/build_public_export.py --verify-only --public-safe-profile` → 4/4 gates pass:
   - Legal Blockers (public-safe profile active) — PASS
   - Publication Audit `--tracked-only` — **OK (622 tracked files, 0 findings)**
   - Full-History Audit — **0 findings (766 commits, 5,178 objects)**
   - SBOM Verification — OK (release locks + dashboard toolchain compatibility)
2. `python tools/build_public_export.py --export-dir … --public-safe-profile` →
   - 15 unresolved paths excluded (4 fonts, pgf.c/h, pgd.c/h, pgd tools/tests)
   - `PUBLIC_EXPORT.json` provenance metadata written into the export (profile `public-safe-v1`,
     profile SHA-256 `90b28206…581`, source commit `dd0bcaea`, 607 exported / 15 excluded)
   - Post-export Candidate-Tree Audit (`--candidate-tree --public-scope`) — **OK (608 files, 0 findings)**
   - **[EXPORT CLEARED]**
3. `mingw32-make public-safe-verify` inside the export — **exit 0**: the exported generic source
   boundary compiles with the fail-closed unavailable backends (`PUBLIC_SAFE=1`).
4. Unit coverage: `tools/test_public_export.py` + `tools/test_publish_audit.py` — **37 tests OK**.

The exported tree is a buildable generic recompiler/runtime that requires users to supply lawful
inputs locally. It is not a game-specific binary and contains no retail/generated/private material.

## 5. Tooling defect ledger (found and fixed this pass)

| Defect | Fix | State |
| --- | --- | --- |
| `tools/psp_oracle/verify_vfpu_addr.py` missing SPDX header → FAST audit + export gate failed | Added `GPL-2.0-or-later` header | Merged (`1dd9676`) |
| `build_public_export.py --public-safe-profile` did **not** apply the exclusion profile (archived HEAD wholesale); metadata absent; no post-export audit | Export now filters `exclude_paths`/`exclude_globs` via the shared `public_candidate` profile, writes `PUBLIC_EXPORT.json`, runs the candidate-tree audit gate, and commits with a neutral synthetic identity (no reliance on a global git config) | On `origin/main` |
| `publish_audit` candidate scan walked the export's own `.git/` internals → `MAGIC_UNKNOWN` findings | `_get_filesystem_entries` prunes `_VCS_METADATA_DIRS` (`.git`/`.hg`/`.svn`) | Recovered 2026-08-10 into `publication/recover-unique-evidence-20260810` (PR pending); regression tests added in `tools/test_publish_audit.py` |
| `publish_audit` orphan check flagged manifest components with `public_scope_included: false` as `MANIFEST_ORPHAN_PATH` in public-scope candidates | Orphan check honors `public_scope_included: false` under `public_scope=True` | Recovered 2026-08-10 into `publication/recover-unique-evidence-20260810` (PR pending); regression tests added in `tools/test_publish_audit.py` |
| `tools/test_public_export.py` standard-export test asserted no `PUBLIC_EXPORT.json`, contradicting the generator at the time (wrote metadata for every export) | Drafted test updated to assert metadata with `profile: "standard"` | **NOT recovered.** Current `main` makes metadata conditional on an applied profile (`build_public_export.py` writes `PUBLIC_EXPORT.json` only when `profile is not None`, so a standard export stays byte-identical to the tracked index), and `test_public_export.py` already asserts the profile-only metadata contract. The superseded standard-profile expectation was rejected by adjudication |

The first two fixes (files: `tools/publish_audit.py`, `tools/test_publish_audit.py`) are required for the
candidate-tree audit to pass. They were recovered and committed on 2026-08-10 from this stash record
(adjudicated hunk-by-hunk; the stash was **not** applied wholesale), and are pending review as a
focused draft PR. The third item above was intentionally left out of that PR.

## 7. Wiki corpus integration (PSPRecompWiki sections 06/07)

Consulted the local research corpus the user pointed this lane at
(PSPRecompWiki, docs 60/61/63/64/77/78/90, extracted to text for review
2026-08-06). How each document informs the lane:

| Wiki doc | Evidence contributed | Folded into |
| --- | --- | --- |
| 60 — Provenance, Independence, Licensing Boundaries, and Publication Engineering | 50-point publication framework: public-safe component profile (§35), fail-visible unavailable backends (§35), candidate-tree audit (§31), history audit with measured scope (§32), unreachable-object caveat (§33), fresh sanitized repository (§34), indexed-blob audit source of truth (§30) | Confirms the lane architecture; §30 explains the `verify_vfpu_addr.py` SPDX incident (the audit reads the Git index blob, not the worktree) |
| 90 — PSP Security and Protected-Content Architecture | Protection-layer taxonomy; the fixed-key BBMac/BBCipher flow sits in the savedata/content-authentication-services layer; public-safe documentation include/exclude lists; "understand ≠ reproduce/distribute/bypass" core rule | PGD/amctrl packet (#104) supporting context |
| 61 — Prior Art and Source Atlas | PPSSPP/JPCSP/intraFont citation family; source-lineage-graph recommendation (immediate vs ultimate provenance); PSP-Archive-as-mirror warning | PGF packet (#98) supporting context |
| 63 — Evidence Conflict and Contradiction Registry | C-020 (reachable-history clean ≠ no unreachable hosting objects → fresh repo); C-028 (successful protected-content transform ≠ redistribution/publication permission); C-012 (sal063 retention supersedes earlier independence claims); C-011 (NID derivability vs name provenance) | Architecture rationale for the fresh-repo plan; #104 exclusion posture; IND-1/IND-4 evidence |
| 64 — Source Preservation | Revision/blob pinning; "first visible in available history" phrasing; recovery search order | PGF packet (#98) methodology |
| 77 — Legacy Research Intake Audit | KEEP/REVERIFY/CORRECT/REJECT classification; `legacy_source` record fields | Methodology reference; not yet applied to repository docs (out of scope this pass) |
| 78 — Machine-Readable Wiki Claim Schema | Publication classification vocabulary (`public-source-owned`, `private-key-secret`, `qualified-review-required`, `do-not-publish`); static-site export pipeline ending in a publication audit | Vocabulary referenced by the packets and the matrix |

No wiki document changes any disposition outcome: #98/#104 remain OPEN with the `public-safe-v1`
profile excluding the components; #99/#102 remain CLOSED as previously verified. The wiki corpus is
research/editorial material, not legal advice and not repository source of truth.

## 8. Residuals and next steps

- The two `publish_audit` hardening fixes above were recovered and committed on 2026-08-10
  (branch `publication/recover-unique-evidence-20260810`); the third drafted fix was rejected and
  left behind. See the defect ledger above.
- #98 and #104 remain OPEN: qualified human review of the **actual** candidate tree/build
  configuration is required before any retaining configuration or visibility change. Do not change
  repository visibility on the strength of this report.
- `ISSUES.md` publication rows for #99/#149/#154/#27 (and the #102 baseline) were reconciled by the
  later tracker-reconcile pass (PR #340).
- DCO (#152) is explicitly out of scope for this lane.
