# AGENTS.md — Nakagawa Recomp

This file is for AI coding agents working in this repository; a human contributor follows the same
rules. `CLAUDE.md` and `.github/copilot-instructions.md` only point here and add no rules
([how each tool reaches this file](docs/AI_USAGE.md#how-each-tool-reads-these-rules)). Personal
instruction files and tool settings stay local; `.gitignore` keeps them out of the repository.

**Where to start** is the quick start and covers most tasks; sections 1 to 11, which code, tests and
documents cite by number, are the full contract; **Maintainer process** applies only to agents the
maintainer runs.

## Where to start

Nakagawa Recomp is a static recompiler for PSP games. Users bring their own game; this repository
contains no game data and never will.

1. Read [`README.md`](README.md), then [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the
   design. [`docs/CODE_TOUR.md`](docs/CODE_TOUR.md) follows one game through the code, file by file.
   [`docs/README.md`](docs/README.md) says which guide covers which area.
2. Work from an issue. GitHub Issues are where work is agreed; [`docs/STATUS.md`](docs/STATUS.md) is a
   short summary of them. For anything bigger than a small fix, comment on the issue first. Good
   starting points are labelled `good first issue` and `help wanted`. An issue you file carries at
   least one label and a milestone ([which milestone](CONTRIBUTING.md#start-here)).
3. Start a branch from freshly fetched `origin/main` after the preflight in Section 1: the problem is
   not already fixed there, and no open pull request already covers it.
4. Make the change, with a test that fails before it and passes after it. A documentation change
   runs the documentation gates instead.
5. Run the checks for the files you changed (below, and Section 9).
6. Open the pull request and fill in the template. Draft, Ready and when to merge `main`: Section 10.

If you were asked only to review or diagnose, report what you found and stop: no branch, worktree,
commit, pull request or other external change.

### Build and test

**Windows 11** is the supported desktop platform. [`docs/SETUP.md`](docs/SETUP.md) lists the
toolchain and its setup, including the [`PATH` order](docs/SETUP.md#path-order) that matters on
Windows. From the repository root, with MSYS2 UCRT64's `bin` folder on `PATH`:

```powershell
python -m pip install .                                # Python tooling, once
python tools/nk_doctor.py --scope build                # checks your toolchain
mingw32-make --no-print-directory display-smoke        # the whole pipeline on a synthetic fixture
mingw32-make --no-print-directory native-core-tests    # the native runtime self-tests
mingw32-make contrib-check
```

**Linux** (Ubuntu 24.04, native or under WSL 2) builds the runtime and runs the tests headlessly; the
desktop player is Windows-first. Setup is in [`docs/LINUX_DEVELOPMENT.md`](docs/LINUX_DEVELOPMENT.md).

```bash
make CC=gcc native-core-tests
make contrib-check
```

Run one Python test module with `python -m unittest tools.test_<name>`. The whole Python suite
(`python -m unittest discover -s tools -p "test_*.py"`) is long; hosted CI runs it in shards whenever
a change routes Python, so run it locally only when hosted CI cannot.

`contrib-check` runs only the gates that apply to your changes against `origin/main`. It is not hosted
CI, and `SKIP` (tool not installed) is not a pass
([the gates and their results](CONTRIBUTING.md#the-fast-path-one-command)).

### Hard limits

These hold for every file, commit, pull request, issue, log and AI prompt. The cited section has the
full rule.

- **No game data** except curated presentation media, and no C generated from a retail game
  (Section 5). Game titles and retail disc IDs are not game data: they may appear in documentation,
  comments, tests, tools and the compatibility table.
- **No keys or credentials**: no console-bound values, user keys, tokens or passwords (Section 5).
- **No private captures**: no traces, dumps, private logs, user names or local paths (Section 5).
- **No firmware** and no firmware decryption. Decrypting the user's own game disc on their machine is
  allowed (Section 5).
- **No hidden failures**: no loop caps, sleeps, forced returns or fake success; misses stay visible
  (Section 6).
- **No hand edits to generated code** (Section 6).
- **No network use** in the player, runtime or tests without a maintainer decision (Section 5).
- **No invented identity or origin**: an agent never writes a `Signed-off-by:` line, never claims
  someone wrote something they did not, and never hides where code came from (Section 3).
- **No tags, releases, repository settings or rewritten history** (Section 3).

### What a good pull request looks like

- It comes from a branch on your fork, with **Allow edits by maintainers** left on.
- It says what was wrong, links the issue (`Tracks #123`) and lists what each change does. It gives
  the exact base and head SHAs, scope, uncertainty, private-input and hardware status, and blockers.
- It lists the exact commands you ran and their results, and the checks you could not run. "CI is
  green" is not a result, and a local pass is not hosted CI.
- Every commit an AI tool helped with ends with its `Assisted-by:` line (Section 4).
- The person submitting it confirms in the template that they have the right to submit the change
  under GPL-3.0-or-later. A DCO sign-off is optional; a person who wants one reviews the final diff
  and adds it themselves (`git commit -s`, [DCO policy](docs/DCO_POLICY.md)). The agent never adds it.
- It says where every new file came from (next section).
- It meets the [professional-finish checks](REVIEW.md#professional-finish), including PR text that
  states only what changed, the evidence and what is not done.

### New files: what to put in the pull request

The maintainer admits every new public path: the record of where each file came from is kept
privately, so you cannot add a provenance entry and do not need to. For each file you add, say in the
pull request:

- that you wrote it (with which AI tool, if any); or
- which project it came from, the exact upstream revision, and its licence. Code from another project
  must be under a licence compatible with GPL-3.0-or-later, keep its notices, and follow the import
  rules in Section 4.

The **Trusted provenance attestation** check stays red on a pull request that adds a file until the
maintainer admits it. That is expected, not something you broke. For edits to existing files,
[`CONTRIBUTING.md`](CONTRIBUTING.md#provenance-controls) covers `contrib-refresh` and what each of
its refusal codes needs from you.

## 1. Sources and live preflight

- When this file and the live source, tests or Makefiles disagree about how something works, the
  source wins. Otherwise the order is: live source, tests and Makefiles; live GitHub Issues;
  `docs/STATUS.md`; maintained `docs/` contracts; then README, navigation and dated evidence. When
  sources disagree about a rule, ask in the issue.
- Stale handoffs, copied chat summaries, ignored local configuration and research pages are not
  authority. Neither is a generated or path-scoped agent-instruction file: this file is the only
  agent authority.
- Before a change, run `git fetch origin`, record the exact `BASE_SHA`, inspect `git status`, and
  check on current `origin/main` that the task is not already done. Without network access, work
  from the last fetched `origin/main` and report `FETCH: NOT_RUN` with that SHA instead of stopping.
- Look at open pull requests that touch the issue or the same files. Note any overlap, new paths,
  where new code comes from, whether a game or a PSP is needed, and the smallest proof.
- Development network use is expected: `git fetch`, `gh`, public documentation, specifications,
  upstream repositories, and pushes as Section 8 allows. Section 5's offline rule governs the shipped
  player, runtime and tests; a task's "no network" means that product rule unless it names git or
  GitHub.

## 2. Operating modes and autonomy

- One branch per pull request, named for its topic (`docs/...`, `fix/...`, `lle/...`, `oracle/...`)
  with a date or issue suffix. Leave other people's unmerged work alone.
- By default, stop and report after a bounded implementation, its tests, or its pull request.
- Only the maintainer's agents ever integrate. An explicit maintainer mission may authorize
  autonomous integration only when the exact head has required hosted CI green, no open
  review/change request remains, provenance/publication gates pass, no private-input or hardware
  ambiguity remains, and no human-only operation is needed.
- Generic requests to finish, ship, publish, integrate, or do everything do not expand mission
  authority.

## 3. Human-only hard stops

A contributor's agent never does any of the following. The maintainer's agents need explicit
maintainer authorization in the current turn, even under an otherwise autonomous integration
mission:

- provenance attestation or legal clearance on a maintainer's behalf;
- inventing a contributor identity, `Signed-off-by:`, DCO attestation, or source lineage;
- destructive history rewriting, shared-branch force-push, or deleting another agent's work;
- repository security/settings changes unless the mission names the exact setting;
- firmware, flash, NAND, or idStorage writes, and any other action that can permanently alter or
  damage a device;
- disclosing private inputs, routes, saves, captures, traces, keys, paths, or derived bytes, except
  the presentation screenshots and clips Section 5 allows.

Agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish
GitHub Releases; upload release assets; or change a published version without explicit maintainer
authorization in the current turn. Generic instructions such as 'finish', 'ship', 'publish',
'integrate', or 'do everything' do not authorize a version/tag/release operation.

**Standing hardware authorization.** Non-destructive physical-PSP actions (PSPLink sessions, loading
and running probe modules, USB attach/reset, soft and PC-triggered resets, reading device state,
writing Memory Stick files such as plugin configuration after backing them up) need no per-turn
authorization while a standing maintainer authorization is recorded in the private workspace and the
run follows the hardware-lock protocol: one holder at a time, a session-bound lock, evidence kept
private, the lock released at the end. The maintainer can revoke it at any time by saying so; agents
then return to per-turn authorization. A standing authorization never covers the destructive writes
listed above, tags, releases, credentials, or disclosure of private inputs. Physical steps that need a
person (power cycles, media swaps, cable changes) are requested from the maintainer, never simulated
or assumed. This applies to the maintainer's own PSP; a contributor's agent uses a PSP only as its own
user directs.

## 4. Provenance and publication

- Every public path has a provenance record. For a new path, see
  [New files](#new-files-what-to-put-in-the-pull-request); the maintainer's own steps are under
  [Maintainer process](#maintainer-process).
- Deterministic documentation/configuration paths and explicitly synthetic tests may use their narrow
  classes. Never label project implementation as a synthetic fixture.
- Candidate-controlled policy, ledgers, exports, or marker bytes cannot authorize themselves. Never
  fabricate a private record, expand a wildcard, edit hashes to self-attest, or use `--no-verify` to
  bypass a publication failure.
- `--provenance-self-consistency` is a developer tripwire only. It checks coverage, resolution and
  claims (and hashes, on a ledger still in the older hashed form) but does not attest provenance.
  Release readiness requires the external trusted ledger.
- Adding or removing a public path changes `assets/public_source_profile.json`, which only the
  maintainer's admission can authorize; `provenance-refresh` and `tools/contrib_refresh.py` refuse
  added or removed paths and refresh only admitted ones. An admission binds each path to its origin
  claim, so later edits need no new admission while that claim holds. A file imported byte-identical
  stays bound to its exact bytes: changing it, or recording it as a LOCAL DELTA, needs the
  maintainer's approval of the new bytes
  ([what admission means](CONTRIBUTING.md#what-admission-means-for-your-pull-request),
  [byte-identical rows](docs/PUBLICATION_READINESS.md#byte-identical-manifest-rows)).
- A commit carries the regenerated controls for the paths it changes and nothing more; a refresh after
  merging `main` is a commit of its own. Never hand-edit `PUBLIC_EXPORT.json`,
  `assets/public_provenance_ledger.json` or the profile.
- Preserve SPDX, copyright, upstream, and third-party notices.
- End agent commits with one `Assisted-by: <tool> (<model>)` line per tool, never `Co-Authored-By:`
  ([form and tools](docs/AI_USAGE.md#disclosing-ai-help-in-commits)).
- Keep public source correctness, provenance/publication readiness, private title acceptance,
  physical PSP correctness, visual evidence, and release readiness as separate claims.
- Write implementation from specifications, public architecture references, and project-owned
  measurements by default. Never copy or line-by-line translate upstream source into a new path
  except under the import rules below.
- **Import rules.** Source under any licence compatible with GPL-3.0-or-later may be vendored or
  adapted when it works better or lands faster than a rewrite, with no further decision per import:
  for example FFmpeg codec subsets (the `src/rt/atrac3p/` pattern), uofw (github.com/uofw/uofw, MIT),
  GPL-compatible emulator source (PPSSPP, GPL-2.0-or-later; JPCSP, GPL-3.0), MIT code such as
  NVIDIA's dxvk-remix API, BSD, Apache-2.0 or zlib code, and CC0 or public-domain dedications. Each
  file is pinned to an exact upstream revision, listed in a `PROVENANCE.md` manifest as byte-identical
  or LOCAL DELTA, keeps its licence and copyright notices, is credited in `CREDITS.md`, and gets a
  trusted record and admission like any new path. An adapted file keeps the upstream copyright header
  and adds a one-line note that it was modified, as the GPL requires; it needs no special path or
  naming. Emulator-derived units are labelled `derived-translated`
  (`docs/provenance/INDEPENDENCE_MODEL.md`), never "independent". Behaviour claims still need tests,
  and PSP evidence where hardware semantics are claimed. Source under a licence incompatible with
  GPL-3.0-or-later stays excluded.
- Do not rewrite working inherited code only to make it original (`docs/INDEPENDENCE_CAMPAIGN.md`,
  re-scope). Call code "clean-room" or "independent" only when its trusted record supports that label
  (`docs/provenance/INDEPENDENCE_MODEL.md`); prefer "project-authored".

## 5. Private and public boundaries

- Never commit or publish retail executables or ISOs, extracted assets (icons, PIC/ICON, textures,
  audio, fonts), decrypted modules, C generated from a retail game (everything under `build/`), saves,
  console-unique or user-specific keys, credentials, game-byte Ghidra databases, private traces,
  captures, frame, memory or audio dumps, private logs, or anything showing a user name or local
  path. The only exception is the presentation media below.
- **Presentation media.** Curated PNG screenshots, and H.264 MP4 clips of about 60 seconds at most
  with the game's own sound, of a title running in Nakagawa (including the launcher or library with
  it loaded) may be published, as other recompilation projects do. The maintainer chooses them. Each
  is checked for personal data (no profile names, user paths or private menus) and carries a notice
  that the game belongs to its publisher, that no game data is in the repository, and that users need
  their own copy. Raw evidence captures stay private.
- **Built-in game decryption**, the way PPSSPP does it: the player and tools decrypt the encrypted
  executable and modules of a game the user owns, locally, from the user's own disc image, into the
  private per-title folder. Decrypted output stays on the user's machine. The public key tables
  PPSSPP ships may be carried only in the reviewed table `src/core/nk_psp_builtin_keys.c`, the one
  path the publication audit lets hold key literals. Console-bound values (fuse ID, secure-install
  ID, licence files) come only from the user, locally, and are never committed.
- **Firmware** follows PPSSPP's model: the operating system is HLE, the modules a game ships on its
  disc may run as LLE, and the project ships no firmware and carries no key that only firmware uses.
  It ships open replacement fonts (`font/`, `docs/provenance/FONT_ORIGINS.md`). A user may supply
  plain, unencrypted files from their own console's `flash0`, such as the real fonts, in a local
  folder under their user data that the runtime reads (without them, it serves the open replacement
  and says so). Those files are never committed, packaged, shipped or uploaded. Nakagawa never
  decrypts firmware, even privately: updater PSAR archives, the IPL, the Media Engine image,
  system-menu files and encrypted `flash0` modules are refused by name before any decryption step
  runs.
- Treat `place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and local
  third-party material as sensitive. Never run `git clean -fdx` where inputs exist.
- Public-safe CI and private title routes are different evidence classes. Missing private or
  external-oracle inputs are `NOT_RUN`, `BLOCKED`, or `SKIP`, never silent passes. Do not run title
  or physical-hardware routes for a source/configuration mission unless explicitly required. Remote
  host work stays reversible and non-overlapping.
- **Offline by default.** The player and runtime make no network connection unless the user enables
  a documented, opt-in feature. Today there are two, the consented prerequisite download and Check
  for updates with its signature-checked download, listed in
  [Network use](docs/SETUP.md#network-use). A new egress needs a maintainer decision, a "Network use"
  entry in `docs/SETUP.md`, and an update to the egress allowlist test in
  `tools/test_prereq_fetcher.py`. The reserved `network_mode` setting accepts only `"offline"`; an
  unknown value reads as offline.
- A transfer the user starts may move their own packages and saves directly to their own devices,
  for example an Android phone, never through a server. No feature uploads game code, game data,
  decrypted modules, packages, saves or guest memory to any server or third party, and nothing built
  from a user's game is ever published or shared. No telemetry. Tests never touch the internet.
  Network HLE stays fail-closed, and no network setting makes a NID miss or refusal permissive.

## 6. Correctness and evidence

- Prefer a small source-level root-cause fix with a failing-before production-path regression. Never
  add loop caps, sleeps, forced returns, fake success, latch hacks, arbitrary state writes, invented
  assets, or VFS aliases that hide missing behavior.
- **LLE first.** When the guest code for a function exists, for example a module the game ships on
  its disc, and running it needs no firmware, run that code (LLE) instead of writing a new HLE handler
  for it, as #1645 did for the game's own libfont. HLE stays for the operating system: the kernel and
  hardware boundary, firmware modules the project does not carry, syscalls, and device access. A
  system file that only a console's `flash0` holds, such as a font, follows Section 5's firmware rule.
- Unknown NIDs and dispatch misses remain visible. No switch makes a miss permissive: the interpreter
  runs only inside analyzer-owned spans (the PR #118 floor), and every rejection terminates the run.
  Correctness evidence asserts PSP-visible results, state, and wakes.
- Validate complete guest spans and checked size arithmetic before forming bulk host pointers.
- `src/rt/recomp.h` is a load-bearing ABI: MIPS `$ra` is `r[31]`, with no separate `lr`.
- Never hand-edit generated `build/<game>/<game>_recomp_*.c`; change the generator, analysis or
  runtime and regenerate. Chunk count is dynamic; `FUNCS_PER_CHUNK` is not a fixed ABI.
- A private title run proves only its qualified route. A white-box fixture is not a production
  syscall proof; software/Vulkan agreement is not a PSP hardware oracle.
- A mutation is behavioral evidence only when it generates, compiles, executes, and fails for the
  intended semantic reason. Build-only or in-place tracked-file mutations are not valid kills.

## 7. Current execution contract

Re-check live source before changing these facts; the PR numbers are navigation, not a substitute
for source evidence.

- **PR #118:** the production interpreter is a fail-closed AOT-gap correctness floor. Only
  analyzer-owned executable spans have executable authority; unsupported interpreter forms fail
  closed. It is not an all-Allegrex interpreter.
- **PR #126:** a computed `jr`/`jalr` target is latched at transfer before link writes and the delay
  slot, so later register mutation cannot redirect it.
- **PR #127:** linked calls carry explicit `target` and `resume_pc` through `dispatch_call`; CALL and
  TAIL crossings differ, and live `$ra` is not a resume descriptor.
- **PR #128:** source-owned cosimulation compares AOT/interpreter traces, writes, memory, and
  architectural state, including cross-tier CALL/TAIL cells. Its negative corpus fails closed; a
  build-only mutant is not a valid semantic kill.
- Nested interpreted calls outside that floor are not implied to work. Claim broader coverage only
  with a new source-owned contract and regression.
- **LLE Phase 1** (`src/rt/cpu_lle.*`, `src/rt/domain_mode.*`): exception entry, `eret`, and the
  import seam are opt-in via `tools/codegen.py --lle-cpu` and `--lle-import-seam`; the seam implies
  `--lle-cpu`. Default output remains byte-identical to pre-LLE output. `CpuState` (COP0, transfer
  metadata, the LL/SC link bit) is versioned by `SR_CPUSTATE_ABI_VERSION` in `src/rt/recomp.h`.
  Values without hardware measurements stay behind fail-closed paths and are labelled synthetic until
  measured on a physical PSP.
- **HLE arguments:** a handler reads arguments 1-4 from `A0`-`A3`. PSP userland is MIPS EABI, so
  arguments 5-8 arrive in `t0`-`t3` (`stack_arg(s, 0..3)`) and only 9+ come from the stack. Check
  every argument read against the function's public signature: a read past the real arguments returns
  whatever the caller left in that register (#1124).

## 8. Workspace, branch, and worktree lifecycle

- Before switching or removing anything, look at `git status`, untracked files, stashes and unpushed
  commits. Preserve unrelated changes and unique evidence. Never use reset, checkout, clean, or force
  operations for convenience.
- Never write repository-local Git identity (a checkout `[user]` block). Use the host's existing
  identity or stop and report.
- Push only to your own remote (a contributor's fork, or `origin` for the maintainer's agents), and
  only when the task includes a pull request.

## 9. Validation and gate routing

`docs/CI.md`, `docs/SETUP.md`, and the live workflow define hosted behavior. Local gates are not
hosted-CI evidence; report each as `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`. Start with
`contrib-check` (Build and test), then run the focused gate for the area you changed. The table uses
the Windows spelling; on Linux, use `make CC=gcc` in place of `mingw32-make`.

| Changed surface | Focused first gate |
| --- | --- |
| `tools/codegen.py`, analysis, or imports | relevant `tools/test_codegen_*.py` and `python tools/import_audit_gate.py` |
| `guest_interp`, `recomp`, or dispatch | `tools/test_dispatch_c.py`, `tools/test_dispatch_call_boundary.py`, cosim tests, then native selftests |
| scheduler/HLE lifecycle | `tools/test_sched_invariants.py` and `mingw32-make --no-print-directory sched-selftest` |
| FPU/VFPU conversion | relevant `tools/test_*fpu*.py` plus `fp-convert-selftest`/`vfpu-interp-selftest` |
| publication/provenance | `tools/policy_sync.py`, both publication-audit legs (the command below, with and without `--worktree`), and `tools/modified_file_notice_audit.py` |
| docs and agent policy | `python -m unittest tools.test_lint_docs tools.test_dco_policy`, markdownlint (below), and `python tools/audit_public_issue_links.py` |
| workflow/configuration | `tools/test_ci_paths.py`, pre-commit, and the full applicable hosted matrix |
| LLE CPU / domain modes | `tools/test_cpu_lle.py`, `tools/test_domain_mode.py`, then `mingw32-make native-core-tests` |

Then run these whenever they apply:

```powershell
python tools/tree_lint.py --check
python tools/lint_docs.py
python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
python -m pre_commit run --all-files
npx --yes markdownlint-cli2@0.24.0 <changed .md files>
```

Before the first push, check every item in [`REVIEW.md`](REVIEW.md#findings-to-pre-empt) "Findings to
pre-empt"; hosted CI rejects most of them. Native code also builds and runs on hosted Linux, with and
without SDL3: check it in a Linux shell such as WSL.

Which modules hosted CI routes, and the Windows-only modules a Windows-host change runs locally, are in
[`docs/CI.md`](docs/CI.md#what-hosted-ci-does-not-run). Windows host pitfalls:
[`docs/SETUP.md`](docs/SETUP.md#windows-host-pitfalls). Quick recipes (find an HLE handler, run one
native self-test, capture frames unattended): [`docs/DEBUGGING.md`](docs/DEBUGGING.md#quick-recipes).

## 10. PR and integration authorization

- When the task includes a pull request, push early: open a Draft PR after the first valid commit.
  Mark it Ready as soon as the change is finished (tests and checks run, template filled), or open it
  ready for review when it is finished at the first push. A finished PR is never left as a Draft.
  Hosted CI runs only the quick checks on a Draft; the full checks run once it is marked Ready
  ([Draft pull requests](docs/CI.md#draft-pull-requests)). A contributor's agent leaves the switch to
  Ready to its person.
- Ready means finished and open for review, not approved or integrated. A substantive correction
  after Ready, including resolving a real conflict in content that is not generated, returns the PR
  to Draft and requires fresh exact-head validation, which runs only when the PR is marked Ready
  again. A clean merge of `main`, or one whose conflicts are only generated content, by the author or
  the integrator, is not a substantive correction.
- Group related work: one PR may carry several related fixes in the same area or a whole feature
  slice, and fewer, larger PRs are better than one PR per finding. Unrelated changes go in separate
  PRs, and no PR is stacked on another.
- Before readiness, fetch `origin/main` and merge it when needed. Needed means a real conflict in
  content that is not generated, or a request from the maintainer or the integrator. It does not mean
  GitHub's `BEHIND` state, a `CI_DEFINITION_BEHIND` verdict, or a conflict confined to generated
  content the integrator regenerates at landing: `PUBLIC_EXPORT.json`,
  `assets/public_provenance_ledger.json`, `assets/public_source_profile.json`, and the outputs
  `python tools/regen_all.py` rebuilds (the generated censuses and `<!-- BEGIN GENERATED ... -->`
  blocks included); the integrator resolves those when it lands the PR. Outputs `regen_all.py --list`
  marks skipped, such as the shaders and the profile-zero digests, stay the author's to regenerate.
  Every merge of `main` and every control refresh forces new hosted CI and new review. When you do
  merge, preserve newer-main behavior, regenerate only the controls the merge requires, rerun the
  affected gates, and freeze the exact validated head. Never rebase or force-push a pushed branch.
- GitHub runs no hosted CI on a PR with a merge conflict, so until the integrator's merge head runs
  CI, such a PR's evidence is its author's local gates. A semantic conflict (a clean merge that breaks
  because `main` changed something the PR uses) shows in CI on that head; the integrator then hands
  the PR back instead of fixing the author's code.
- Never merge, enable auto-merge, close other PRs, or bypass required checks unless Section 2's
  autonomous-integration conditions hold.
- Answer every review thread, an advisory bot's included. How the maintainer lands pull requests is
  under [Maintainer process](#maintainer-process).

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
not performed. Keep `docs/STATUS.md` concise, detailed evidence in the issue or PR, and workspace
ergonomics debt in workspace documentation, not in this file.

## Maintainer process

This section applies only to agents the maintainer runs; a contributor's agent can skip it.

### Workspace

- Work only in the canonical checkout or a registered worktree, never the non-repository workspace
  root, whose own router file names these places. If a session starts at a parent folder, change
  directory before repository work: the canonical public checkout for read-only integration checks;
  the existing task worktree, or one new registered worktree, for changes; the private authority
  checkout for private-authority work, never a public tree.
- A mutating mission uses one dedicated temporary worktree and branch per pull request, reused when it
  already starts at the required exact base. A follow-up to a PR handed to integration is a new branch
  from `origin/main`. There are no persistent per-agent lanes.
- Before switching or removing anything, also inspect `git worktree list --porcelain`, the branch and
  remote heads (Section 8). A checkout owning local `main` is protected.
- Fast-forward a named canonical checkout's local `main` only when it is clean, has no unique
  untracked/stashed/unpushed work, has no dependent worktree, and an ordinary fast-forward is safe.
  Otherwise leave it untouched and report `BLOCKED_CANONICAL_MAIN`.
- Remove a temporary worktree only after inspecting its dirty and untracked state, preserving unique
  evidence and deleting disposable build and log output. Use ordinary worktree removal, then prune
  stale administrative records when safe; `git worktree remove --force` is never routine. Delete a
  merged or superseded temporary branch only when it is checked out nowhere else, has no unpushed
  commits and holds no retained evidence.
- Never reconnect public branches to private-history refs; the public and private histories stay
  disjoint.

### Admitting new paths

- Before a new implementation-bearing path, inspect `tools/provenance_ledger.py` and its classifier.
  Use a genuine path-specific trusted record or stop with `PROVENANCE_UNRESOLVED`.
- Hosted attestation accepts an added or removed public path only with a maintainer policy-delta
  authority in the private authority repository, plus an exact trusted record for a new
  implementation path. Both must be on the private repository's `main` before the hosted attestation
  runs. Step by step, with the failure each skipped step produces:
  [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).

### Landing pull requests

- Hand a PR to integration only when its exact head is pushed and every review thread, an advisory
  bot's included, is answered and resolved.
- From hand-off to integration until merge or explicit hand-back, a PR's branch and worktree belong to
  the integrator. It merges `main`, resolves conflicts in generated content (Section 10), refreshes
  provenance and pushes from that same worktree, and lands only a head whose required hosted CI is
  green after that merge (Section 2). Do not edit, stage, commit, merge or push there.
- The integrator lands from one landing run, one PR at a time. When the queue is long, the desk lands
  pull requests in batches instead: a linear batch branch with one commit per folded PR, one CI and
  CodeQL run for the batch, then a rebase-merge. Re-keys, and the union V4 delta a batch of several
  profile-changing PRs needs (`admit_paths.py --compose-v4`), follow
  [landing order](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).
  Never hold a ready pull request for an unrelated one.
- At the end of a mission, also state whether canonical main was fast-forwarded or left
  `BLOCKED_CANONICAL_MAIN`, and which worktree and branch were kept or removed.
