# AGENTS.md — Nakagawa Recomp

This file is for AI coding agents working in this repository: Claude Code, Codex, GitHub Copilot,
Cursor, Gemini CLI, Jules and the rest. People can read it too; it is the same set of rules a human
contributor follows, kept in one place. Tool-specific files (`CLAUDE.md`,
`.github/copilot-instructions.md`) only point here and add no rules of their own.

Codex, Cursor, Jules and most other agents read this file on their own. Claude Code and Copilot
reach it through those two files. Gemini CLI reads only `GEMINI.md` unless told otherwise: set
`"context": {"fileName": ["AGENTS.md"]}` in your own `.gemini/settings.json`. With Aider, pass
`--read AGENTS.md`. Personal instruction files and tool settings stay local; `.gitignore` keeps
them out of the repository.

How it is laid out:

- **Where to start** is the quick start: what the project is, how to build and test, the hard
  limits, and what a good pull request looks like. Most tasks need nothing more.
- **Sections 1 to 11** are the full contract. Code, tests and other documents cite them by number.
- **Maintainer process**, at the end, applies only to agents the maintainer runs. A contributor's
  agent can skip it.

When this file and the live source, tests or Makefiles disagree about how something works, the
source wins. When they disagree about a rule, ask in the issue.

## Where to start

Nakagawa Recomp is a static recompiler for PSP games. It translates a game's MIPS code into C ahead
of time, compiles that C together with a native runtime that stands in for the PSP's operating system
and hardware, and runs the result in a native player (C, SDL3 and Vulkan). Users bring their own
game; this repository contains no game data and never will.

Before you change anything:

