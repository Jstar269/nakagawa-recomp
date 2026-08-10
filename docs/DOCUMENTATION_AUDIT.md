# Documentation audit

**Audit scope:** refreshed against authoritative `main` at `fc9bcd7220e51a00c8268bc9fbcd42f92567bf3b`,
then checked against this issue/branch reconciliation candidate

**Audit date:** 2026-08-04

**Purpose:** record the 1:1 review of every tracked documentation and policy surface without
rewriting dated evidence as if it were current. This report is itself maintained documentation for
this audit; it is not a replacement for GitHub Issues, source code, tests, the Makefile, or legal
review.

## Review standard

The review checked each tracked documentation file for:

1. **Authority:** whether the file correctly identifies its source of truth and its current,
   historical, proposed, private, or legal-review status;
2. **Factual alignment:** paths, commands, tool names, build variables, license presentation,
   runtime boundaries, and repository structure against source, tests, manifests, and workflow
   configuration;
3. **Navigation:** relative links, discoverability from the root and `docs/README.md`, and
   separation between live status and dated evidence;
4. **Safety:** private-input, proprietary-data, security, provenance, and legal claims;
5. **Quality:** heading structure, tables, code fences, admonitions, terminology, and Markdown
   lint compliance; and
6. **Disposition:** maintain, clarify, preserve as dated history, or remove/archive only when a
   defensible replacement exists.

The audit intentionally did **not** treat a plausible sentence as proof of runtime behavior. Claims
were compared to implementation surfaces where practical, and external/private evidence was labeled
rather than silently promoted to repository fact.

## Inventory and disposition

The refreshed inventory contains **59 tracked documentation/policy files**: 58 Markdown/text files
(including 33 files under `docs/`) plus the repository `LICENSE` text. It also includes root project
documentation, contribution/policy files, READMEs, templates, license texts, and the dashboard robots
policy. No tracked documentation file was deleted or moved: the repository already separates current
guidance from dated investigations, and moving historical files in this PR would create noise without
improving truthfulness.

