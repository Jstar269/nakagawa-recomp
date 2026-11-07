# AGENTS.md — Nakagawa Recomp

This file is the authoritative repository policy for AI agents and human contributors. Scoped
instruction files may add guidance for their scope but never weaken a rule here: today `CLAUDE.md`
and `.github/copilot-instructions.md`, which only point here, and
`.github/instructions/c-generated.instructions.md` (Section 6 for generated C); there is no nested
`AGENTS.md` ([how tools read this file](docs/AI_USAGE.md#how-each-tool-reads-these-rules)). Personal
instruction files and tool settings stay untracked.

**Where to start** covers most tasks; sections 1 to 11 are the full contract; **Maintainer process**
applies only to agents the maintainer runs.

## Where to start

Nakagawa Recomp is a static recompiler for PSP games. Users bring their own game; this repository
contains no game data and never will.

1. Read this file and the contracts for the area you change ([`docs/README.md`](docs/README.md) maps
   areas to guides). Before changing provenance or publication controls, CI, or codegen, read
   [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md), [`docs/CI.md`](docs/CI.md), or
   [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and Section 7. Read [`README.md`](README.md) and
   [`docs/CODE_TOUR.md`](docs/CODE_TOUR.md) when the task needs design context.
2. Work from an issue ([`docs/STATUS.md`](docs/STATUS.md) summarizes them); for more than a small fix,
   comment there first. An issue you file gets a label and a milestone ([which](CONTRIBUTING.md#start-here)).
3. Branch from freshly fetched `origin/main` after the Section 1 preflight.
4. Make the change with a test that fails before and passes after (documentation runs the
   documentation gates instead), and run the checks for the files you changed (Section 9).
5. Open the pull request and fill in the template (Draft, Ready and merging `main`: Section 10).

If asked only to review or diagnose, report and stop: no branch, worktree, commit, pull request or
other external change.

### Build and test

**Windows 11** is the supported desktop platform. Set up per [`docs/SETUP.md`](docs/SETUP.md) (mind the
[`PATH` order](docs/SETUP.md#path-order)); with MSYS2 UCRT64's `bin` on `PATH`, run
`python -m pip install .` once, `python tools/nk_doctor.py --scope build` to check the toolchain, and
`mingw32-make contrib-check`. On **Linux** (Ubuntu 24.04 or WSL 2;
[`docs/LINUX_DEVELOPMENT.md`](docs/LINUX_DEVELOPMENT.md)): `make CC=gcc native-core-tests`, then
`make contrib-check`. Run one Python module with
`python -m unittest tools.test_<name>`; hosted CI runs the whole suite. `contrib-check` runs the gates
your changes need; it is not hosted CI, and `SKIP` is not a pass
([results](CONTRIBUTING.md#the-fast-path-one-command)).

### Hard limits

These hold for every committed file, commit, pull request, issue, public log, and anything sent off
your machine; the cited section has the full rule.

- **No game data, keys or credentials**: no game files except curated presentation media, no C
  generated from a retail game, no console-bound values, user keys, tokens or passwords (Section 5).
  Game titles and disc IDs are not game data.
- **No private captures in public**: traces, dumps, private logs, user names and local paths collected
  for an authorized task stay in a private workspace; never commit or publish them, and send them
  elsewhere only under Section 3 (Section 5).
- **No firmware** and no firmware decryption; decrypting the user's own game locally is allowed
  (Section 5).
- **No hidden failures** (loop caps, sleeps, forced returns, fake success) and **no hand edits to
  generated code** (Section 6).
- **No network use** in the player, runtime or tests without a maintainer decision (Section 5).
- **No invented identity or origin**: no agent-written `Signed-off-by:`, false authorship or hidden
  origin (Section 3).
- **No tags, releases, repository settings or rewritten history** (Section 3).

### What a good pull request looks like

- It comes from a branch on the contributor's fork, with **Allow edits by maintainers** on, or from a
  maintainer agent's topic branch on `origin` (Section 8).
- It links the issue (`Tracks #123`), says what was wrong and what each change does, and gives the
  exact base and head SHAs, scope, uncertainty, private-input and hardware status, and blockers.
- It lists the exact commands run with results, and the checks not run. "CI is green" is not a
  result; a local pass is not hosted CI.
- Every AI-assisted commit ends with its `Assisted-by:` line (Section 4).
- The submitter confirms in the template the right to submit under GPL-3.0-or-later. A DCO sign-off
  is optional and only the person adds it ([DCO policy](docs/DCO_POLICY.md)); the agent never does.
- It says where every new file came from (next section) and meets the
  [professional-finish checks](REVIEW.md#professional-finish).

### New files: what to put in the pull request

The maintainer admits every new public path; you cannot add a provenance entry. For each new file, say
that you wrote it (with which AI tool, if any), or its source project, exact upstream revision and
GPL-3.0-or-later compatible licence (Section 4). The **Trusted provenance attestation** check stays red
until admission; that is expected. Edits to existing files: [`contrib-refresh`](CONTRIBUTING.md#provenance-controls).

## 1. Sources and live preflight

- When this file and the live source, tests or Makefiles disagree about how something works, the
  source wins. Otherwise: live source, tests and Makefiles; live GitHub Issues; `docs/STATUS.md`;
  maintained `docs/` contracts; then README, navigation and dated evidence. When sources disagree
  about a rule, ask in the issue. Stale handoffs, chat summaries, ignored local configuration and
  research pages are not authority.
- Before a change, run `git fetch origin`, record the exact `BASE_SHA`, inspect `git status`, confirm
  on `origin/main` that the task is not done, and check open pull requests on the same issue or files.
  Note overlap, new paths and their origin, whether a game or a PSP is needed, and the smallest proof.
- Development network use is the default: `git fetch`, `gh`, public documentation and upstream
  repositories, and pushes as Section 8 allows. An explicit network restriction from the user
  overrides that default, `git fetch` and `gh` included. Without network access or permission, work
  from the last fetched `origin/main` and report `FETCH: NOT_RUN` with that SHA. Section 5's offline rule
  governs the shipped player, runtime and tests.

## 2. Operating modes and autonomy

- One branch per pull request, named for its topic (`docs/...`, `fix/...`, `lle/...`, `oracle/...`)
  with a date or issue suffix. Leave other people's unmerged work alone.
- By default, stop and report after a bounded implementation, its tests, or its pull request.
- Only the maintainer's agents integrate, in two stages (Section 10): preparation, which may merge
  `main` to produce a head hosted CI can test, and landing. An explicit maintainer mission may
  authorize autonomous landing only when the exact head has required hosted CI green, no open
  review/change request remains, provenance/publication gates pass, no private-input or hardware
  ambiguity remains, and no human-only operation is needed.
- Generic requests to finish, ship, publish, integrate, or do everything do not expand mission
  authority.

## 3. Human-only hard stops

A contributor's agent never does any of the following. The maintainer's agents need explicit
maintainer authorization in the current turn, even under an autonomous integration mission:

- provenance attestation or legal clearance on a maintainer's behalf;
- inventing a contributor identity, `Signed-off-by:`, DCO attestation, or source lineage;
- destructive history rewriting, shared-branch force-push, or deleting another agent's work;
- repository security/settings changes unless the mission names the exact setting;
- firmware, flash, NAND, or idStorage writes, and any other action that can permanently alter or
  damage a device;
- disclosing private inputs, routes, saves, captures, traces, keys, paths, or derived bytes, except
  the presentation media Section 5 allows.

Agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish
GitHub Releases; upload release assets; or change a published version without explicit maintainer
authorization in the current turn. Generic instructions such as 'finish', 'ship', 'publish',
'integrate', or 'do everything' do not authorize a version/tag/release operation.

**Standing hardware authorization.** Non-destructive physical-PSP actions (PSPLink, probe modules,
USB attach/reset, resets, reading state, writing backed-up Memory Stick files) need no per-turn
authorization while a standing maintainer authorization is recorded in the private workspace and the
run follows the hardware-lock protocol: one holder at a time, a session-bound lock, evidence kept
private, the lock released at the end. If the maintainer revokes
it, agents return to per-turn authorization. It never covers the destructive writes above, tags,
releases, credentials, or disclosure of private inputs. Physical steps (power cycles, media swaps,
cables) are requested from the maintainer, never simulated or assumed. It covers only the
maintainer's PSP; a contributor's agent uses a PSP only as its user directs.

## 4. Provenance and publication

- Every public path has a provenance record ([New files](#new-files-what-to-put-in-the-pull-request),
  [Maintainer process](#maintainer-process)). Only deterministic documentation/configuration paths and
  explicitly synthetic tests use their narrow classes; never label implementation a synthetic fixture.
- Candidate-controlled policy, ledgers, exports, or marker bytes cannot authorize themselves. Never
  fabricate a private record, expand a wildcard, edit hashes to self-attest, or use `--no-verify` to
  bypass a publication failure. `--provenance-self-consistency` is a tripwire, not an attestation;
  release readiness requires the external trusted ledger.
- Only the maintainer's admission adds or removes a public path (it changes
  `assets/public_source_profile.json`; `tools/contrib_refresh.py` refuses it). An admission binds a
  path to its origin claim, so later edits need none while that claim holds; changing a
  byte-identical import, or recording it as LOCAL DELTA, needs the maintainer's approval of the new
  bytes ([admission](CONTRIBUTING.md#what-admission-means-for-your-pull-request),
  [byte-identical rows](docs/PUBLICATION_READINESS.md#byte-identical-manifest-rows)).
- A commit carries the regenerated controls for the paths it changes and nothing more; a refresh after
  merging `main` is its own commit. Never hand-edit `PUBLIC_EXPORT.json`,
  `assets/public_provenance_ledger.json` or the profile. Preserve SPDX, copyright, upstream and
  third-party notices.
- End agent commits with one `Assisted-by: <tool> (<model>)` line per tool, never `Co-Authored-By:`
  ([form](docs/AI_USAGE.md#disclosing-ai-help-in-commits)).
- **Import rules.** Write implementation from specifications, public references and project-owned
  measurements; never copy or line-by-line translate upstream source except as follows. Source under
  a GPL-3.0-or-later compatible licence (such as FFmpeg, uofw, PPSSPP, JPCSP, MIT, BSD, Apache-2.0,
  zlib, CC0) may be vendored or adapted without a per-import decision: pinned to an exact upstream
  revision, listed in a `PROVENANCE.md` manifest as byte-identical or LOCAL DELTA, notices kept
  (adapted files add a modification note), credited in `CREDITS.md`, and admitted. Emulator-derived
  units are `derived-translated`, never "independent". Behaviour claims still need tests, and PSP
  evidence for hardware semantics. Incompatible licences stay out.
- Do not rewrite working inherited code only to make it original. Say "clean-room" or "independent"
  only when its trusted record supports it ([independence model](docs/provenance/INDEPENDENCE_MODEL.md));
  prefer "project-authored".

## 5. Private and public boundaries

- Never commit or publish retail executables or ISOs, extracted assets, decrypted modules, C
  generated from a retail game (all of `build/`), saves, console-unique or user keys, credentials,
  game-byte Ghidra databases, private traces, captures, frame, memory or audio dumps, private logs,
  or anything showing a user name or local path, except the presentation media below. Collecting and
  examining such evidence in a private workspace, within an authorized task and by its own agent, is
  allowed; sending it anywhere else, an AI service included, is disclosure (Section 3).
- **Presentation media.** The maintainer may publish curated PNG screenshots and H.264 MP4 clips (about
  60 seconds at most, with the game's sound) of a title running in Nakagawa, each checked for personal
  data and noting that the game belongs to its publisher, no game data is in the repository, and users
  need their own copy. Raw captures stay private.
- **Decryption and firmware**, as in PPSSPP ([details](docs/SETUP.md)): a user's own game is decrypted
  locally into the private per-title folder and stays there. Key literals live only in
  `src/core/nk_psp_builtin_keys.c`. Console-bound values (fuse ID, secure-install ID, licence files)
  come only from the user and are never committed. The project ships no firmware or firmware-only key
  and never decrypts firmware, even privately: firmware inputs are refused before decryption. A user's
  plain `flash0` files (such as fonts) stay local, never committed, packaged or uploaded.
- Treat `place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and local
  third-party material as sensitive. Never run `git clean -fdx` where inputs exist.
- Public CI and private title routes are different evidence classes. Missing private or oracle inputs
  are `NOT_RUN`, `BLOCKED`, or `SKIP`, never silent passes. A source/configuration mission runs title or
  hardware routes only when required. Remote host work stays reversible and non-overlapping.
- **Offline by default.** The player and runtime connect only through a documented opt-in feature
  ([Network use](docs/SETUP.md#network-use)). A new egress needs a maintainer decision, a Network use
  entry, and an update to the allowlist test in `tools/test_prereq_fetcher.py`. The reserved
  `network_mode` setting accepts only `"offline"`.
- A user-started transfer may move their own packages and saves directly to their own devices, never
  through a server. Nothing uploads game code or data, decrypted modules, packages, saves or guest
  memory, or shares anything built from a user's game. No telemetry. Tests never touch the internet.
  Network HLE stays fail-closed; no setting makes a NID miss permissive.

## 6. Correctness and evidence

- Prefer a small source-level root-cause fix with a failing-before production-path regression. Never
  add loop caps, sleeps, forced returns, fake success, latch hacks, arbitrary state writes, invented
  assets, or VFS aliases that hide missing behavior.
- **LLE first.** When a function's guest code exists (a module the game ships) and needs no firmware,
  run it instead of writing a new HLE handler. HLE stays for the OS: kernel and hardware boundary,
  firmware modules, syscalls, device access.
- Unknown NIDs and dispatch misses stay visible; no switch makes a miss permissive, and every
  rejection terminates the run. Correctness evidence asserts PSP-visible results, state, and wakes.
- Validate complete guest spans and checked size arithmetic before forming bulk host pointers.
- Never hand-edit generated `build/<game>/<game>_recomp_*.c`; change the generator, analysis or
  runtime and regenerate.
- Keep source correctness, provenance/publication readiness, private title acceptance, physical PSP
  correctness, visual evidence, and release readiness as separate claims. A private title run proves
  only its route; a white-box fixture is not a production syscall proof; software/Vulkan agreement is
  not a PSP hardware oracle.
- A mutation is evidence only when it generates, compiles, executes, and fails for the intended
  semantic reason; build-only or in-place tracked-file mutations are not kills.

## 7. Current execution contract

Re-check live source before changing these; PR numbers are navigation, not evidence
([details](docs/ARCHITECTURE.md)).

- **PR #118:** the interpreter is a fail-closed floor: only analyzer-owned spans run, unsupported forms fail closed; not an all-Allegrex interpreter.
- **PR #126:** a `jr`/`jalr` target is latched at transfer, before link writes and the delay slot.
- **PR #127:** `dispatch_call` carries `target` and `resume_pc`; CALL and TAIL differ; live `$ra` is not a resume descriptor.
- **PR #128:** cosimulation compares AOT/interpreter state, CALL/TAIL cells included; its negative corpus fails closed; a build-only mutant is not a valid semantic kill.
- Nested interpreted calls outside that floor are unproven; broader claims need a new contract and test.
- `src/rt/recomp.h` is a load-bearing ABI: `$ra` is `r[31]`; `CpuState` is versioned by `SR_CPUSTATE_ABI_VERSION`.
- Chunk count is dynamic; `FUNCS_PER_CHUNK` is not a fixed ABI.
- LLE CPU and import seam are opt-in (`--lle-cpu`, `--lle-import-seam`); default output stays byte-identical.
- Unmeasured hardware values stay fail-closed and labelled synthetic until measured on a PSP.
- HLE arguments 1-4 are `A0`-`A3`, 5-8 `t0`-`t3` (`stack_arg`), 9+ the stack; never read past the public signature.

## 8. Workspace, branch, and worktree lifecycle

- Before switching or removing anything, check `git status`, untracked files, stashes and unpushed
  commits. Preserve unrelated changes and unique evidence. Never reset, checkout, clean, or force for
  convenience.
- Never write repository-local Git identity (a checkout `[user]` block); use the host's identity or
  stop and report.
- Push only when the task includes a pull request: a contributor to their fork, the maintainer's
  agents to topic branches on `origin`. Nobody pushes to `main`.

## 9. Validation and gate routing

`docs/CI.md`, `docs/SETUP.md`, and the live workflow define hosted behavior. Local gates are not
hosted-CI evidence; report each as `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`. Run
`contrib-check`, then the focused gate for each changed surface and the full gates that apply, both
in [CONTRIBUTING.md](CONTRIBUTING.md#the-full-gates) (on Linux, `make CC=gcc` for `mingw32-make`).
A publication or provenance change always runs `tools/policy_sync.py`, both publication-audit legs
(`python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency`,
and again without `--worktree`), and `tools/modified_file_notice_audit.py`. A workflow or
configuration change runs `tools/test_ci_paths.py`, pre-commit, and the full applicable hosted matrix.

Before the first push, check [Findings to pre-empt](REVIEW.md#findings-to-pre-empt), and build native
code in a Linux shell such as WSL (hosted Linux builds it with and without SDL3).

## 10. PR and integration authorization

- With a pull request in scope, open a Draft after the first valid commit and mark it Ready once
  finished (tests and checks run, template filled); a finished PR never stays a Draft. Hosted CI runs
  only quick checks on a Draft ([Draft pull requests](docs/CI.md#draft-pull-requests)). A
  contributor's agent leaves Ready to its person.
- Ready means finished, not approved. A substantive correction after Ready, including resolving a
  real conflict in content that is not generated, returns the PR to Draft for fresh exact-head
  validation. A clean merge of `main`, or one whose conflicts are only generated
  content, by the author or the integrator, is not a substantive correction.
- Group related work in one PR, unrelated changes in separate PRs; never stack PRs.
- An author merges `main` only for a real conflict in content that is not generated, or when the
  maintainer or the integrator asks; not for GitHub's `BEHIND` state, a `CI_DEFINITION_BEHIND`
  verdict, or a conflict confined to generated content: `PUBLIC_EXPORT.json`,
  `assets/public_provenance_ledger.json`, `assets/public_source_profile.json`, and what
  `python tools/regen_all.py` rebuilds (censuses and `<!-- BEGIN GENERATED ... -->` blocks included;
  outputs `--list` marks skipped, such as shaders and profile-zero digests, stay the author's). A
  merge preserves newer-main behavior, regenerates only the controls it requires, reruns the affected
  gates and freezes the exact head. Every merge of `main` and every control refresh forces new hosted
  CI and review. Never rebase or force-push a pushed branch.
- **Integration has two stages.** Preparation: the integrator merges `main` into a handed-off PR,
  resolves generated-content conflicts, refreshes controls and pushes a head hosted CI can test; it
  lands nothing. Landing: merging that exact head, only once its required hosted CI is green, under
  Section 2. GitHub runs no CI on a conflicting PR, so until the preparation head
  runs, the evidence is the author's local gates. If that head's CI shows a semantic conflict, the
  integrator hands the PR back instead of fixing the author's code.
- Never merge a PR, enable auto-merge, close other PRs, or bypass required checks unless Section 2's
  autonomous-landing conditions hold. Answer every review thread, an advisory bot's included.

## 11. Reporting and cleanup

End an implementation task with exact facts, not a play-by-play. Fields that do not apply say `N/A`;
a contributor's agent puts its branch name in `LANE`.

```text
LANE:
MISSION:
BASE_SHA:
HEAD_SHA:
PR:
STATUS: DRAFT / READY / BLOCKED
RESULT:
ROOT_CAUSE:
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

Say whether any tag, release, asset, version, hardware, or private-input operation was intentionally
not performed.

## Maintainer process

This section applies only to agents the maintainer runs.

### Workspace

- Work in the canonical public checkout (read-only integration checks), a registered task worktree
  (changes), or the private authority checkout (private-authority work), never the workspace root.
  One temporary worktree and branch per PR; a follow-up to a PR handed to integration is a new branch
  from `origin/main`.
- Before switching or removing anything, also inspect `git worktree list --porcelain` and the remote
  heads. The checkout owning local `main` is protected: fast-forward it only when it is clean and has
  no unique or dependent work, else report `BLOCKED_CANONICAL_MAIN`.
- Remove a temporary worktree by ordinary removal after preserving its unique evidence (`--force` is
  never routine); delete its branch only when checked out nowhere, fully pushed, and holding no
  retained evidence.
- Public and private histories stay disjoint; never connect public branches to private-history refs.

### Admitting new paths

- Before a new implementation-bearing path, inspect `tools/provenance_ledger.py` and its classifier;
  use a genuine path-specific trusted record or stop with `PROVENANCE_UNRESOLVED`.
- Hosted attestation accepts an added or removed public path only with a maintainer policy-delta
  authority, plus an exact trusted record for a new implementation path, both on the private
  authority repository's `main` first
  ([step by step](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path)).

### Landing pull requests

- Hand a PR to integration only when its exact head is pushed and every review thread is answered and
  resolved. Until landing or explicit hand-back, its branch and worktree belong to the integrator,
  which prepares and lands it (Section 10); nobody else edits, commits, merges or pushes there.
- Land one PR at a time, or a long queue as one linear batch branch (a commit per PR, one CI and
  CodeQL run, a rebase-merge), in [landing order](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).
  Never hold a ready PR for an unrelated one.
- At mission end, state whether canonical main was fast-forwarded or left `BLOCKED_CANONICAL_MAIN`,
  and which worktree and branch were kept or removed.
