# Publication readiness — thresholds, sequencing, and checklists

**Status: engineering assessment, not legal advice.** This is the operational counterpart to [`OSPS_BASELINE.md`](OSPS_BASELINE.md), [`LEGAL_REWRITE_ASSESSMENT.md`](LEGAL_REWRITE_ASSESSMENT.md), [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md), and [`NOTICE.md`](../NOTICE.md). Legal questions are gates for qualified review, not conclusions made by this file.

> [!IMPORTANT]
> This repository is the **sanitized public-source repository** (`public-safe-v1`); the historical development repository remains **private**. Publishing this source does not authorize distributing HST game binaries, recompiled game-derived C, or extracted game assets. Do not publish a binary release while the blockers below remain unresolved. The PGF and PGD/amctrl implementations are **excluded** from `public-safe-v1` and build against fail-closed backends; the open component questions below are unchanged by this source release. Runtime correctness or green local tests are not evidence that publication is legally safe.

## Three distinct thresholds — do not collapse them

1. **Generic source is publishable.** The recompiler tooling + generic runtime + synthetic/homebrew tests + only provably redistributable assets can be public. This is gated by provenance, legal review, security/repository hygiene, and history/privacy work — not by perfect PSP emulation.
2. **The runtime is PSP-faithful.** A separate engineering question measured against hardware/PPSSPP/autotest oracles.
3. **A game-specific binary is distributable.** A materially higher bar. Recompiled HST C chunks, `*_image.bin`, private oracle material, extracted game assets, decrypted PRXs and any binary embedding game code/data are **not** part of the initial public-source plan.

The lowest-exposure public architecture is therefore a generic recompiler/runtime that requires users to supply their own lawful inputs locally.

## Current publication gates