| Path | Disposition | Audit result / maintenance action |
| --- | --- | --- |
| `.github/PULL_REQUEST_TEMPLATE.md` | Maintained template | Current evidence, provenance, DCO, privacy, and historical-document rules are appropriate. |
| `.github/PULL_REQUEST_TEMPLATE/default.md` | Maintained template | Legacy/general template remains useful for GitHub selection; documentation-only checks are clearly marked. |
| `.github/copilot-instructions.md` | Maintained policy | Kept aligned with `AGENTS.md`; corrected the current chunk-variable terminology in this PR. |
| `.github/instructions/c-generated.instructions.md` | Maintained policy | Correct generated-code boundary; no change required. |
| `AGENTS.md` | Maintained repository policy | Primary agent/developer guardrails; corrected the current chunk-variable terminology in this PR. |
| `CODE_OF_CONDUCT.md` | Maintained policy | Attribution and private-reporting guidance present; no repository-specific factual defect found. |
| `CONTRIBUTING.md` | Maintained contributor guide | Corrected project-level license wording; setup and verification defer appropriately to `docs/SETUP.md`. |
| `DEDICATION.md` | Personal project context | Retained as intentional project context, not engineering guidance. |
| `ISSUES.md` | Maintained status dashboard | Refreshed against merged #236–#262 state: finite #15/#170/#171, #17, #223, #142, and #143 are closed after exact-main evidence, and current hosted-run evidence is distinguished from later-head status. |
| `LICENSE` | Legal text | GPL version 3 text is authoritative for the repository-level declaration; surrounding documentation is corrected in this PR to stop describing it as GPLv2. |
| `docs/DOCUMENTATION_AUDIT.md` | Maintained audit record | This refreshed report; it records the current-main baseline, complete inventory, dispositions, validation, and residual review gates. |
| `NOTICE.md` | Maintained provenance notice | Corrected the description of the canonical repository license; component-level uncertainty remains intentionally explicit. |
| `README.md` | Maintained entry point | Corrected the project/license boundary; current status, setup pointers, and private-input warnings remain concise. |
| `SECURITY.md` | Maintained security policy | Scope and private-reporting fallback are appropriately modest for a private unreleased project. |
| `TODO.md` | Maintained hypothesis backlog | Preserved as a hypothesis/performance record; corrected stale `FUNCS_PER_FILE` terminology to the live `FUNCS_PER_CHUNK` name. |
| `THIRD_PARTY_LICENSES/PPSSPP_FONTS.txt` | Maintained provenance record | Correctly refuses to assert a blanket font license; unresolved per-font questions remain open. |
| `THIRD_PARTY_LICENSES/PSPSDK.txt` | Third-party license text | Preserved verbatim as an attribution/license text. |
| `THIRD_PARTY_LICENSES/SHADCN_UI.txt` | Third-party license text | Preserved verbatim as an attribution/license text. |
| `assets/README.md` | Maintained asset provenance guide | Matches the checked-in VFPU provenance model; external rechecks are clearly labeled. |
| `assets/titles/README.md` | Maintained manifest guide | Correctly distinguishes source-owned manifests from private bindings and HST opt-in status. |
| `assets/upstream/pspdev.NOTICE.md` | Maintained upstream notice | Correctly labels unresolved component licenses and non-runtime PSPDEV scope. |
| `docs/README.md` | Maintained documentation index | Expanded in this PR to provide a complete, categorized entry point and link the audit. |
| `docs/AI_USAGE.md` | Maintained policy | Human review, privacy, provenance, DCO, and evidence boundaries are consistent with repository policy. |
| `docs/ARCHITECTURE.md` | Maintained technical guide | Corrected the live chunk-variable name; source/Makefile remain authoritative. |
| `docs/CI.md` | Maintained CI guide | Workflow topology and fail-closed classifier rules are documented; the latest successful hosted run is named with its recorded head and not generalized to later revisions. |
| `docs/DEBUGGING.md` | Maintained diagnostic guide | Detailed switches and private-evidence warnings are appropriate; source/manager remain authoritative. |
| `docs/DECOMPME_INTEGRATION.md` | Maintained forward-looking plan | Clearly labels the exporter and private/self-hosted boundary; no public upload claim is made. |
| `docs/GHIDRA.md` | Maintained optional developer guide | Optional, private, version-sensitive workflow is clearly separated from build/runtime requirements. |
| `docs/HARDWARE_ORACLE.md` | Proposal | Explicitly says nothing is built/tested; future tool names are proposals, not broken current links. |
| `docs/IMPORT_AUDIT.md` | Maintained technical guide | Current tools, classifications, fail-closed behavior, and private-EBOOT boundary align. |
| `docs/INVESTIGATION_CHECKPOINT_2026-07-18.md` | Dated historical checkpoint | Preserved unchanged; its snapshot banner and `ISSUES.md` pointer prevent it being mistaken for current status. |
| `docs/KEY_HISTORY_SCRUB.md` | Maintained security runbook | Correctly warns that the key-only procedure must not be run independently of #102; destructive commands remain owner-controlled. |
| `docs/LEGAL_REWRITE_ASSESSMENT.md` | Maintained engineering/legal-risk assessment | Clearly states it is not legal advice and distinguishes source lineage from legal conclusions. |
| `docs/NEXT_SESSION.md` | Maintained handoff | Reconciled against `main` at `fc9bcd72`; completed #15/#17/#170/#171/#223/#142/#143 state is recorded, and measurements remain private/local evidence. |
| `docs/OSPS_BASELINE.md` | Maintained control matrix | Correctly distinguishes tree evidence from owner/settings verification and publication gates. |
| `docs/PARALLEL_WORK.md` | Maintained optional local-tool guide | Local OpenGrip-style data is correctly kept ignored and out of publication. |
| `docs/PERFORMANCE.md` | Maintained measurement record | Clearly labels local/private evidence and non-scene-identical comparisons; historical measurements remain evidence, not guarantees. |
| `docs/PGD_AMCTRL_REVIEW_PACKET.md` | Legal-review packet | Engineering facts and requested qualified review are separated; implementation remains excluded from public-safe scope. |
| `docs/PGD_KEYS.md` | Maintained private-input boundary guide | Does not publish constants or acquisition instructions; local schema and fail-closed behavior are documented. |
| `docs/PGF_LICENSE_REVIEW_PACKET.md` | Snapshot legal-review packet | Explicitly tied to a source snapshot and requires review against the intended release tree. |
| `docs/PLATFORM_PORTABILITY.md` | Maintained roadmap/definition | Correctly says object compilation is not a supported linked/running port. |
| `docs/PORTING.md` | Maintained technical guide | Correctly limits another-title support and points to the manifest/Make boundaries. |
| `docs/PROJECT_MODEL.md` | Maintained boundary model | Separates HST, private reconstruction, and reusable toolkit concerns; no public game material is invited. |
| `docs/PSPDEV_LOCAL_VERIFICATION.md` | Maintained local-verification guide | Exact source-lock, local-tool, and generated-artifact boundaries are explicit. |
| `docs/PUBLICATION_READINESS.md` | Maintained publication gate | Keeps source publication, runtime fidelity, and binary distribution as separate thresholds. |
| `docs/PUBLIC_SOURCE_PROFILE.md` | Maintained candidate-profile guide | Correctly points to `assets/public_source_profile.json` and fail-closed unavailable backends. |
| `docs/ROADMAP.md` | Maintained strategic roadmap | Explicitly says it is not a task list and defers live priority to `ISSUES.md`. |
| `docs/SETUP.md` | Maintained setup guide | Corrected the live chunk-variable name; private inputs, optional dependencies, and blocked oracle gates remain separated. |
| `docs/STATIC_VERIFY.md` | Maintained verification guide | Correctly scopes static verification as local consistency evidence, not hardware/HLE proof. |
| `docs/STATUS_HISTORY.md` | Dated historical record | Preserved as history; it contains superseded narratives by design and is excluded from Markdown lint. |
| `docs/TITLE_CODEGEN_PLAN.md` | Maintained implementation plan | Correctly labels the manifest bridge as opt-in/read-only and fail-closed. |
| `docs/WORKSPACE_DOCTOR.md` | Maintained preflight guide | Matches `hst.ps1` scopes and fail-closed technical/privacy boundaries. |
| `docs/issue-51-entry-semantics.md` | Dated issue-specific evidence | Preserved as implementation rationale/evidence; current acceptance remains with issue #51. |
| `font/README.md` | Maintained asset/provenance guide | Correctly separates byte provenance from font-license provenance and keeps #99 open. |
| `interface/README.md` | Maintained dashboard guide | Matches the local-only boundary, current package engine constraints, and validation scripts. |
| `interface/public/robots.txt` | Maintained web policy | Local dashboard disallows crawler access; no change required. |
| `src/rt/gpu_sdl3vk/README.md` | Maintained subsystem guide | Clearly labels phase status and software/GPU semantic limits; version-sensitive dependencies defer to setup/configuration. |
| `tools/README.md` | Maintained tooling guide | Corrected the live chunk-variable name; command sequence and external-oracle boundary remain accurate. |
| `tools/TRACE_FORMAT.md` | Maintained format specification | Versioned trace grammar and comparison semantics are clear; hardware extension remains proposal-only. |

