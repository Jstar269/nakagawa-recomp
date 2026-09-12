# Documentation

This index covers the active sanitized public source repository and its
public-safe candidate/export boundary. Source code, tests and the
Makefile remain authoritative for implementation behavior; curated GitHub Issues
are authoritative for actionable work. Private operational, title-run, legal
review and historical-archive documents are intentionally outside the public
source tree.

## Start here

Rows are what you are about to do, not topics. Read the row you are in, not the table.

| When you are about to | Read |
| --- | --- |
| Get the project building at all | [`README.md`](../README.md), [`SETUP.md`](SETUP.md) |
| Change runtime, codegen, or the two-phase build | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| Pick up work, or check whether something is already known | [`ISSUES.md`](../ISSUES.md) (live GitHub Issues win) |
| **Add a tracked file that did not exist before** | [`PROVENANCE_MERGE_GATE.md`](PROVENANCE_MERGE_GATE.md) — a new implementation path needs its *own* record; blanket records such as `tools/*` are deliberately inert and will not cover it |
| **Change a `*_posix.c` / `*_win32.c` pair, or code behind a `!_WIN32` guard** | [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — hosted CI compiles the POSIX backend on Linux; a Windows-green change can still fail there |
| Change what the public tree ships | [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md), [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) |
| Touch provenance, notices, or attribution | [`NOTICE.md`](../NOTICE.md), [`../assets/public_provenance_ledger.json`](../assets/public_provenance_ledger.json), [`provenance/INDEPENDENCE_MODEL.md`](provenance/INDEPENDENCE_MODEL.md) |
| Decide whether a surface is title-specific or generic | [`provenance/HST_PUBLIC_CENSUS.md`](provenance/HST_PUBLIC_CENSUS.md) |
| Contribute, sign off, or report a vulnerability | [`SECURITY.md`](../SECURITY.md), [`CONTRIBUTING.md`](../CONTRIBUTING.md), [`DCO_POLICY.md`](DCO_POLICY.md) |

## Maintained engineering guides

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — runtime, code generation, renderer and
  two-phase build structure.
- [`SETUP.md`](SETUP.md) — supported toolchain and documented external inputs.
- [`CI.md`](CI.md) — path-gated hosted checks and their evidence limits.
- [`DEBUGGING.md`](DEBUGGING.md) — diagnostics and safe local troubleshooting.
- [`PORTING.md`](PORTING.md) — generic title-manifest/code-generation boundaries.
- [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — portability plan.
- [`STATIC_VERIFY.md`](STATIC_VERIFY.md) — oracle-free verification and blocked
  external-input gates.
- [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md) — bounded proposal and limits;
  source-owned probes are not hardware acceptance without measured provenance.
- [`research/PSP_THREADING_SEMANTICS.md`](research/PSP_THREADING_SEMANTICS.md) —
  frozen CreateThread and StartThread research, bounded evidence classes,
  corrected 28/5 hardware-oracle design, and local Nakagawa/general Wiki draft
  text; the frozen CT/ST oracle campaign itself remains `HARDWARE_NOT_RUN`
  while reused lifecycle/callback/ABI facts are separately measured.
- [`research/competitive/COMPETITIVE_GAP_AUDIT_2026-09-05.md`](research/competitive/COMPETITIVE_GAP_AUDIT_2026-09-05.md)
  and [`research/competitive/COMPETITIVE_ACTION_REGISTER_2026-09-05.md`](research/competitive/COMPETITIVE_ACTION_REGISTER_2026-09-05.md)
  — dated architecture comparison against other recompilation projects, and the
  action register derived from it. Both cite files in *other* repositories; treat
  them as a snapshot of that comparison, not as this project's status.
- [`AI_USAGE.md`](AI_USAGE.md) — factual AI-assistance and review boundaries.

## Doctrine, player, and productization

These were reachable only by knowing their filenames until now. Doctrine binds
current work; the productization documents are design, not built state.

- [`LLE_FIDELITY_ARCHITECTURE.md`](LLE_FIDELITY_ARCHITECTURE.md) — the
  fidelity-over-convenience doctrine and why original guest execution outranks
  host reimplementation. Read before proposing any HLE shortcut.
- [`HLE_AND_WORKAROUND_INVENTORY.md`](HLE_AND_WORKAROUND_INVENTORY.md) — the
  five-tier HLE budget, including `TITLE_SPECIFIC_HLE` driven toward zero.
- [`TITLE_PROFILE_ARCHITECTURE.md`](TITLE_PROFILE_ARCHITECTURE.md) — why no
  title id, address, or disc id may be hardcoded into generic code. The rule is
  machine-enforced by `tools/test_generic_title_planning_proof.py`.
- [`NATIVE_UI_REGRESSION_MATRIX.md`](NATIVE_UI_REGRESSION_MATRIX.md) — the
  functional checklist a native player slice is measured against.
- [`NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md`](NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md)
  — per-capability web-to-native parity, with the gaps named.
- [`NATIVE_PLAYER_ARCHITECTURE.md`](NATIVE_PLAYER_ARCHITECTURE.md),
  [`AOT_PRODUCTIZATION_ARCHITECTURE.md`](AOT_PRODUCTIZATION_ARCHITECTURE.md),
  [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md),
  [`ISO_ONLY_GAP_ANALYSIS.md`](ISO_ONLY_GAP_ANALYSIS.md) — the "program + ISO"
  productization target and the routes evaluated for it. All four are DRAFT:
  they describe work that is not built, so do not cite them as capability.
- [`WEB_UI_MIGRATION.md`](WEB_UI_MIGRATION.md) — dated inventory of the
  `interface/` prototype and the native migration plan derived from it.
- [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md),
  [`PREVIEW_RELEASE_NOTES.md`](PREVIEW_RELEASE_NOTES.md) — proposed scope and
  copy for an early preview. Both are proposals: creating a tag or release is a
  maintainer-only action (`AGENTS.md` section 3), and no agent may perform it.

## Provenance and publication

- [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) defines the exact-tree,
  policy, provenance, history, SBOM, build, documentation and governance gates.
- [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) explains the explicit
  include/exclude policy and fail-closed candidate construction.
- [`../assets/public_provenance_ledger.json`](../assets/public_provenance_ledger.json)
  is the path-hashed public provenance ledger; unresolved records are not clearance.
- [`provenance/HST_PUBLIC_CENSUS.md`](provenance/HST_PUBLIC_CENSUS.md) classifies
  title-specific versus generic/synthetic surfaces.
- [`provenance/MODIFIED_FILE_NOTICES.md`](provenance/MODIFIED_FILE_NOTICES.md)
  describes the retained upstream notice contract.

The active public repository and any publication candidate must not include
private inputs, game-derived bytes, captures, saves, keys, oracle material,
decrypted modules, generated retail code, private repository metadata, or
counsel/incident work product. Unknown paths fail closed in the machine policy.

## Document status taxonomy

Every substantial document carries one status. No physical moves are proposed
in this slice; classification comes first, directory moves separately.

| Path | Status | Authority / scope |
| --- | --- | --- |
| `ARCHITECTURE.md` | CURRENT | Implementation behavior (source/tests remain authoritative) |
| `SETUP.md` | CURRENT | Supported toolchain; declared authority for setup claims |
| `CI.md` | CURRENT | Hosted-check routing and evidence limits |
| `DEBUGGING.md` | CURRENT | Diagnostics and safe local troubleshooting |
| `PORTING.md` | CURRENT | Title-manifest/codegen boundaries; second-title readiness record |
| `TITLE_CODEGEN_PLAN.md` | CURRENT | Manifest-to-build ownership chain |
| `PLATFORM_PORTABILITY.md` | CURRENT | Portability plan; host-neutral gate is not platform support |
| `STATIC_VERIFY.md` | CURRENT | Oracle-free verification and blocked external-input gates |
| `HARDWARE_ORACLE.md` | CURRENT | Bounded proposal + measured-cells index; trace oracle unbuilt |
| `PROVENANCE_MERGE_GATE.md` | CURRENT | Merge-path provenance control (observation mode) |
| `PUBLICATION_READINESS.md` | CURRENT | Publication gates incl. the trusted refresh workflow |
| `PUBLIC_SOURCE_PROFILE.md` | CURRENT | Include/exclude policy explanation |
| `PROJECT_MODEL.md` | CURRENT | Repository and project model |
| `WORKSPACE_DOCTOR.md` | CURRENT | Doctor checks and host contract |
| `DCO_POLICY.md` | CURRENT | Sign-off governance |
| `SYMBOL_REFERENCE.md` | CURRENT | Symbol reference scope |
| `DECOMPME_INTEGRATION.md` | CURRENT | Forward-looking integration plan (not built work) |
| `AI_USAGE.md` | CURRENT | AI-assistance and review boundaries |
| `PSPDEV_LOCAL_VERIFICATION.md` | CURRENT | PSPDEV local-verification boundary |
| `LLE_FIDELITY_ARCHITECTURE.md` | CURRENT | Fidelity doctrine; ranks correctness above convenience |
| `HLE_AND_WORKAROUND_INVENTORY.md` | CURRENT | The tier doctrine and HLE budget. The per-item inventory inside it is capture-time; live counts come from `tools/test_compat_manifest.py` |
| `TITLE_PROFILE_ARCHITECTURE.md` | CURRENT | No-hardcoded-title rule, enforced by `tools/test_generic_title_planning_proof.py` |
| `NATIVE_UI_REGRESSION_MATRIX.md` | CURRENT | Native slice functional checklist |
| `NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md` | CURRENT | Web-to-native parity per capability; PARTIAL rows name what is absent |
| `PREVIEW_RELEASE.md` | CURRENT | Proposed preview scope. A proposal only — tags and releases are maintainer-only |
| `PREVIEW_RELEASE_NOTES.md` | CURRENT | Proposed preview copy, same proposal-only scope |
| `NATIVE_PLAYER_ARCHITECTURE.md` | DRAFT | Productization vision; the preparation pipeline it assumes is not connected in this build |
| `AOT_PRODUCTIZATION_ARCHITECTURE.md` | DRAFT | Evaluation of end-user recompilation routes A-G; none is implemented |
| `RUNTIME_PACKAGING_ARCHITECTURE.md` | DRAFT | Packaging target for an end-user build that does not exist yet |
| `ISO_ONLY_GAP_ANALYSIS.md` | DRAFT | LLE gap analysis and proposed resolution path; carries its own inline SUPERSEDED note for the HLE-bridge recommendations it withdrew |
| `WEB_UI_MIGRATION.md` | REFERENCE | Dated `interface/` prototype inventory and migration plan; the native player has since landed |
| `provenance/HST_PUBLIC_CENSUS.md` | CURRENT | Title-specific vs generic/synthetic classification |
| `provenance/INDEPENDENCE_MODEL.md` | CURRENT | Independence model |
| `provenance/GUEST_INTERP_ATTESTATION.md` | CURRENT | Live attestation finding |
| `provenance/GENERICITY_CENSUS_20260826.md` | REFERENCE | Dated evidence; live source authoritative |
| `TOOLCHAIN_BASELINE_2026-08.md` | REFERENCE | Dated capture; live manifests/SETUP authoritative |
| `research/PSP_THREADING_SEMANTICS.md` | REFERENCE | Frozen design + measured-scope table; CT/ST oracle NOT_RUN |
| `research/competitive/COMPETITIVE_GAP_AUDIT_2026-09-05.md` | REFERENCE | Dated cross-project comparison; cites other repositories, not this one |
| `research/competitive/COMPETITIVE_ACTION_REGISTER_2026-09-05.md` | REFERENCE | Dated action register derived from that audit; live issues authoritative |
| `research/project-truth/` | REFERENCE (reserved) | Reserved for frozen audit snapshots admitted via [issue #155](https://github.com/Jstar269/nakagawa-recomp/issues/155); no snapshot ships in this change |
| `OSPS_BASELINE.md` | HISTORICAL | Pre-republication snapshot; do not cite as status |
| `PSP_INTR_WAITS_MATRIX.md` | HISTORICAL | Snapshot table; live counts in `src/rt/intr_conformance.h` |
| `IMPORT_AUDIT.md` | HISTORICAL | Method current, snapshot example superseded |
| `ISSUE196_DIRECT_XB.md` | HISTORICAL | Superseded hypothesis preserved with scope banner |
| `provenance/MODIFIED_FILE_NOTICES.md` | HISTORICAL | Retained notice contract, capture-time scope |

Meanings: CURRENT is maintained contract — update it with the change.
REFERENCE is dated evidence — read it, do not restate it as current.
HISTORICAL is pre-republication evidence — preserve, do not cite as status.
SUPERSEDED applies to rows/sections inside a file (marked inline with a
pointer), never to a deleted file. DRAFT marks explicitly unfinished design —
read it for intent, never cite it as capability or as built state.

## Mutable facts policy

These facts must not be hand-maintained in prose — Generate, Link, or Remove,
preferring removal when the duplicate adds no value:

- issue open/closed state: link the live issue, never restate its state
  (`ISSUES.md` carries no state labels; `audit_public_issue_links.py`
  enforces the At-a-glance rule where that section exists);
- HLE address/site/bucket and compatibility-override counts: cite
  `tools/test_compat_manifest.py` over `tools/compat_overrides.py`;
- NID/hook and evidence-chain facts: regenerate via
  `tools/hle_manifest.py --evidence-chain`;
- intr-conformance totals: read `src/rt/intr_conformance.h` via
  `tools/test_intr_waits_matrix.py`;
- tracked/included/excluded path totals: regenerate via
  `tools/policy_sync.py --regen-export`;
- synthetic corpus support: enforce via `tools/test_vfpu_synth_corpus.py`;
- title-manifest span/module/base bindings: derive from the validated plan;
- toolchain effective versions: read the doctor output and live manifests,
  never copy capture-time rows into new docs.
