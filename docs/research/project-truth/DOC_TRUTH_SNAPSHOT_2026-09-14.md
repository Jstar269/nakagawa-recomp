# Project truth snapshot — 2026-09-14

**STATUS:** `REFERENCE_SNAPSHOT`
**SNAPSHOT_DATE:** `2026-09-14`
**PUBLIC_BASE:** `origin/main` at
`5d267bcfaefe30a1ffd14e5ac2b3ed9455621488`
**AUTHORITY_ORDER:** live source, tests, Makefiles, live GitHub state, then dated
documentation and reports

This is a dated public-source snapshot. It records what the checked tree can prove at
the snapshot boundary; it is not a provenance attestation, a private-title acceptance
record, a hardware oracle result, or a release approval. A later live source or test
result supersedes this snapshot.

## Scope and evidence classes

The snapshot covers the public checkout, the native player slice, the productization
documents, and the workspace handoff facts needed to interpret them. It deliberately
does not include retail ISOs, decrypted modules, private title manifests, saves, keys,
hardware captures, browser credentials, or private provenance records.

The following claims are separate:

| Claim | Evidence class | This snapshot says |
| --- | --- | --- |
| Public source behavior | source and focused inspection | bounded, current at `PUBLIC_BASE` |
| Native player preparation | source behavior | not connected in this build |
| Productization architecture | maintained decision record | selected boundaries and unbuilt work are annotated |
| Private title execution | private route | `NOT_RUN` here |
| PSP hardware correctness | hardware route | `NOT_RUN` here |
| Publication readiness | trusted policy and maintainer gate | not self-authorized by this snapshot; new files admit only through the trusted `admit-new-reviewed` route merged in PR #157 |

## Repository anchors

- `origin/main` was fetched before the documentation branch was created. The truth-pass
  branch starts at base `ffb9381c35e29be04e7b0d0399ab9913277d3aa5`, which already
  contained the merged first-time setup wizard and clean-room XB staging (PR #202), so
  it is a later public commit than `PUBLIC_BASE`.
- This snapshot was captured from `PUBLIC_BASE` before PR #202 landed, so its native
  player claims describe the pre-wizard slice and are superseded on this branch. The
  wizard's bounded ISO/XB staging exists here; module decryption, end-user AOT
  generation, and full retail preparation remain unbuilt.
- GitHub reported PR #199 merged and no open pull requests at the capture. The repository
  wiki is disabled. These are live-state observations, not permanent document facts.
- The earlier `buffy/project-truth-snapshot-155` branch was inspected as a dated
  2026-09-05 reference. Its unrelated source and documentation changes were not
  cherry-picked; only its snapshot shape informed this new file.
- The workspace debt handoff remains open, with local diagnostics and other unlanded
  content preserved outside this public tree. Detailed workspace audit records are
  maintainer-side evidence and are intentionally not reproduced or linked here.

## Native player: what exists

The native player is a source-owned SDL3 player slice, not a complete
“program + ISO → prepared title” product:

- `src/player/main.c` can inspect the library, select an ISO, and construct a launch
  session for a runtime that is already available.
- `src/core/nk_iso.c` provides bounded ISO/PARAM.SFO inspection and file extraction
  helpers. This is not a complete retail preparation pipeline.
- `src/core/nk_launch.c` validates a candidate prepared runtime, resolves its image and
  data roots, builds the launch environment, and manages the child process.
- The player refuses an unprepared library entry and reports
  `PREPARATION UNAVAILABLE` / `Recompiled Binary Not Available`. It does not decrypt
  retail containers, generate title-specific C, compile an AOT runtime, or silently
  mark a title prepared.
- The source-owned display-smoke fixture is a proving route for the player and child
  lifecycle. It is not evidence of retail-title preparation or gameplay.
- The `Makefile` contains a `player` target. `mingw32-make --no-print-directory help`
  currently exits 2 because no `help` target exists; O-16 remains open.

## Productization truth

The five documents upgraded in this branch are now maintained records with explicit
boundaries:

| Document | Current truth at this snapshot |
| --- | --- |
| `WEB_UI_MIGRATION.md` | Current inventory and migration record. `interface/` remains a developer web UI; native migration and retirement are incomplete. |
| `NATIVE_PLAYER_ARCHITECTURE.md` | Current native-shell architecture and target onboarding contract. ISO preparation, module decryption, and progress backend remain unbuilt. |
| `AOT_PRODUCTIZATION_ARCHITECTURE.md` | Current evaluation of AOT routes. The recommended packaging route is a target decision, not an end-user code-generation capability in this tree. |
| `RUNTIME_PACKAGING_ARCHITECTURE.md` | Current process-isolation decision record. Native session/child-launch code exists; installers and distributable title packages do not. |
| `ISO_ONLY_GAP_ANALYSIS.md` | Current LLE gap analysis. ISO inspection and bounded extraction helpers exist; full encrypted retail preparation, original middleware execution, and title acceptance remain gaps. |

“Current” on these rows means the document is maintained and honest about the present
boundary. It does not mean every design step it describes has been implemented.

## Unbuilt productization stages

The following stages remain outside the proven public source route:

1. Lawful user-input preparation for encrypted retail EBOOT/PRX containers.
2. Clean-room KIRK/module-decryption integration and validation of resulting ELFs.
3. Generic, title-specific, provenance-approved AOT generation from a selected ISO.
4. Complete archive/VFS preparation and firmware-resource handling for a supported title.
5. Native UI progress, cancellation, installation promotion, and recovery for those
   operations.
6. Cross-platform installers, title package distribution, and end-user acceptance.

The documents may describe designs for these stages, but the player must continue to
surface the unavailable state until a source-owned implementation and its focused
evidence land.

## Open controls that qualify this snapshot

The dated security, improvement, and full-workspace registers remain the owners of
their respective namespaces and are unchanged. They are maintainer-side records
outside this public tree; their workspace paths and detailed contents are not part of
this snapshot.

No register identifier is renumbered or closed by this snapshot. Missing private,
hosted, or hardware evidence remains `BLOCKED`, `NOT_RUN`, or open as appropriate.

## Verification boundary

At the capture boundary (`PUBLIC_BASE`, before the branch existed):

```text
git rev-parse HEAD
5d267bcfaefe30a1ffd14e5ac2b3ed9455621488

mingw32-make --no-print-directory help
exit 2: No rule to make target 'help'
```

Documentation and policy gates run after the edits are complete. A new snapshot path
requires external trusted admission through the `admit-new-reviewed` route of
`tools/provenance_ledger.py` (merged in PR #157; first production use was PR #154's
reviewed competitive snapshots), run against the trusted private ledger with exact
path/hash authority for this file. The candidate-controlled ledger, export, or a
stranded policy patch cannot authorize this file.