| Gate | Canonical item | Status |
| --- | --- | --- |
| PGF/JPCSP/intraFont provenance | [`PGF_LICENSE_REVIEW_PACKET.md`](PGF_LICENSE_REVIEW_PACKET.md) | Open; implementation is excluded by the public-safe profile |
| Exact licenses/notices for replacement PGF fonts | [`THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt`](../THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt) | Open; all PGFs are excluded by the public-safe profile |
| Full reachable-history secret/proprietary/privacy audit | [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md), [`tools/history_audit.py`](../tools/history_audit.py) | Closed 2026-08-06; full-history audit completed across 680 reachable commits and 4,599 objects with 0 sensitive findings found under measured scope |
| Upstream copyright/notice inventory | [`NOTICE.md`](../NOTICE.md) | Closed 2026-07-23; evidence still belongs in final review |
| Qualified PGD/amctrl distribution review | [`PGD_AMCTRL_REVIEW_PACKET.md`](PGD_AMCTRL_REVIEW_PACKET.md), [`provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md`](provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md) | Open; technical provenance is complete to recoverable evidence and the implementation is excluded by the public-safe profile |
| Reproducible release manifest & SBOM | [`assets/release_manifest.json`](../assets/release_manifest.json) | Closed 2026-08-06; SPDX 2.3, SPDX 3.0.1 JSON-LD, CycloneDX 1.5 SBOM generator (`tools/generate_sbom.py`), verifier (`tools/verify_sbom.py`), and python lock (`tools/requirements-lock.txt`) implemented |
| Contributor rights-attestation policy (DCO 1.1) | [`docs/DCO_POLICY.md`](../docs/DCO_POLICY.md) | Open/Partial (DCO 1.1 policy document, `CONTRIBUTING.md`, PR template, bot policy, and sign-off correction runbooks implemented; final terms gated on PGF/PGD review) |
| Complete public-tree manifest gate | [`tools/publish_audit.py`](../tools/publish_audit.py) | Closed 2026-08-06; exhaustive candidate-tree manifest gate implemented; passing output is engineering evidence for qualified review, not legal clearance |
| Live GitHub/OSPS governance review | [PR #27](https://github.com/Jstar269/nakagawa-recomp/pull/27), [`docs/OSPS_BASELINE.md`](../docs/OSPS_BASELINE.md) | Closed 2026-08-06; OSPS Level 1 baseline re-audited against empirical live GitHub API evidence with 5-state control schema |
| KIRK/amctrl constants reachable in old Git history | [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md) | Mandatory before any historical repository is exposed |

### Licensing posture

The repository-level project declaration is **GPL-3.0-or-later**, as reflected by `LICENSE`,
`assets/release_manifest.json`, and the dashboard package metadata. Individual files and inherited
components may retain GPL-2.0-or-later or other upstream-specific terms; the project declaration
must not be read as a substitute for component-level provenance and license review.

- Most project-authored and PPSSPP-derived source is marked **GPL-2.0-or-later**. That does **not** settle the combined-work license while PGF license review is open. PPSSPP's PGF source explicitly records JPCSP lineage and warns that copied portions make the file effectively GPLv3. Any public combined-work presentation must satisfy the applicable stronger terms if GPL-3.0-or-later portions are included; do not promise GPLv2 as an available option for that combined configuration merely because other files say `GPL-2.0-or-later`.
- JPCSP's font implementation credits BenHur's intraFont, whose archived source is CC BY-SA 3.0. Creative Commons currently lists no non-CC license as compatible with BY-SA 3.0; GPLv3 compatibility is a BY-SA **4.0** mechanism. This makes the source-comparison question in PGF provenance review material: determine whether protectable intraFont expression actually flowed downstream or whether the commonality is functional PSP format/API information. Do not assume either outcome.
- Byte provenance is not license provenance. A binary being present in PPSSPP does not automatically place that binary under PPSSPP's program license.
- The four PGFs and their parser/rasterizer are excluded by [`public-safe-v1`](PUBLIC_SOURCE_PROFILE.md). Its unavailable backend fails visibly; it does not revive the retired synthetic-font fallback.
- PGD/amctrl is separately sensitive. The private development tree still compiles it, while `public-safe-v1` excludes the implementation/tools and builds a fail-closed unavailable backend. Qualified review remains required for any later source or binary configuration that retains PGD/amctrl.

## Recommended publication topology: fresh sanitized public repository

Do **not** make the current historical development repository public by merely changing its visibility.

GitHub documents that a private→public visibility change exposes the code and **Actions history/logs** and disables push rulesets. History rewrites/force-pushes also do not provide the same cleanliness guarantee as constructing a new repository; old SHA references, PR refs, forks/clones, cached views, and issue comments can preserve references to removed history.

The conservative plan is:

1. Keep the historical development repository private as the archive.
2. Finish PGF and PGD provenance review (see [`PGF_LICENSE_REVIEW_PACKET.md`](PGF_LICENSE_REVIEW_PACKET.md) and [`PGD_AMCTRL_REVIEW_PACKET.md`](PGD_AMCTRL_REVIEW_PACKET.md)) and governance review.
3. Perform the one coordinated private-history rewrite required by [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md) if the archive itself is to be retained in sanitized form.
4. Maintain the **sanitized public repository (`public-safe-v1`)** as an explicitly approved public tree/history.
5. Push only the approved public `main` and intentionally approved tags. Do not migrate archive refs, old PR refs, Actions history, private issue comments, private oracle material, game-derived artifacts, or orphan objects.
6. Curate/recreate only currently useful public issues with private/game-derived evidence summarized rather than copied.
7. Configure rulesets/security settings on the public repository before accepting contributions.

This is a risk-minimization architecture, not a statement that publication is otherwise unlawful.

As of 2026-08-10 this topology is in place: the repository you are reading is the fresh sanitized public repository built from an approved `public-safe-v1` tree, and the historical development repository remains private with its history unchanged. Step 6 (public issue curation) is still outstanding.

## History audit and rewrite — audit first, rewrite once

The known PSP KIRK/amctrl constants are absent from the current tree but remain in old history. Sequence the cleanup:

1. Freeze history-changing development work.
2. Complete full-history audit across every reachable ref/object/metadata record.
3. Decide whether personal author email, AI-session URLs, private paths and other metadata are acceptable; add every removal to one specification.
4. Generate **one combined** `git filter-repo` specification (known constants + everything else found).
5. Rewrite once; verify key scrub + generic secret/proprietary scans + intended tip-tree equality; fresh-clone the result.
6. Repair SHA references in private documentation as needed.

For the public repository, prefer creation from the approved sanitized result rather than relying on old-object deletion semantics.

## Public-source-ready checklist

### Legal / provenance

- [x] Initial-source fallback implemented: affected PGF implementation/assets excluded and audited by `public-safe-v1`; substantive issues remain open for any retaining configuration.
- [x] Initial-source fallback implemented: PGD/amctrl implementation excluded and audited by `public-safe-v1`; qualified review remains required for any retaining configuration.
- [x] Upstream source-notice inventory completed; retain the evidence for final review.
- [ ] Combined-work GPL presentation internally consistent; no file/binary is assigned rights the project cannot prove.
- [ ] Every redistributed third-party artifact has exact origin, revision, terms, copyright/attribution and modification notice where required.
- [ ] Trademark/compatibility statements remain descriptive, non-affiliation is prominent, and no branding implies sponsorship.
- [ ] A qualified reviewer sees the **actual intended public tree**, not only an abstract memo.

### Copyright / game material

- [ ] No EBOOT/ISO/CSO/PBP/PRX/firmware/game asset, recompiled game chunk, raw game disassembly, game-derived frame dump, oracle trace, save, title key, or proprietary extracted data in the public tree/history/issues/releases.
- [ ] Synthetic/homebrew fixtures replace proprietary examples wherever practical.
- [ ] User documentation requires lawful user-supplied inputs and does not direct users to unauthorized copies, keys or bypass services.

### History / privacy

- [ ] Full-history secret & privacy audit complete.
- [ ] Known PSP constants absent from every intended public object/ref.
- [ ] General secret scan and binary/proprietary-object scan clean.
- [ ] Personal email / AI-session / private URL/path metadata deliberately accepted or removed.
- [ ] Fresh-clone verification complete.

### Security / supply chain

- [ ] Every tracked file classified (path, mode, size, SHA-256, MIME/magic, text/binary, SPDX/copyright/provenance, reason tracked, release inclusion).
- [ ] No unexplained binary blobs.
- [ ] `.gitignore`, publication audit and packaging manifest agree.
- [ ] `npm audit`/exact lockfile dependency review clean or consciously waived with evidence.
- [ ] SBOM generated for release scope.
- [ ] Parser/input boundaries suitable for public/untrusted inputs: #15 addressed to the release's stated threat model; synthetic ASan/UBSan/fuzz runs performed where supported.
- [ ] Optional local extractor dependencies are pinned/reproducible and extraction is path/size-contained rather than trusting archive member names.

### GitHub settings — owner verification

The repository tree cannot prove these. For the **new public repository**, verify against current OSPS Baseline (v2026.02.19 as of 2026-07-25):

- [ ] MFA for maintainer account; least-privilege collaborators.
- [ ] `main` ruleset/branch protection, deletion/force-push policy, required review/status rules appropriate to a one-maintainer project.
- [ ] Actions default token permissions minimal; third-party actions SHA-pinned; secrets unavailable to untrusted forks.
- [ ] Dependency graph/Dependabot as appropriate; secret scanning + push protection where available; private vulnerability reporting enabled.
- [ ] CODEOWNERS/review/governance policy matches actual maintainer structure; do not claim controls the plan/account cannot enforce.

### Documentation

- [ ] README accurately states experimental status without saying either "not playable" or an unqualified "playable"; deterministic match/return evidence and known fidelity/performance limits are both described.
- [ ] SECURITY threat model matches the actual public surface and makes clear that arbitrary PSP/game files are untrusted input.
- [ ] Setup cleanly distinguishes tracked source, system/toolchain dependencies, optional open-source dependencies, and user-supplied private inputs.
- [ ] NOTICE/license/trademark statements are modest and consistent with PGF/PGD review outcomes.
- [ ] AI-assistance disclosure remains factual and does not imply AI establishes originality or legal clearance.

## Binary-release-ready checklist — higher bar

Everything above, plus:

- [ ] Qualified review of the **actual binary contents** and its linked/embedded dependencies.
- [ ] No copyrighted game code/assets embedded unless distribution rights are established.
- [ ] Complete corresponding source for the exact GPL build is offered as required; scripts/config needed to build it are included.
- [ ] SDL/Vulkan/other redistributed dependency notices included; font redistribution rights proven or fonts excluded.
- [ ] Formal self-contained `package`/`dist` target tested from an unrelated directory.
- [ ] Release SBOM, manifest, SHA-256 hashes, compiler/toolchain revision, signing/attestation policy.
- [ ] Clean-machine package test with no reliance on private keys/oracles/local absolute paths.

## Exhaustive technical audit procedure before saying "every tracked file audited"

A remote connector review cannot honestly certify every byte/object. `tools/publish_audit.py` provides two operational modes:

- **FAST publish tripwire** (`tools/publish_audit.py --tracked-only`): Routine pre-commit and local hygiene check that quickly validates Git index paths, symlink escapes, LFS pointer format, filename collisions, and secret patterns.
  - The path set always comes from Git; `--worktree` selects the *content* behind those paths. Without it the audit reads staged blobs, which is what the pre-commit and pre-push hooks want (they run with unstaged changes stashed) and what a release export should gate on. An interactive check of a dirty tree must pass `--worktree`, or it reports on bytes that are not the ones on disk. `hst_manager.ps1 -Action Verify` runs both so a PASS covers every byte the checkout could publish; each run names its source in the summary line and in `meta.content_source`.
- **EXHAUSTIVE candidate-tree manifest gate** (`tools/publish_audit.py --candidate-tree --public-scope --manifest-out <path> --csv-out <path>`): Complete per-file provenance, license, magic, LFS/symlink, and release-disposition evidence gate for qualified review.

> [!NOTE]
> A green publication audit result (FAST or EXHAUSTIVE) is technical and engineering evidence for qualified review, **NOT legal advice or clearance**.

At the intended publication commit, run a clean-clone local pass:

1. Enumerate `git ls-files -s`, submodules, LFS pointers and ignored state. Record each tracked path's mode, size, SHA-256, MIME/magic, text/binary classification, SPDX/copyright/provenance and public-release disposition.
2. Check symlinks/submodules/LFS/executable scripts/unexpected Unicode/case collisions/data-bearing innocuous extensions.
3. Classify every binary (PGFs, VFPU LUTs, embedded shader data, fixtures): exact source, upstream revision, license, transformation, whether redistribution is permitted.
4. Map every source file to original/derived/generated status and required notice/modification history.
5. Scan `git rev-list --objects --all` and commit/tag metadata for credentials, keys, PSP/game binary magic, large blobs, private paths/usernames/URLs, proprietary strings and oracle material.
6. Fresh-build using only documented external inputs. Classify every consumed file as tracked, documented system dependency, pinned optional dependency, user-supplied private input, or generated output. Mystery local dependencies are failures.
7. Audit runtime file opens from an unrelated working directory to prove packaging completeness.
8. Run source/dependency/security gates and preserve machine-readable results with the publication candidate.

### Full-History Secret & Privacy Audit Procedure

`tools/history_audit.py` performs a non-destructive audit across all reachable Git commits, tree objects, commit log messages, and refs.

#### Measured Audit Baseline (as of 2026-08-06)

- **Head Commit (`origin/main`)**: `29c30f4f1e21ad2529566d2c00365ab6380a1d4a`
- **Reachable Commits**: 680
- **Reachable Objects**: 4,599
- **Reachable Refs**: 121
- **Sensitive Findings under Measured Scope**: 0

#### Repository Publication Architecture & GitHub Limitations

- **Reachable History vs. GitHub Unreachable Objects**:
  In-place history rewrites (such as `git filter-repo` or BFG) purge objects from local branch histories, but GitHub retains unreachable objects in internal cache layers for pull requests, commits, and refs. Anyone with a direct SHA link can still view cached historical objects on GitHub even after a force-push.
- **Fresh Sanitized Export Recommendation**:
  The recommended public repository release architecture is a **fresh sanitized public export repository** created from a clean snapshot (e.g. via `git checkout-index`), preserving the historical development graph in a separate private repository.
- **Automated Verification & Export Tool (`tools/build_public_export.py`)**:
  1. Run `python tools/build_public_export.py --verify-only` to verify all pre-publication gates (fails closed if PGF/PGD blockers remain).
  2. Run `python tools/build_public_export.py --export-dir /path/to/public_export --public-safe-profile --dry-run` to validate export path.
  3. Generate sanitized single-commit export: `python tools/build_public_export.py --export-dir /path/to/public_export --public-safe-profile`.
  4. Confirm 0 findings, 100% reproducible lock verification, and 0 history secrets before pushing to public remote.

## Additional engineering/security gates before accepting arbitrary public inputs

- PR [#15](https://github.com/Jstar269/nakagawa-recomp/pull/15) remains the main malformed-input/guest-span hardening umbrella. Include ELF/PRX, SFO/savedata, MPEG/PSMF, PGD and archive-extraction boundaries in sanitizer/fuzz planning.
- GPU/CPU framebuffer coherence tracks GPU/CPU framebuffer coherence; it is correctness rather than publication law, but public claims should not imply robust handling of arbitrary guest behavior until such invariants are tested.
- Add or enable CodeQL/dependency review/sanitizer workflows only when they can actually run; Actions are currently account-blocked (#27), so repository YAML alone is not a control.
- The earlier machine-wide process-kill concern is **resolved**: the manager now tracks workspace-launched build process trees/PIDs and scopes `hst.exe` termination to this workspace. Do not keep it listed as an active defect.

## Related

[`OSPS_BASELINE.md`](OSPS_BASELINE.md) · [`LEGAL_REWRITE_ASSESSMENT.md`](LEGAL_REWRITE_ASSESSMENT.md) · [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md) · [`PGD_KEYS.md`](PGD_KEYS.md) · [`PGF_LICENSE_REVIEW_PACKET.md`](PGF_LICENSE_REVIEW_PACKET.md) · [`PGD_AMCTRL_REVIEW_PACKET.md`](PGD_AMCTRL_REVIEW_PACKET.md) · [`NOTICE.md`](../NOTICE.md) · [`ROADMAP.md`](ROADMAP.md)