## Findings implemented by this PR

- **License presentation:** `LICENSE` and `assets/release_manifest.json` establish
  GPL-3.0-or-later as the repository/project-level declaration. The dashboard package metadata agrees,
  but is not treated as the authority for the repository. Many source files retain
  GPL-2.0-or-later or upstream-specific terms, so the documentation now distinguishes the project
  declaration from per-component/source obligations instead of calling the canonical license text
  GPLv2.
- **Build terminology:** the live Makefile and generator use `FUNCS_PER_CHUNK`; current guidance
  previously mixed that with the nonexistent `FUNCS_PER_FILE` name. Current guidance now uses the
  actual variable consistently.
- **Navigation:** `docs/README.md` now links every maintained documentation family and this audit,
  while keeping status, strategy, evidence, legal review, and local operations distinct.
- **Current-main reconciliation:** `ISSUES.md`, `README.md`, `docs/NEXT_SESSION.md`, and
  `docs/ROADMAP.md` now reflect the merged #236–#262 integration state and the evidence-backed closure
  of #15, #17, #142, #143, #170, #171, and #223; sanitizer availability remains explicitly separated
  from local passes.
- **Historical discipline:** dated checkpoints, status history, issue-specific evidence, and legal
  snapshots were not rewritten into present tense. Their banners and canonical-source pointers are
  the correct archival treatment.

## Validation performed during the audit

- Refreshed tracked documentation inventory: **59 files total** — 58 Markdown/text files plus
  `LICENSE`, including **33 `docs/*.md` files**.
- Relative Markdown target-path check: **no broken relative targets** found in tracked documentation; URL and fragment semantics remain a separate renderer-level concern.
- Markdown lint: `npx --yes markdownlint-cli2@0.23.1` — **0 issues across 49 linted files**
  (the configured exclusions intentionally omit `.github/`, `docs/STATUS_HISTORY.md`, private logs,
  generated output, and vendored trees).
- Repository/source cross-check: current manifest paths, manager action names, `hst.ps1` scopes,
  Make variables, and public-safe profile paths were checked against tracked files and implementation.
- Source/runtime validation at `ecbd2182` (carried into final `main` at `fc9bcd72`): 669 tests ran — 641
  passed, 27 were skipped, and 1 failed. The single failure is the known ignored private PGD-backend
  manifest hash mismatch, so it is not folded into a broad-pass or publication claim. Focused #170/#171
  contracts and the existing private-aware routes are recorded in their issue resolutions.
- Private inputs, ignored local evidence, remote GitHub state, and legal conclusions were not treated
  as repository-verifiable facts.

## Residual maintenance plan

These items are deliberately **not** claimed complete by a documentation PR:

1. Reconcile live GitHub Issues and hosted Actions status at the exact PR head before merge; the
   latest hosted success cited here is for its own recorded candidate head, not this PR head.
2. Re-run link and Markdown checks whenever the documentation tree changes; keep historical files
   excluded from current-status navigation and lint unless their archival formatting is intentionally
   changed.
3. Update dated measurements only when the underlying route, binary, fixture, and manifest are
   re-run; never refresh a date without refreshing its evidence.
4. Resolve provenance/legal blockers through the canonical issues and qualified human review, not by
   editing labels or disclaimers.
5. Re-audit the full tracked tree before any visibility change or public-source cut, including Git
   history, ignored/private inputs, generated artifacts, dependency locks, and repository settings.

## Reviewer checklist

- [ ] Every row above has an intentional disposition.
- [ ] Current guidance changes are limited to high-confidence factual/navigation corrections.
- [ ] Historical records were not silently rewritten.
- [ ] The root license declaration and per-component caveats remain consistent.
- [ ] No private game input, oracle evidence, local path, credential, or generated output was added.
- [ ] The reviewer has checked the exact base-to-head diff and any external status claims separately.