1. Read [`README.md`](README.md), then [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the
   design. [`docs/CODE_TOUR.md`](docs/CODE_TOUR.md) follows one game through the code, file by file.
   [`docs/README.md`](docs/README.md) says which guide covers which area.
2. Work from an issue. GitHub Issues are where work is agreed; [`ISSUES.md`](ISSUES.md) is a short
   summary of them. For anything bigger than a small fix, comment on the issue first. Good
   starting points are labelled `good first issue` and `help wanted`. An issue you file carries at least
   one label and a milestone ([which milestone](CONTRIBUTING.md#start-here)).
3. Run `git fetch origin` and start a branch from current `origin/main`. Check that the problem is
   not already fixed there and that no open pull request already covers it.
4. Make the change, with a test that fails before it and passes after it. Related fixes in the same
   area, or a feature with its tests and docs, can share one pull request; unrelated changes cannot.
5. Run the checks for the files you changed (below, and Section 9).
6. Open a **Draft** pull request and fill in the template.

If you were asked only to review or diagnose, report what you found and stop: no branch, commit or
pull request.

### Build and test

**Windows 11** is the supported desktop platform. You need PowerShell 7.4 or later, MSYS2 UCRT64
(GCC, GNU Make, SDL3, SDL3_ttf, the Vulkan headers and loader), CPython 3.15 and the Vulkan SDK.
[`docs/SETUP.md`](docs/SETUP.md) has the full steps, including the
[`PATH` order](docs/SETUP.md#path-order) that matters on Windows. From the repository root, with
MSYS2 UCRT64's `bin` folder on `PATH`:

```powershell
python -m pip install .                                # Python tooling, once
python tools/nk_doctor.py --scope build                # checks your toolchain
mingw32-make --no-print-directory display-smoke        # builds and checks the whole pipeline on a synthetic fixture; no game needed
mingw32-make --no-print-directory native-core-tests    # the native runtime self-tests
mingw32-make contrib-check                             # the checks that apply to the files you changed
```

**Linux** (Ubuntu 24.04, native or under WSL 2) builds the runtime and runs the tests headlessly; the
desktop player is Windows-first. Setup is in
[`docs/LINUX_DEVELOPMENT.md`](docs/LINUX_DEVELOPMENT.md).

```bash
make CC=gcc native-core-tests    # the native runtime self-tests, as hosted CI runs them
make contrib-check               # the checks that apply to the files you changed
```

On either platform, run one Python test module with `python -m unittest tools.test_<name>`. The whole
Python suite (`python -m unittest discover -s tools -p "test_*.py"`) is long; hosted CI runs it in
shards on every pull request, so run it locally only when hosted CI cannot.

`contrib-check` compares your branch with `origin/main` and runs only the gates that apply: Ruff, the
matching Python tests, a strict C compile of changed C, markdownlint and the documentation lint for
changed Markdown, and the publication audit. It is a quick local check, not a substitute for hosted
CI. Each gate reports `PASS`, `FAIL`, `SKIP` (tool not installed: not a pass), `NOT_RUN` or
`MAINTAINER-SIDE` ([what each means](CONTRIBUTING.md#the-fast-path-one-command)).

### Hard limits

These hold for every file, commit, pull request, issue, log and AI prompt. Sections 3, 5 and 6 give
the full rules.

- **No game data.** No ISOs, executables, decrypted modules, extracted assets (icons, textures, audio,
  fonts), saves, game-byte Ghidra databases, or C generated from a retail game (everything under
  `build/`). The only game images allowed are the curated presentation media Section 5 describes,
  and the maintainer chooses those.
- **No keys or credentials.** No console-bound values (fuse ID, secure-install ID, licence files),
  user keys, tokens or passwords. The public key tables for built-in game decryption live only in
  `src/core/nk_psp_builtin_keys.c`.
- **No private captures.** No traces, frame or memory dumps, private logs, or anything showing a
  user name or local path.
- **No firmware.** No firmware files, no firmware decryption, and no key that only firmware uses.
  Decrypting the user's own game disc on their machine is allowed (Section 5).
- **No hidden failures.** No loop caps, sleeps, forced returns or fake success that hide missing
  behaviour. Unknown functions and dispatch misses stay visible (Section 6).
- **No hand edits to generated code.** Never edit `build/<game>/<game>_recomp_*.c`; change the
  generator, analysis or runtime and regenerate.
- **No network use** in the player, runtime or tests without a maintainer decision (Section 5).
- **No invented identity or origin.** An agent never writes a `Signed-off-by:` line, never claims
  someone wrote something they did not, and never hides where code came from.
- **No tags, releases, repository settings or rewritten history** (Section 3).

### What a good pull request looks like

- A **Draft** pull request from a branch on your fork, with **Allow edits by maintainers** left on.
  Related changes may share it; unrelated ones go in separate pull requests, and none is stacked on
  another.
- It says what was wrong, links the issue (`Tracks #123`), and lists what each change does.
- It adds a test that fails before the change and passes after it. A documentation change runs the
  documentation gates instead.
- It lists the exact commands you ran and their results. Say which checks you could not run.
  "CI is green" is not a result, and a local pass is not hosted CI.
- Every commit an AI tool helped with ends with one `Assisted-by: <tool> (<model>)` line per tool,
  for example `Assisted-by: Cursor (Claude Sonnet 5.5)`. Never `Co-Authored-By:` for an AI
  ([details](docs/AI_USAGE.md#disclosing-ai-help-in-commits)).
- Every commit carries a DCO sign-off from the human contributor. The agent does not add it: the
  person reviews the final diff and signs off themselves, with `git commit -s` or
  `git commit --amend -s --no-edit` before pushing ([DCO policy](docs/DCO_POLICY.md)).
- It says where every new file came from (next section).

### New files: what to put in the pull request

Every public file has a provenance entry: where it came from, the evidence, and a hash of its bytes.
The record behind those entries is kept privately by the maintainer, so you cannot add an entry, and
you do not need to. **The maintainer admits new paths.** For each file you add, say in the pull
request:

- that you wrote it (with which AI tool, if any); or
- which project it came from, the exact upstream revision, and its licence. Code from another project
  must be under a licence compatible with GPL-3.0-or-later, keep its notices, and follow the import
  rules in Section 4.

The **Trusted provenance attestation** check stays red on a pull request that adds a file until the
maintainer admits it. That is expected, not something you broke. If you only changed existing files,
run `make contrib-refresh` (Linux) or `mingw32-make contrib-refresh` (Windows) and commit what it
stages, or leave that to the maintainer. Never hand-edit `PUBLIC_EXPORT.json` or
`assets/public_provenance_ledger.json`. If the tool refuses, paste its refusal code in the pull
request; [`CONTRIBUTING.md`](CONTRIBUTING.md#provenance-controls) says what each code needs from you.

## 1. Sources and live preflight

- Resolve disagreements in this order: live source/tests/Makefiles, live GitHub Issues,
  `ISSUES.md`, maintained `docs/` contracts, then README/navigation and dated evidence.
- Before a change, run `git fetch origin`, record the exact `BASE_SHA`, inspect `git status`, and
  check on current `origin/main` that the task is not already done. Without network access, work
  from the last fetched `origin/main` and report `FETCH: NOT_RUN` with that SHA instead of stopping.
- Development network use is expected: `git fetch`, `gh`, public documentation, specifications
  and upstream repositories, and pushes as Section 8 allows. Section 5's offline rule governs
  the shipped player, runtime and tests, not development; a task's "no network" means that
  product rule unless it names git or GitHub.
- Look at open pull requests that touch the issue or the same files. Note any overlap, new paths,
  where new code comes from, whether a game or a PSP is needed, and the smallest proof.
- Stale handoffs, copied chat summaries, ignored local configuration, and research pages
  are not authority when live source or GitHub disagrees.
- A generated, scoped agent-instruction file is not a policy source, even if a more-specific
  path would normally win. This file is the only agent authority.
- Read-only review or diagnosis creates no branch, worktree, commit, PR, or external
  mutation; report the boundary and stop if no implementation was requested.

## 2. Operating modes and autonomy

- One branch per pull request, named for its topic (`docs/...`, `fix/...`, `lle/...`,
  `oracle/...`) with a date or issue suffix.
- Keep the change coherent, avoid stacked pull requests, and leave other people's unmerged work
  alone.
- By default, stop and report after a bounded implementation, its tests, or a Draft pull request.
- Only the maintainer's agents ever integrate. An explicit maintainer mission may authorize
  autonomous integration only when the exact head has required hosted CI green, no open
  review/change request remains, provenance/publication gates pass, no private-input or hardware
  ambiguity remains, and no human-only operation is needed.
- Generic requests to finish, ship, publish, integrate, or do everything do not expand
  mission authority.

## 3. Human-only hard stops

A contributor's agent never does any of the following. The maintainer's agents need explicit
maintainer authorization in the current turn, even under an otherwise autonomous integration
mission:

- provenance attestation or legal clearance on a maintainer's behalf;
- inventing a contributor identity, `Signed-off-by:`, DCO attestation, or source lineage;
- destructive history rewriting, shared-branch force-push, or deleting another agent's work;
- repository security/settings changes unless the mission names the exact setting;
- firmware, flash, NAND, or idStorage writes, and any other action that can permanently alter or damage a device;
- disclosing private inputs, routes, saves, captures, traces, keys, paths, or derived bytes, except the presentation screenshots and clips Section 5 allows.

**Standing hardware authorization.** PSPLink sessions and other non-destructive physical-PSP actions (loading and
running probe modules, USB attach/reset, soft and PC-triggered resets, reading device state, writing Memory Stick
files such as plugin configuration after backing them up) do not need per-turn authorization while a standing
maintainer authorization is recorded in the private workspace and the run follows the hardware-lock protocol
(one holder at a time, session-bound lock, evidence kept private, lock released at the end). The maintainer can
revoke it at any time by saying so; agents must then return to per-turn authorization. A standing authorization
never covers the destructive writes listed above, tags, releases, credentials, or disclosure of private inputs.
Physical steps that need a person (power cycles, media swaps, cable changes) are requested from the maintainer,
never simulated or assumed. This applies to the maintainer's own PSP; a contributor's agent uses a PSP only as
its own user directs.

Agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish GitHub Releases; upload release assets; or change a published version without explicit maintainer authorization in the current turn. Generic instructions such as 'finish', 'ship', 'publish', 'integrate', or 'do everything' do not authorize a version/tag/release operation.

## 4. Provenance and publication

- Every public path has a provenance record. A new path is admitted by the maintainer; a pull
  request that adds one says where each new file came from (see
  [New files](#new-files-what-to-put-in-the-pull-request)). The maintainer's own steps are under
  [Maintainer process](#maintainer-process).
- Deterministic documentation/configuration paths and explicitly synthetic tests may use
  their narrow classes. Never label project implementation as a synthetic fixture.
- Candidate-controlled policy, ledgers, exports, or marker bytes cannot authorize
  themselves. Never fabricate a private record, expand a wildcard, edit hashes to
  self-attest, or use `--no-verify` to bypass publication failure.
- `--provenance-self-consistency` is a developer tripwire only. It checks coverage,
  resolution, and hashes but does not attest provenance. Release readiness requires the
  external trusted ledger.
- Preserve SPDX, copyright, upstream, and third-party notices (sign-off identity: Section 3).
- End agent commits with `Assisted-by: <tool> (<model>)`, never `Co-Authored-By:` ([form and tools](docs/AI_USAGE.md#disclosing-ai-help-in-commits)).
- Keep public source correctness, provenance/publication readiness, private title
  acceptance, physical PSP correctness, visual evidence, and release readiness as
  separate claims.
- Write implementation from specifications, public architecture references, and
  project-owned measurements by default. Never copy or line-by-line translate upstream
  source into a new path except under the import rules below.
- **Import rules.** Source under any licence compatible with GPL-3.0-or-later may be vendored or
  adapted when it works better or lands faster than a rewrite: for example FFmpeg codec subsets
  (the `src/rt/atrac3p/` pattern), uofw (github.com/uofw/uofw, MIT), GPL-compatible emulator
  source (PPSSPP, GPL-2.0-or-later; JPCSP, GPL-3.0), MIT, BSD, Apache-2.0 or zlib code, and CC0 or
  public-domain dedications such as NVIDIA's MIT-licensed dxvk-remix API (maintainer decisions of
  2026-10-20, 2026-10-22 and 2026-10-25; no further decision is needed per import). Each file must
  be pinned to an exact upstream revision, listed in a `PROVENANCE.md` manifest as byte-identical or
  LOCAL DELTA, keep its licence and copyright notices, be credited in `CREDITS.md`, and get a trusted
  record and admission like any new path. An adapted file keeps the upstream copyright header and
  adds a one-line note that it was modified, as the GPL requires; no special path or naming is
  needed beyond that. Emulator-derived units are labelled `derived-translated`
  (`docs/provenance/INDEPENDENCE_MODEL.md`), never "independent". Behaviour claims still need
  tests, and PSP evidence where hardware semantics are claimed. Source under a licence incompatible
  with GPL-3.0-or-later stays excluded.
- Do not rewrite working inherited code only to make it original: replace it when evidence shows
  it is wrong or an LLE path makes it unnecessary (`docs/INDEPENDENCE_CAMPAIGN.md`, re-scope).
- Do not call code "clean-room" or "independent" unless its trusted record supports that
  label (see `docs/provenance/INDEPENDENCE_MODEL.md`); prefer "project-authored".
- Adding or removing a public path changes `assets/public_source_profile.json`, which only the
  maintainer's admission can authorize. `provenance-refresh` and `tools/contrib_refresh.py` refuse
  added or removed paths and only re-key admitted ones. A commit carries the regenerated controls
  for the paths it changes and nothing more; a refresh after merging `main` is a commit of its own.
  Never hand-edit them.

## 5. Private and public boundaries

- Never commit or publish retail executables/ISOs/assets, decrypted modules, generated
  retail C, saves, console-unique or user-specific keys, game-byte Ghidra databases, private
  traces/captures, or private paths, except as the presentation-media bullet below allows.
- Built-in game decryption (maintainer decision, 2026-10-27, the way PPSSPP does it): the player
  and tools decrypt the encrypted executable and modules of a game the user owns, locally, from
  the user's own disc image, into the private per-title folder. The public key tables PPSSPP
  ships may be carried, in the reviewed table `src/core/nk_psp_builtin_keys.c` only (the one path
  the publication audit lets hold key literals). Console-bound values (fuse ID, secure-install
  ID, licence files) come only from the user, locally, and are never committed. Decrypted output
  stays on the user's machine under the rules below. Firmware is never decrypted, even privately:
  updater PSAR archives, the IPL, the Media Engine image and system-menu files are refused by
  name before any decryption step runs, and no key only firmware uses is carried.
- Presentation screenshots and short clips are the exception (maintainer policy deltas of 2026-10-21 for screenshots
  and 2026-10-27 for clips, following established recomp practice in Zelda64Recomp, Unleashed Recompiled and Ship of
  Harkinian): curated PNGs, and H.264 MP4 clips of about 60 seconds at most with the game's own sound, of a title
  running in Nakagawa, including the launcher or library with it loaded, may be published when each is checked for
  personal data (no profile names, user paths or private menus) and carries a notice that the game belongs to its
  publisher, that no game data is in the repository, and that users need their own copy. Raw evidence captures,
  frame and audio dumps, traces and any extracted game asset file (icons, PIC/ICON, textures, audio, fonts) stay
  private.
- Treat `place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and
  local third-party material as sensitive. Never run `git clean -fdx` where inputs exist.
- Public-safe CI and private title routes are different evidence classes. Missing private
  or external-oracle inputs are `NOT_RUN`, `BLOCKED`, or `SKIP`, never silent passes.
- Do not run title or physical-hardware routes for a source/configuration mission unless
  explicitly required. Remote host work stays reversible and non-overlapping.
- Product network rule: offline by default (maintainer decision, 2026-10-22). The player and runtime make no network
  connection unless the user enables a documented, opt-in feature; today there are two: the
  consented prerequisite download, and Check for updates (maintainer decision, 2026-10-28):
  GitHub's Releases API for this repository when the user asks, or once at start if they turned
  that on (off by default), then that release's Windows ZIP on a separate click, kept only if it
  matches the release's published SHA-256. A new egress needs a maintainer decision, a "Network use"
  entry in `docs/SETUP.md`, and an update to the egress allowlist test in
  `tools/test_prereq_fetcher.py`. The reserved `network_mode` setting accepts only `"offline"`,
  and a value a build does not know reads back as offline, never as something more permissive.
- A user may move their own packages and saves to their own devices, for example an Android phone
  (maintainer decision, 2026-10-23); such a transfer is started by the user and goes directly to
  their device, never through a server. No feature uploads game code, game data, decrypted
  modules, packages, saves or guest memory to any server or third party, and nothing built from a
  user's game is ever published or shared. No telemetry. Tests never touch the internet. Network
  HLE stays fail-closed, and no network setting makes a NID miss or refusal permissive.

## 6. Correctness and evidence

- Prefer a small source-level root-cause fix with a failing-before production-path
  regression. Never add loop caps, sleeps, forced returns, fake success, latch hacks,
  arbitrary state writes, invented assets, or VFS aliases that hide missing behavior.
- Unknown NIDs and dispatch misses remain visible. No switch makes a miss permissive: the interpreter
  runs only inside analyzer-owned spans (the PR #118 floor), and every rejection terminates the run.
  Correctness evidence asserts PSP-visible results, state, and wakes.
- Validate complete guest spans and checked size arithmetic before forming bulk host
  pointers.
- `src/rt/recomp.h` is a load-bearing ABI: MIPS `$ra` is `r[31]`, with no separate `lr`.
- Never hand-edit generated `build/<game>/<game>_recomp_*.c`. Change the generator,
  analysis, or runtime and regenerate. Chunk count is dynamic; `FUNCS_PER_CHUNK` is not
  a fixed ABI.
- A private title run proves only its qualified route. A white-box fixture is not a
  production syscall proof; software/Vulkan agreement is not a PSP hardware oracle.
- A mutation is behavioral evidence only when it generates, compiles, executes, and fails
  for the intended semantic reason. Build-only or in-place tracked-file mutations are not
  valid kills.

## 7. Current execution contract

These concise facts are the routed contract. Re-check live source before changing them;
the PR numbers are navigation, not substitutes for source evidence:

- **PR #118:** the production interpreter is a fail-closed AOT-gap correctness floor.
  Only analyzer-owned executable spans have executable authority; unsupported interpreter
  forms fail closed. It is not an all-Allegrex interpreter.
- **PR #126:** a computed `jr`/`jalr` target is latched at transfer before link writes
  and the delay slot, so later register mutation cannot redirect it.
- **PR #127:** linked calls carry explicit `target` and `resume_pc` through
  `dispatch_call`; CALL and TAIL crossings differ, and live `$ra` is not a resume
  descriptor.
- **PR #128:** source-owned cosimulation compares AOT/interpreter traces, writes, memory,
  and architectural state, including cross-tier CALL/TAIL cells. Its negative corpus
  fails closed; a build-only mutant is not a valid semantic kill.
- Nested interpreted calls outside that floor are not implied to work. Claim broader
  coverage only with a new source-owned contract and regression.
- **LLE Phase 1** (`src/rt/cpu_lle.*`, `src/rt/domain_mode.*`): exception entry, `eret`,
  and the import seam are opt-in via `tools/codegen.py --lle-cpu` and `--lle-import-seam`;
  the seam implies `--lle-cpu`. Default output remains byte-identical to pre-LLE output.
  `CpuState` (COP0, transfer metadata, the LL/SC link bit) is versioned by
  `SR_CPUSTATE_ABI_VERSION` in `src/rt/recomp.h`. Values without hardware measurements stay
  behind fail-closed paths and are labelled synthetic until measured on a physical PSP.
- **HLE arguments:** a handler reads arguments 1-4 from `A0`-`A3`. PSP userland is MIPS EABI, so
  arguments 5-8 arrive in `t0`-`t3` (`stack_arg(s, 0..3)`) and only 9+ come from the stack. Check every
  argument read against the function's public signature: a read past the real arguments returns whatever
  the caller left in that register (#1124).

## 8. Workspace, branch, and worktree lifecycle

- Before switching or removing anything, look at `git status`, untracked files, stashes and
  unpushed commits. Preserve unrelated changes and unique evidence. Never use reset, checkout,
  clean, or force operations for convenience.
- Never write repository-local Git identity. A checkout `[user]` block silently re-authors
  later commits there and in every worktree sharing its Git directory. Use the host's
  existing identity or stop and report.
- Push only to your own remote (a contributor's fork, or `origin` for the maintainer's agents), and
  only when the task includes a pull request.
- The maintainer's workspace rules (canonical checkout, temporary worktrees, hand-off to
  integration) are under [Maintainer process](#maintainer-process).

## 9. Validation and gate routing

`docs/CI.md`, `docs/SETUP.md`, and the live workflow define hosted behavior. Local gates
are not hosted-CI evidence; report each as `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`.
Start with `make contrib-check` (Linux) or `mingw32-make contrib-check` (Windows), then run the
focused gate for the area you changed. The table uses the Windows spelling; on Linux, use
`make CC=gcc` in place of `mingw32-make`.

| Changed surface | Focused first gate |
| --- | --- |
| `tools/codegen.py`, analysis, or imports | relevant `tools/test_codegen_*.py` and `python tools/import_audit_gate.py` |
| `guest_interp`, `recomp`, or dispatch | `tools/test_dispatch_c.py`, `tools/test_dispatch_call_boundary.py`, cosim tests, then native selftests |
| scheduler/HLE lifecycle | `tools/test_sched_invariants.py` and `mingw32-make --no-print-directory sched-selftest` |
| FPU/VFPU conversion | relevant `tools/test_*fpu*.py` plus `fp-convert-selftest`/`vfpu-interp-selftest` |
| publication/provenance | `tools/policy_sync.py`, both publication-audit legs, and `tools/modified_file_notice_audit.py` |
| docs and agent policy | `python -m unittest tools.test_lint_docs tools.test_dco_policy`, markdownlint (below), and `python tools/audit_public_issue_links.py` |
| workflow/configuration | `tools/test_ci_paths.py`, pre-commit, and the full applicable hosted matrix |
| LLE CPU / domain modes | `tools/test_cpu_lle.py`, `tools/test_domain_mode.py`, then `mingw32-make native-core-tests` |

Locally, run the focused gates above for the files you changed, then these whenever they apply:

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

Hosted CI is the full Python suite. An exact head whose required hosted CI is green meets the Python-suite
requirement for readiness (Section 2); run the full suite locally only when hosted CI cannot run for the change.
Which modules hosted CI routes, and the Windows-only modules a Windows-host change runs locally, are in
[`docs/CI.md`](docs/CI.md#what-hosted-ci-does-not-run). The PR template's checklist is satisfied the same way.

Windows host pitfalls: [`docs/SETUP.md`](docs/SETUP.md#windows-host-pitfalls). Quick recipes (find an HLE
handler, run one native self-test, capture frames unattended):
[`docs/DEBUGGING.md`](docs/DEBUGGING.md#quick-recipes).

## 10. PR and integration authorization

- When the task includes a pull request, push early: open a Draft PR after the first valid commit.
  Group related work: one PR may carry several related fixes in the same area or a whole feature
  slice, and fewer, larger PRs are better than one PR per finding. Include exact base/head SHAs,
  scope, new-path provenance, failing-before evidence, tests, uncertainty, private/hardware status,
  and blockers.
- Never mark Ready, merge, enable auto-merge, close other PRs, or bypass required checks
  unless Section 2's autonomous-integration conditions hold; local success never means a
  Draft is hosted-CI green. A contributor's agent leaves Ready to its person. A substantive
  correction after Ready returns the PR to Draft and requires fresh exact-head validation.
- Before readiness, fetch `origin/main` and merge it when needed; never rebase or
  force-push a pushed branch. Preserve newer-main behavior, regenerate only required
  canonical metadata, rerun affected gates, and freeze the exact validated head.
- Answer every review thread, an advisory bot's included. How the maintainer lands pull requests
  is under [Maintainer process](#maintainer-process).

## 11. Reporting and cleanup

End an implementation task with exact facts, not a play-by-play. Fields that do not apply say
`N/A`; a contributor's agent puts its branch name in `LANE`.

```text
LANE:
MISSION:
BASE_SHA:
HEAD_SHA:
PR:
STATUS: DRAFT / BLOCKED / READY-QUALITY
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

Say whether any tag, release, asset, version, hardware, or private-input operation was
intentionally not performed. Keep `ISSUES.md` concise and detailed evidence in the issue or PR.
Put build/workspace ergonomics debt in maintained workspace documentation, not in this enduring
contract.

## Maintainer process

This section applies only to agents the maintainer runs. A contributor's agent can skip it. It
covers the maintainer's workspace, how new paths are admitted, and how pull requests land.

### Workspace

- Work only in the canonical checkout or a registered worktree, never the non-repository
  workspace root. If a session starts at a parent folder, select the tree and change directory
  before repository work: the canonical public checkout for read-only integration checks; the
  existing task worktree, or one new registered worktree, for changes; the private authority
  checkout for private-authority work, never a public tree. The workspace root has its own router
  file that names these places.
- A mutating mission uses one dedicated temporary worktree and branch per pull request. Reuse it
  when it already starts at the required exact base. A follow-up to a PR handed to integration is
  a new branch from `origin/main`. There are no persistent per-agent lanes.
- Before switching or removing anything, inspect `git worktree list --porcelain`, branch, status,
  untracked files, stashes, and remote heads. A checkout owning local `main` is protected.
- Fast-forward a named canonical checkout's local `main` only when it is clean, has no unique
  untracked/stashed/unpushed work, has no dependent worktree, and an ordinary fast-forward is
  safe. Otherwise leave it untouched and report `BLOCKED_CANONICAL_MAIN`.
- Treat a temporary worktree as disposable only after inspecting dirty/untracked state, preserving
  unique evidence, and removing disposable build/log output. Use ordinary worktree removal, then
  prune stale administrative records when safe; never establish a routine
  `git worktree remove --force` rule.
- Delete a temporary merged/superseded branch only after verifying it is checked out nowhere else,
  has no unpushed commits, and contains no retained evidence.
- Never reconnect public branches to private-history refs. The public and private histories stay
  disjoint.

### Admitting new paths

- Before a new implementation-bearing path, inspect `tools/provenance_ledger.py` and its
  classifier. Use a genuine path-specific trusted record or stop with `PROVENANCE_UNRESOLVED`.
- Hosted attestation accepts an added or removed public path only with a maintainer policy-delta
  authority in the private authority repository, plus an exact trusted record for a new
  implementation path. Both must be on the private repository's `main` before the hosted
  attestation runs. Step by step, with the failure each skipped step produces:
  [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).

### Landing pull requests

- Hand a PR to integration only when its exact head is pushed and every review thread, an
  advisory bot's included, is answered and resolved.
- From hand-off to integration until merge or explicit hand-back, a PR's branch and worktree
  belong to the integrator, which merges `main`, refreshes provenance and pushes from that same
  worktree. Do not edit, stage, commit, merge or push there.
- The integrator lands from one landing run, one PR at a time. When the queue is long, the desk lands
  pull requests in batches instead: a linear batch branch with one commit per folded PR, one CI and
  CodeQL run for the batch, then a rebase-merge. A path-adding PR needs a re-key after
  another path-adding PR lands unless both were batch-admitted
  ([landing order](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path)).
  Never hold a ready pull request for an unrelated one.
- At the end of a mission, also state whether canonical main was fast-forwarded or left
  `BLOCKED_CANONICAL_MAIN`, and which worktree and branch were kept or removed.
