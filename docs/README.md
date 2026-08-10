# Documentation

This index is the entry point for the repository's tracked documentation. Code, tests, and the
Makefile remain authoritative for implementation behavior. GitHub Issues are canonical for
actionable work, priorities, and acceptance criteria; [`ISSUES.md`](../ISSUES.md) is only a concise
status map. Dated evidence is preserved separately and must be re-verified before reuse.

## Start here

| Need | Read |
| --- | --- |
| Project overview, current capability, setup pointer | [`README.md`](../README.md) |
| Current priorities and known limitations | [`ISSUES.md`](../ISSUES.md) |
| Contributor workflow and verification | [`CONTRIBUTING.md`](../CONTRIBUTING.md) |
| Repository/agent guardrails | [`AGENTS.md`](../AGENTS.md) |
| Windows toolchain and private-input setup | [`SETUP.md`](SETUP.md) |
| This PR's complete documentation review | [`DOCUMENTATION_AUDIT.md`](DOCUMENTATION_AUDIT.md) |

## Maintained engineering guides

| Document | Scope |
| --- | --- |
| [`SETUP.md`](SETUP.md) | Windows toolchain, local game inputs, build, run, dashboard, troubleshooting |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Recompiler pipeline, runtime modules, shared ABI, build model |
| [`DEBUGGING.md`](DEBUGGING.md) | Diagnostic categories, environment variables, watches, visual-oracle runs |
| [`WORKSPACE_DOCTOR.md`](WORKSPACE_DOCTOR.md) | Fail-closed workspace preflight, scopes, exit codes, privacy boundaries |
| [`PORTING.md`](PORTING.md) | Adapting the toolkit to another PSP title; current limits of generality |
| [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) | Staged Windows, Linux, Android, and console portability plan |
| [`IMPORT_AUDIT.md`](IMPORT_AUDIT.md) | Import coverage, fake-success classification, public gate, private-EBOOT audit |
| [`STATIC_VERIFY.md`](STATIC_VERIFY.md) | Oracle-free static verification and its evidence limits |
| [`CI.md`](CI.md) | Actions topology, path classifier, aggregate status, caching, Dependabot |
| [`AI_USAGE.md`](AI_USAGE.md) | AI information boundaries, review, provenance, DCO, evidence claims |
| [`NEXT_SESSION.md`](NEXT_SESSION.md) | Maintained machine-capable handoff and evidence discipline |
| [`PARALLEL_WORK.md`](PARALLEL_WORK.md) | Optional local symbol-reference lookup and publication boundary |
| [`PSPDEV_LOCAL_VERIFICATION.md`](PSPDEV_LOCAL_VERIFICATION.md) | PSPDEV/PSPSDK source-lock and local verification boundary |
| [`TEST_DISCOVERY.md`](TEST_DISCOVERY.md) | Canonical unittest discovery and loader/startTest accounting |
| [`TEST_SKIPS.md`](TEST_SKIPS.md) | Explicit platform, private-input, and sanitizer skip inventory |
| [`TEST_SHAPE_CLASSIFICATION.md`](TEST_SHAPE_CLASSIFICATION.md) | Conservative source-shape evidence classification and deletion boundary |
| [`TEST_MATRIX.json`](TEST_MATRIX.json) | Generated per-case evidence/matrix metadata; semantic fields require review |
| [`PSP_HARDWARE_ORACLE.md`](PSP_HARDWARE_ORACLE.md) | Source-owned PSP probe protocol, PSPLINK runbook, and readiness gate |
| [`PSP_INTR_WAITS_MATRIX.md`](PSP_INTR_WAITS_MATRIX.md) | Hardware wait/blocking context matrix (#88), its executable harness, and per-cell current-`main` status |
| [`RETAINED_BRANCH_AUDIT_2026-08-04.md`](RETAINED_BRANCH_AUDIT_2026-08-04.md) | Exact-base audit of the four retained remote branches |
| [`GHIDRA.md`](GHIDRA.md) | Optional developer-only headless Ghidra cross-check |
| [`DECOMPME_INTEGRATION.md`](DECOMPME_INTEGRATION.md) | Private decomp.me integration plan and read-only exporter |

## Planning and project boundaries

| Document | Scope |
| --- | --- |
| [`ROADMAP.md`](ROADMAP.md) | Long-range three-product strategy; not a task list |
| [`PROJECT_MODEL.md`](PROJECT_MODEL.md) | HST, private reconstruction, and reusable toolkit boundaries |
| [`TITLE_CODEGEN_PLAN.md`](TITLE_CODEGEN_PLAN.md) | Read-only title-manifest/codegen contract and fail-closed limits |
| [`PERFORMANCE.md`](PERFORMANCE.md) | Reproducible local performance/build measurements and rejected hypotheses |
| [`TODO.md`](../TODO.md) | Performance hypothesis backlog; not the live task list |

## Publication, provenance, and security

| Document | Scope |
| --- | --- |
| [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) | Source-publication and binary-release thresholds/checklists |
| [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) | `public-safe-v1` candidate profile and excluded components |
| [`OSPS_BASELINE.md`](OSPS_BASELINE.md) | OpenSSF control matrix; separates tree evidence from owner/settings checks |
| [`LEGAL_REWRITE_ASSESSMENT.md`](LEGAL_REWRITE_ASSESSMENT.md) | Engineering risk assessment, not legal advice |
| [`provenance/INDEPENDENCE_MODEL.md`](provenance/INDEPENDENCE_MODEL.md) | Classification vocabulary, evidence tiers, and the five-phase replacement process |
| [`provenance/IMPLEMENTATION_PROVENANCE.json`](provenance/IMPLEMENTATION_PROVENANCE.json) | Machine-readable per-subsystem provenance ledger and audit findings |
| [`provenance/INDEPENDENCE_BACKLOG.md`](provenance/INDEPENDENCE_BACKLOG.md) | Ranked independence candidates, hardware-oracle questions, and recorded non-goals |
| [`provenance/SAL063_RETENTION_2026-08-06.md`](provenance/SAL063_RETENTION_2026-08-06.md) | Measured per-file retention of the sal063 upstream; resolves PROV-F2 |
| [`provenance/PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md`](provenance/PGF_SOURCE_ARCHAEOLOGY_2026-08-08.md) | Function-level PGF lineage and bounded PPSSPP revision evidence for #98 |
| [`provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md`](provenance/PGD_AMCTRL_SOURCE_ARCHAEOLOGY_2026-08-09.md) | Full-history PGD/amctrl lineage, private-archive chronology, and expression matrix for #104 |
| [`COVERAGE_LEDGER.md`](COVERAGE_LEDGER.md) | Per-file audit record for #179; dated session entries must be re-verified by diff when reviewed content changes |
| [`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md) | Coordinated private-history scrub runbook; do not run standalone |
| [`PGF_LICENSE_REVIEW_PACKET.md`](PGF_LICENSE_REVIEW_PACKET.md) | Qualified review packet for PGF/JPCSP/intraFont provenance |
| [`PGD_AMCTRL_REVIEW_PACKET.md`](PGD_AMCTRL_REVIEW_PACKET.md) | Qualified review packet for PGD/amctrl distribution questions |
| [`PGD_KEYS.md`](PGD_KEYS.md) | Local-only PSP KIRK/amctrl constants schema and safety boundary |
| [`PUBLISH_VS_EXCLUDE_MATRIX.md`](PUBLISH_VS_EXCLUDE_MATRIX.md) | File/component publish-vs-exclude disposition matrix for the fresh public-repository candidate |
| [`PUBLICATION_LANE_VERIFICATION_2026-08-06.md`](PUBLICATION_LANE_VERIFICATION_2026-08-06.md) | Verification of completed #99/#102 work plus the sanitized-public-repository validation and tooling defect ledger |

## Proposed or dated evidence

| Document | Disposition |
| --- | --- |
| [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md) | Proposal only; explicitly says nothing is built or tested |
| [`PSP_ISSUE_MATRIX.json`](PSP_ISSUE_MATRIX.json) | Generated current-open-issue routing matrix; regenerate with `tools/psp_issue_matrix.py` |
| [`STATUS_HISTORY.md`](STATUS_HISTORY.md) | Dated investigations, resolved blockers, and superseded hypotheses; not current status |
| [`INVESTIGATION_CHECKPOINT_2026-07-18.md`](INVESTIGATION_CHECKPOINT_2026-07-18.md) | Dated checkpoint; re-verify against source and fresh routes before reuse |
| [`issue-51-entry-semantics.md`](issue-51-entry-semantics.md) | Issue-specific implementation rationale/evidence; current acceptance remains in issue #51 |
| [`issue-139-face-resource-semantics.md`](issue-139-face-resource-semantics.md) | Static scorecard face-resource semantics for #139/#196; mounted-slot hypothesis resolved negative, with proven/inference/hypothesis tags |
| [`NID_INTEGRITY_AUDIT_2026-08-06.md`](NID_INTEGRITY_AUDIT_2026-08-06.md) | Dated NID→name→signature→handler integrity audit for #75/#78/#83/#86; re-verified against the enforcement mechanism actually adopted on `main` (2026-08-10 note) |

Historical and private evidence must not be promoted into current guidance merely by changing its
heading or date. Update the maintained guide and canonical GitHub Issue when current behavior,
acceptance criteria, or setup changes. Preserve private inputs, game-derived traces, screenshots,
Ghidra databases, local paths, and legal advice outside Git history.

## Related documentation outside `docs/`

- [`NOTICE.md`](../NOTICE.md), [`LICENSE`](../LICENSE), and [`THIRD_PARTY_LICENSES/`](../THIRD_PARTY_LICENSES/)
  record license, attribution, and provenance boundaries.
- [`assets/README.md`](../assets/README.md) and [`assets/titles/README.md`](../assets/titles/README.md)
  document tracked data and title manifests.
- [`font/README.md`](../font/README.md) documents PGF byte provenance and unresolved rights.
- [`tools/README.md`](../tools/README.md) and [`tools/TRACE_FORMAT.md`](../tools/TRACE_FORMAT.md)
  document host-side tools and trace syntax.
- [`interface/README.md`](../interface/README.md) documents the optional local dashboard.
- [`src/rt/gpu_sdl3vk/README.md`](../src/rt/gpu_sdl3vk/README.md) documents the renderer subsystem.
