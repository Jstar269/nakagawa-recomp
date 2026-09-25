# Documentation

This index covers the active sanitized public source repository and its
public-safe candidate/export boundary. Source code, tests and the
Makefile remain authoritative for implementation behavior; curated GitHub Issues
are authoritative for actionable work. Private operational, title-run and legal
review documents are intentionally outside the public source tree; explicitly
labelled historical evidence stays in the tree for provenance.

## Start here

Rows are what you are about to do, not topics. Read the row you are in, not the table.

| When you are about to | Read |
| --- | --- |
| Get the project building at all | [`README.md`](../README.md), [`SETUP.md`](SETUP.md), [`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md) |
| Play a game you own in the native player | [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md) |
| Check title and subsystem compatibility | [`COMPATIBILITY.md`](COMPATIBILITY.md) |
| Run the release smoke test on a build | [`SMOKE_TEST.md`](SMOKE_TEST.md) |
| Change runtime, codegen, or the two-phase build | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| Pick up work, or check whether something is already known | [`ISSUES.md`](../ISSUES.md) (live GitHub Issues win) |
| **Add a tracked file that did not exist before** | [`PROVENANCE_MERGE_GATE.md`](PROVENANCE_MERGE_GATE.md) — a new implementation path needs its *own* record; blanket records such as `tools/*` are deliberately inert and will not cover it |
| **Change a `*_posix.c` / `*_win32.c` pair, or code behind a `!_WIN32` guard** | [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — hosted CI compiles the POSIX backend on Linux; a Windows-green change can still fail there |
| Change what the public tree ships | [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md), [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) |
| Touch provenance, notices, or attribution | [`NOTICE.md`](../NOTICE.md), [`../assets/public_provenance_ledger.json`](../assets/public_provenance_ledger.json), [`provenance/INDEPENDENCE_MODEL.md`](provenance/INDEPENDENCE_MODEL.md) |
| Decide whether a surface is title-specific or generic | [`provenance/HST_PUBLIC_CENSUS.md`](provenance/HST_PUBLIC_CENSUS.md) |
| Operate an AI agent | [`AGENTS.md`](../AGENTS.md) (the policy authority), then [`REVIEW.md`](../REVIEW.md) when reviewing |
| Contribute, sign off, or report a vulnerability | [`SECURITY.md`](../SECURITY.md), [`CONTRIBUTING.md`](../CONTRIBUTING.md), [`DCO_POLICY.md`](DCO_POLICY.md) |

## Maintained engineering guides

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — runtime, code generation, renderer and
  two-phase build structure.
- [`SETUP.md`](SETUP.md) — supported toolchain and documented external inputs.
- [`YOUR_OWN_GAMES.md`](YOUR_OWN_GAMES.md) — what the player needs to play a game you own.
- [`COMPATIBILITY.md`](COMPATIBILITY.md) — per-title and subsystem compatibility
  states, semantic boundaries, and tracking issues.
- [`SMOKE_TEST.md`](SMOKE_TEST.md) — numbered pass/fail release smoke test for the
  native player.
- [`CI.md`](CI.md) — path-gated hosted checks and their evidence limits.
- [`DEBUGGING.md`](DEBUGGING.md) — diagnostics and safe local troubleshooting.
- [`PORTING.md`](PORTING.md) — generic title-manifest/code-generation boundaries.
- [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — portability plan.
- [`LINUX_DEVELOPMENT.md`](LINUX_DEVELOPMENT.md) — Linux/WSL development commands and gate inventory (issue #306).
- [`ENHANCEMENT_CONTRACT.md`](ENHANCEMENT_CONTRACT.md) — authentic-vs-enhanced
  boundary and package declarations; no enhancement loader yet (in the works).
- [`STATIC_VERIFY.md`](STATIC_VERIFY.md) — oracle-free verification and blocked
  external-input gates.
- [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md) — bounded proposal and limits;
  source-owned probes are not hardware acceptance without measured provenance.
- [`research/PSP_THREADING_SEMANTICS.md`](research/PSP_THREADING_SEMANTICS.md) —
  frozen CreateThread and StartThread research, bounded evidence classes,
  corrected 28/5 hardware-oracle design, and local Nakagawa/general Wiki draft
  text; the CT/ST oracle campaign is completed (`HARDWARE_MEASURED`) while
  reused lifecycle/callback/ABI facts are separately measured.
- [`AI_USAGE.md`](AI_USAGE.md) — factual AI-assistance and review boundaries.

## Doctrine, player, and productization

These were reachable only by knowing their filenames until now. Doctrine binds
current work; the productization documents are maintained decision and gap records
with explicit target and unbuilt boundaries.

- [`LLE_FIDELITY_ARCHITECTURE.md`](LLE_FIDELITY_ARCHITECTURE.md) — the
  fidelity-over-convenience doctrine and why original guest execution outranks
  host reimplementation. Read before proposing any HLE shortcut.
- [`HLE_AND_WORKAROUND_INVENTORY.md`](HLE_AND_WORKAROUND_INVENTORY.md) — the
  five-tier HLE budget, including `TITLE_SPECIFIC_HLE` driven toward zero.
- [`TITLE_PROFILE_ARCHITECTURE.md`](TITLE_PROFILE_ARCHITECTURE.md) — why no
  title id, address, or disc id may be hardcoded into generic code. The rule is
  machine-enforced by `tools/test_generic_title_planning_proof.py`.
- [`TITLE_MANAGER_DECOUPLING.md`](TITLE_MANAGER_DECOUPLING.md) — roadmap for
  decoupling the primary build orchestrator from legacy title defaults.
- [`NATIVE_UI_REGRESSION_MATRIX.md`](NATIVE_UI_REGRESSION_MATRIX.md) — the
  functional checklist a native player slice is measured against.
- [`NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md`](NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md)
  — per-capability web-to-native parity, with the gaps named.
- [`NATIVE_PLAYER_ARCHITECTURE.md`](NATIVE_PLAYER_ARCHITECTURE.md),
  [`AOT_PRODUCTIZATION_ARCHITECTURE.md`](AOT_PRODUCTIZATION_ARCHITECTURE.md),
  [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md),
  [`ISO_ONLY_GAP_ANALYSIS.md`](ISO_ONLY_GAP_ANALYSIS.md) — the "program + ISO"
  productization target and the routes evaluated for it. All four are CURRENT
  maintained records; each marks unbuilt behavior inline, so CURRENT does not mean
  the target pipeline exists.
- [`WEB_UI_MIGRATION.md`](WEB_UI_MIGRATION.md) — CURRENT inventory and migration
  record for the `interface/` prototype and the native migration derived from it.
- [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md),
  [`PREVIEW_RELEASE_NOTES.md`](PREVIEW_RELEASE_NOTES.md) — proposed scope and
  copy for an early preview. Both are proposals: creating a tag or release is a
  maintainer-only action (`AGENTS.md` section 3), and no agent may perform it.

## Provenance and publication

- [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) defines the exact-tree,
  policy, provenance, history, SBOM, build, documentation and governance gates.
- [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) explains the explicit
  include/exclude policy and fail-closed candidate construction.
- [`INDEPENDENCE_CAMPAIGN.md`](INDEPENDENCE_CAMPAIGN.md) plans the route from
  derived code to original code and LLE replacement. A plan only — nothing in
  it is implemented unless marked.
- [`cleanroom/SCHED_SPEC.md`](cleanroom/SCHED_SPEC.md) specifies scheduler
  behaviour for a future clean-room implementation. A spec only — the derived
  scheduler it describes replacing is still the shipped code.
- [`cleanroom/PRX_LOADER_SPEC.md`](cleanroom/PRX_LOADER_SPEC.md) specifies the
  runtime PSP module (PRX) loader for clean-room rewrite unit G3.
- [`cleanroom/PGF_SPEC.md`](cleanroom/PGF_SPEC.md) specifies the runtime PSP
  PGF reader for clean-room rewrite unit G4. A spec only — reader support is
  still in the works (#349).
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
| `YOUR_OWN_GAMES.md` | CURRENT | User guide: own-game input, unencrypted files, ownership and in-the-works boundaries |
| `COMPATIBILITY.md` | CURRENT | Per-title and subsystem compatibility states and semantic boundaries |
| `SMOKE_TEST.md` | CURRENT | Numbered pass/fail release smoke test for the native player |
| `CI.md` | CURRENT | Hosted-check routing and evidence limits |
| `DEBUGGING.md` | CURRENT | Diagnostics and safe local troubleshooting |
| `PORTING.md` | CURRENT | Title-manifest/codegen boundaries; second-title readiness record |
| `TITLE_CODEGEN_PLAN.md` | CURRENT | Manifest-to-build ownership chain |
| `PLATFORM_PORTABILITY.md` | CURRENT | Portability plan; host-neutral gate is not platform support |
| `LINUX_DEVELOPMENT.md` | CURRENT | Linux/WSL dev commands and gate inventory (issue #306) |
| `ENHANCEMENT_CONTRACT.md` | CURRENT | Enhancement contract and package validator; loader in the works |
| `STATIC_VERIFY.md` | CURRENT | Oracle-free verification and blocked external-input gates |
| `HARDWARE_ORACLE.md` | CURRENT | Bounded proposal + measured-cells index; trace oracle unbuilt |
| `HARDWARE_RUNNER_AUTONOMY.md` | CURRENT | Runner autonomy design; simulated-transport conformance suite only, PSP-side runner unbuilt |
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
| `TITLE_MANAGER_DECOUPLING.md` | CURRENT | Orchestrator decoupling and nk_manager transition plan |
| `NATIVE_UI_REGRESSION_MATRIX.md` | CURRENT | Native slice functional checklist |
| `NATIVE_PLAYER_IMPLEMENTATION_PROGRESS.md` | CURRENT | Web-to-native parity per capability; PARTIAL rows name what is absent |
| `PREVIEW_RELEASE.md` | CURRENT | Proposed preview scope. A proposal only — tags and releases are maintainer-only |
| `PREVIEW_RELEASE_NOTES.md` | CURRENT | Proposed preview copy, same proposal-only scope |
| `NATIVE_PLAYER_ARCHITECTURE.md` | CURRENT | Maintained native-shell architecture; the wizard's bounded ISO/XB staging exists (PR #202), while module decryption and full retail preparation remain unbuilt and are marked inline |
| `AOT_PRODUCTIZATION_ARCHITECTURE.md` | CURRENT | Maintained evaluation of end-user recompilation routes A-G; recommended routes remain unimplemented |
| `RUNTIME_PACKAGING_ARCHITECTURE.md` | CURRENT | Maintained process-isolation decision record; installers and distributable title packages remain unbuilt |
| `ISO_ONLY_GAP_ANALYSIS.md` | CURRENT | Maintained LLE gap analysis; existing bounded ISO helpers and absent retail-preparation stages are distinguished inline |
| `WEB_UI_MIGRATION.md` | CURRENT | Maintained `interface/` inventory and native migration record; the wizard staging slice landed (PR #202), while web retirement and full parity remain unbuilt |
| `provenance/HST_PUBLIC_CENSUS.md` | CURRENT | Title-specific vs generic/synthetic classification |
| `provenance/INDEPENDENCE_MODEL.md` | CURRENT | Independence model |
| `INDEPENDENCE_CAMPAIGN.md` | DRAFT | Independence route plan; read for intent, never cite as built state |
| `cleanroom/SCHED_SPEC.md` | DRAFT | Scheduler clean-room behaviour spec; read for intent, never cite as shipped behaviour |
| `cleanroom/PRX_LOADER_SPEC.md` | DRAFT | Runtime PRX loader clean-room behaviour spec (G3); read for intent, never cite as shipped behaviour |
| `cleanroom/PGF_SPEC.md` | DRAFT | Runtime PGF reader clean-room behaviour spec (G4); read for intent, never cite as shipped behaviour |
| `provenance/GUEST_INTERP_ATTESTATION.md` | CURRENT | Live attestation finding |
| `provenance/FONT_ORIGINS.md` | CURRENT | PGF replacement-font origin evidence and route decision |
| `TOOLCHAIN_BASELINE_2026-08.md` | REFERENCE | Dated capture; live manifests/SETUP authoritative |
| `research/PSP_THREADING_SEMANTICS.md` | REFERENCE | Frozen design + measured-scope table; CT/ST oracle HARDWARE_MEASURED |
| `PSP_INTR_WAITS_MATRIX.md` | HISTORICAL | Snapshot table; live counts in `src/rt/intr_conformance.h` |
| `PSP_INTR_WAITS_CENSUS.md` | CURRENT | Generated registration census for the waits matrix; regenerate with `tools/waits_census.py` |
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
