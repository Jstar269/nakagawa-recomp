# AGENTS.md — Nakagawa Recomp

Nakagawa Recomp is an experimental PSP static recompiler and native runtime. It translates a
user-supplied PSP PRX/ELF into C (`tools/codegen.py`), links the generated translation with the
native runtime (`src/rt/`), and currently has its strongest private acceptance coverage on
*Hot Shots Tennis: Get a Grip* (HST). `interface/` is a separate local Next.js dashboard and is
not part of the native runtime.

This file is the operating contract for automated coding agents. Read it before touching the
repository. Prefer a small, proven change over a broad speculative rewrite.

## 1. Sources of truth

When project surfaces disagree, use this order for the relevant domain:

1. **Live implementation:** source, tests, Makefile, generated build metadata.
2. **Actionable defects and acceptance criteria:** GitHub Issues in
   `Jstar269/nakagawa-recomp`.
3. **Current concise status:** [`ISSUES.md`](ISSUES.md).
4. **Architecture/setup/publication contracts:**
   [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
   [`docs/SETUP.md`](docs/SETUP.md),
   [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md), and
   [`docs/PUBLIC_SOURCE_PROFILE.md`](docs/PUBLIC_SOURCE_PROFILE.md).
5. **Project identity/navigation:** [`README.md`](README.md) and
   [`docs/README.md`](docs/README.md).
6. **User manual:** GitHub Wiki.
7. **Dated research/hardware records:** evidence for the exact revision and experiment they
   describe, not evergreen truth.

Before changing a subsystem:

- fetch `origin`;
- record the exact base SHA;
- read the canonical issue, relevant source/tests, and maintained docs;
- inspect open PRs touching the same issue/files;
- verify that the task is not already fixed on current `main`.

Do not use `opencode.json`, old handoff notes, copied chat summaries, stale PR bodies, or a research
website page as authority when live GitHub/source says otherwise.

## 2. Five persistent AI lanes

Normal AI development uses exactly these persistent branch lanes:

| Agent | Branch |
| --- | --- |
| Claude | `ai/claude` |
| Codex | `ai/codex` |
| Antigravity | `ai/antigravity` |
| OpenCode | `ai/opencode` |
| Freebuff | `ai/freebuff` |

Rules:

- Commit only to the assigned lane unless the maintainer explicitly says otherwise.
- One active **mission** PR maximum per lane.
- A mission PR contains one coherent objective. Do not start a second implementation mission on
  the same branch while its current PR is unmerged.
- Every AI PR starts **DRAFT**. Agents do not mark Ready, merge, close other agents' PRs, or commit
  to `main`.
- The maintainer/orchestrator owns integration order and issue-closure decisions.
- Avoid stacked PRs. A dependency on another unmerged PR requires explicit orchestrator approval.
- Do not continuously rebase a healthy Draft just because `main` moved. Finish the bounded mission,
  then rebase once onto current `origin/main` before readiness and rerun affected gates.
- `--force-with-lease` is allowed on a Draft lane only after checking the current remote head.
  Never use an unguarded force push.
- Once the orchestrator marks a PR Ready, treat the exact head as frozen. A substantive correction
  returns it to Draft and requires fresh exact-head validation.
- After a mission merges, synchronize the persistent lane to current `origin/main` before beginning
  its next mission; never discard unmerged lane work.
- Existing pre-lane historical PRs are grandfathered until merged/closed. Do not recreate them just
  to change branch naming.

A worktree is **not** required merely because these lanes exist. Use separate persistent
checkouts/workspaces when agents run concurrently. Use a worktree only when one agent genuinely
needs simultaneous revisions, an immutable oracle baseline, or a side-by-side comparison.

## 3. Mission start: mandatory preflight

Before coding, report internally or in the Draft PR:

- `BASE_SHA`;
- assigned lane;
- canonical issue(s);
- overlapping open PRs;
- expected file ownership;
- expected **new paths**;
- provenance disposition for every expected new implementation-bearing path;
- whether private title inputs or physical PSP hardware are required;
- the failing behavior and the smallest evidence that would prove it.

If the requested result can be obtained by inspecting existing code/tests instead of changing code,
do the inspection first.

Do not create broad refactors as a substitute for understanding the failing contract.

## 4. Provenance is a precondition, not cleanup

The public provenance ledger is evidence, not self-authorization. A candidate cannot authorize its
own implementation provenance.

Before creating a **new implementation-bearing path**:

1. inspect `tools/provenance_ledger.py` and the current classifier;
2. determine whether a real deterministic classification applies;
3. otherwise require a genuine path-specific record in the trusted private detailed ledger;
4. if the trusted record does not exist, stop before publishing/committing that new implementation
   path and report `PROVENANCE_UNRESOLVED`.

Never:

- fabricate or infer a private detailed-ledger record;
- label project implementation as a synthetic fixture merely to pass a gate;
- move code into an already-attested file to evade path provenance;
- edit hashes/records to make candidate-controlled bytes self-attest;
- use `--no-verify` to bypass a provenance/publication failure unless the maintainer explicitly
  authorizes that exact action and the PR remains visibly blocked.

`--provenance-self-consistency` is a developer tripwire, **not** release attestation. A bare audit
without the external trust anchor may fail `PROVENANCE_UNVERIFIED` by design. Do not report either
state as a release approval.

Preserve per-file SPDX/copyright/upstream notices. **AI tools and automated agents must never invent
or add a `Signed-off-by:` identity on behalf of a human, company, or the tool itself.** DCO
attestation is a human contributor action.

## 5. Private inputs and output isolation

Private HST/game inputs remain local and Git-ignored. Never commit or publish:

- game executables, ISOs, assets, or extracted retail content;
- decrypted game/firmware PRXs;
- generated retail translation units;
- saves or installed game data;
- keys;
- Ghidra databases containing game bytes;
- private oracle traces/captures;
- screenshots or logs containing proprietary bytes/private paths;
- private provenance/legal-review work products.

`place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and local
`third_party/` content require care.

**Never run `git clean -fdx` in a checkout containing private inputs.**

Concurrent agents may share lawful private source inputs read-only, but should use isolated build,
save, snapshot, and log outputs. A result is not trustworthy if another agent may have mutated its
runtime state.

Scope searches to tracked source areas (`src/`, `tools/`, `docs/`, `interface/`, `assets/`,
`fixtures/`) or explicit files. Do not recursively grep ignored private trees unless the task
requires it, and never echo private bytes into Git or a public report.

## 6. HST route qualification

A private title run is evidence only after its environment is qualified.

For the currently known USA HST route, verify at minimum:

- exact repository SHA;
- intended runtime/build profile (`compiler-info` is authoritative; do not infer the built profile
  from source-file existence);
- correct USA title/input pairing;
- canonical extracted data root; the currently qualified baseline indexes **56,672 files**;
- intended disc/VFS backend is actually active;
- no persistent `umd.ufl`/disc-open retry caused by misrouting;
- correct save/SaveBase state;
- intended acceptance state was actually reached, not merely the requested vblank.

If any qualification fails, classify the observation as environment/route failure and do not turn
it into a runtime defect.

Fixed-vblank input scripts are not proof that the intended menu/game state was reached. Acceptance
routes must assert reached state wherever practical. A completed process is not evidence of the
intended route.

Use `SR_DISPATCH_FATAL=1` (or the current fail-closed equivalent) when establishing new correctness
evidence. A run that advanced through ignored dispatch misses is not a correctness pass.

## 7. Physical PSP / hardware-oracle discipline

Physical PSP evidence is authoritative only for the exact measured contract.

Hardware access is exclusive: one explicitly assigned agent owns PSPLink/physical PSP use at a
time. A branch assignment does not grant hardware ownership.

For every accepted hardware result record:

- PSP model;
- firmware/CFW;
- probe source identity and exact revision;
- toolchain identity where relevant;
- transport qualification;
- exact case/vector identity;
- repeat count;
- raw scalar/result-bit facts needed for review;
- whether a failed attempt was launch/transport failure or semantic output.

PPSSPP, Vita/Adrenaline, software-vs-Vulkan agreement, and two host implementations agreeing are
corroboration—not PSP silicon truth.

Do not promote an unmeasured cell to PSP-correct behavior merely because it advances HST.

## 8. Correctness rules

### No band-aids

Do not add:

- loop caps;
- timing sleeps;
- forced returns;
- fake success;
- latch/flip hacks;
- invented assets;
- VFS aliases used only to hide a missing resource;
- title-address patches without a proven compatibility contract.

Unknown NIDs remain visible until implemented or intentionally rejected.

Any unavoidable title/address-specific compatibility behavior is semantic debt and must be:

- tied to concrete evidence;
- represented in the compatibility-override inventory where applicable;
- covered by a deterministic regression/acceptance route;
- given a retirement criterion.

### Guest memory

Validate the complete guest span and checked size arithmetic **before** forming/using a bulk host
pointer. Invalid input must not cause partial side effects unless the measured PSP contract
requires them.

### `CpuState`

`src/rt/recomp.h` is a load-bearing ABI. MIPS `$ra` is `r[31]`; there is no separate `lr`. Any ABI
change requires coordinated consumers/mirrors and explicit offset/layout verification.

### Generated code

`build/<game>/<game>_recomp_*.c` is generated and must never be hand-edited.

Change generator/analysis/runtime source and regenerate. Generated chunk count is dynamic. The
Makefile's two-phase pipeline/compile behavior is intentional; do not collapse it into a single
parse-time dependency graph without proving clean-build correctness.

### Dependencies/security

A version bump is not a vulnerability fix by itself. Verify the resolved dependency graph and run
the real affected consumer path.

Treat PSP/game/manifests/filesystem paths as untrusted input. Path-confinement and parser changes
need adversarial tests, not only happy-path examples.

## 9. Evidence labels and claim discipline

Use the strongest accurate label; never upgrade evidence in prose:

1. **PSP_HARDWARE** — qualified physical hardware measurement.
2. **PRODUCTION_DISPATCH** — real registered production entry/NID executes and PSP-visible
   return/output/state/wake behavior is asserted.
3. **PRODUCTION_HELPER** — production implementation executes through test-specific setup/entry.
4. **PRIVATE_TITLE_ACCEPTANCE** — qualified lawful private title route exercises integration.
5. **MODEL_REFERENCE** — separate model/reference implementation.
6. **HOST_DIFFERENTIAL** — two host paths agree/disagree.
7. **SOURCE_SHAPE** — static structure/text/emission assertion only.

A private title route proves integration for that route; it is not automatically a PSP hardware
oracle. A white-box wait-state fixture does not prove the production wait syscall entered that
state. Renderer parity does not prove PSP graphics behavior.

Before claiming an issue or criterion complete:

- inspect the exact base-to-head diff and changed-file list;
- map each criterion to executable or documentary evidence;
- obtain failing-before/passing-after proof where practical;
- state residual uncertainty;
- verify the canonical GitHub issue can truthfully be closed.

## 10. Build and verification

Canonical HST development commands use PowerShell 7.6+:

```powershell
.\hst_manager.ps1 -Action BuildFull
.\hst_manager.ps1 -Action BuildFast
.\hst_manager.ps1 -Action Run
.\hst_manager.ps1 -Action Test
.\hst_manager.ps1 -Action Verify
mingw32-make --no-print-directory compiler-info
```

Direct Make callers must satisfy the current contract in [`docs/SETUP.md`](docs/SETUP.md).
Do not copy stale compiler/Vulkan/version assumptions into new docs.

Run tests proportional to the change. For broad/integration work, use
`.\hst_manager.ps1 -Action Verify` plus the affected subsystem's native/private route.

At minimum, when applicable:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree --provenance-self-consistency
pre-commit run --all-files
```

Documentation:

```powershell
python tools/lint_docs.py
npx --yes markdownlint-cli2@0.23.1
```

Dashboard changes require the checked-in lockfile and the full dashboard gate documented in
[`docs/CI.md`](docs/CI.md).

Report every gate as **PASS**, **FAIL**, **SKIP**, **BLOCKED**, or **NOT_RUN**. Missing private
or external-oracle inputs are never silent passes.

Local tests are not hosted CI. A branch is `CI-green` only when the exact PR head's applicable
GitHub jobs actually executed and passed. Draft PRs intentionally suppress substantive jobs; do
not call a Draft CI-green merely because its cheap gates passed.

Before the orchestrator is asked to mark a PR Ready:

1. fetch current `origin/main`;
2. rebase once if needed;
3. resolve conflicts preserving newer main behavior;
4. regenerate only required canonical metadata from final bytes;
5. rerun affected/full local gates;
6. update the PR's exact `BASE_SHA` and `HEAD_SHA`;
7. report `READY_QUALITY: YES` only if no known blocker remains.

Agents do not mark Ready themselves.

## 11. PR scope, metadata, and status

Open the Draft PR early after the first valid commit so the lane is visible.

Recommended PR body:

```text
AGENT_LANE
MISSION
BASE_SHA
HEAD_SHA
CANONICAL_ISSUES
SCOPE / OUT_OF_SCOPE
NEW_PATHS_AND_PROVENANCE
FAILING_BEFORE
IMPLEMENTATION
EVIDENCE
TESTS
PRIVATE_ACCEPTANCE
HARDWARE
KNOWN_UNCERTAINTY
DEPENDENCIES
BLOCKERS
READY_QUALITY: YES/NO
```

Keep unrelated fixes out. If a new defect is discovered:

- search GitHub Issues first;
- file/update the canonical issue with evidence and acceptance criteria;
- do not opportunistically implement it if it belongs to another lane or would broaden the PR.

`ISSUES.md` stays concise. Detailed narratives belong in the issue/PR. Evergreen docs should not
contain temporary run IDs, ephemeral blocker lists, or old "as of" status snapshots.

## 12. Integration discipline

The normal flow is:

```text
five Draft agent PRs may exist concurrently
        ↓
orchestrator selects one integration candidate
        ↓
candidate rebases to current main once
        ↓
local exact-head gates
        ↓
orchestrator marks Ready
        ↓
hosted exact-head CI
        ↓
orchestrator merges
        ↓
next candidate rebases
```

This minimizes wasted hosted CI and repeated rebases.

Do not use an already-passed CI run as evidence after the head SHA changes.

Do not merge automatically, enable auto-merge, or bypass required checks unless the maintainer
explicitly directs that exact action.

## 13. Documentation/publication boundary

The public source repository is an experimental compatibility/research project, not a game
download.

Do not claim that public-safe CI proves private HST playability. Do not claim a private HST run
proves the checked-in public candidate can reproduce it.

Keep these separate:

- **public source correctness**;
- **trusted provenance/publication readiness**;
- **private title acceptance**;
- **physical PSP correctness**;
- **end-user release readiness**.

Release/publication work must follow
[`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md). Exact-tree, history, SBOM,
provenance, candidate-export, hosted-CI, and human governance checks are distinct gates; one passing
does not imply the others.

## 14. Required final agent report

End every implementation mission with this compact structure:

```text
LANE:
MISSION:
BASE_SHA:
HEAD_SHA:
PR:
STATUS: DRAFT / BLOCKED / READY-QUALITY

RESULT:
ROOT_CAUSE: (if applicable)
CHANGED_FILES:
NEW_PATH_PROVENANCE:
FAILING_BEFORE:
TESTS:
PRIVATE_ACCEPTANCE:
HARDWARE:
UNRESOLVED:
DISCOVERED_FOLLOWUPS:
READY_QUALITY: YES/NO
```

Do not pad the report with a play-by-play. Report facts, exact SHAs, exact blockers, and the next
smallest useful action.
