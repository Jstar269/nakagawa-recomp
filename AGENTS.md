# AGENTS.md — Nakagawa Recomp

This repository's single operating contract for automated agents. Live source, tests,
Makefiles, and maintained documents remain authoritative for implementation details.

## Where to start

Work only in the canonical checkout or a registered worktree, never the non-repository
workspace root. If a session starts at a parent, select the tree and change directory
before repository work:

- read-only integration checks: the canonical public checkout;
- changes: the existing task worktree, or one new registered worktree under Section 8;
- private-authority work: the private authority checkout, never a public tree.

Read this contract in the selected tree before acting. Tool-specific guidance files may
only route here.

## 1. Sources and live preflight

- Resolve disagreements in this order: live source/tests/Makefiles, live GitHub Issues,
  `ISSUES.md`, maintained `docs/` contracts, then README/navigation and dated evidence.
- Before a mutation, run `git fetch origin`, record the exact `BASE_SHA`, inspect
  `git status`, and verify on current `origin/main` that the task is not already fixed.
  Without network access, work from the last fetched `origin/main` and report
  `FETCH: NOT_RUN` with that SHA instead of stopping.
- Development network use is expected: `git fetch`, `gh`, public documentation, specifications
  and upstream repositories, and pushes as Section 8 allows. Section 5's offline rule governs
  the shipped player, runtime and tests, not development; a mission's "no network" means that
  product rule unless it names git or GitHub.
- Inspect open PRs touching the issue or files. Record overlap, expected ownership, new
  paths, provenance disposition, private-input or hardware needs, and smallest proof.
- Stale handoffs, copied chat summaries, ignored local configuration, and research pages
  are not authority when live source or GitHub disagrees.
- A generated, scoped agent-instruction file is not a policy source, even if a more-specific
  path would normally win. This is the only agent authority.
- Read-only review or diagnosis creates no branch, worktree, commit, PR, or external
  mutation; report the boundary and stop if no implementation was requested.

## 2. Operating modes and autonomy

- A mutating mission uses one dedicated temporary worktree and branch per pull request.
  Reuse it when it already starts at the required exact base. A follow-up to a PR handed
  to integration (Section 8) is a new branch from `origin/main`.
- Name branches for their topic (`docs/...`, `fix/...`, `lle/...`, `oracle/...`) with a
  date or issue suffix. There are no persistent per-agent lanes.
- Keep the mission coherent, avoid stacked PRs, and preserve other agents' unmerged work.
- Default autonomy is STOP/REPORT after a bounded implementation, tests, or draft PR.
  An explicit mission may authorize autonomous integration only when the exact head has
  required hosted CI green, no open review/change request remains, provenance/publication
  gates pass, no private-input or hardware ambiguity remains, and no human-only operation
  is needed.
- Generic requests to finish, ship, publish, integrate, or do everything do not expand
  mission authority.

## 3. Human-only hard stops

The following always require explicit maintainer authorization in the current turn, even
under an otherwise autonomous integration mission:

- provenance attestation or legal clearance on a maintainer's behalf;
- inventing a contributor identity, `Signed-off-by:`, DCO attestation, or source lineage;
- destructive history rewriting, shared-branch force-push, or deleting another agent's work;
- repository security/settings changes unless the mission names the exact setting;
- firmware, flash, NAND, or idStorage writes, and any other action that can permanently alter or damage a device;
- disclosing private inputs, routes, saves, captures, traces, keys, paths, or derived bytes, except the presentation screenshots Section 5 allows.

**Standing hardware authorization.** PSPLink sessions and other non-destructive physical-PSP actions (loading and
running probe modules, USB attach/reset, soft and PC-triggered resets, reading device state, writing Memory Stick
files such as plugin configuration after backing them up) do not need per-turn authorization while a standing
maintainer authorization is recorded in the private workspace and the run follows the hardware-lock protocol
(one holder at a time, session-bound lock, evidence kept private, lock released at the end). The maintainer can
revoke it at any time by saying so; agents must then return to per-turn authorization. A standing authorization
never covers the destructive writes listed above, tags, releases, credentials, or disclosure of private inputs.
Physical steps that need a person (power cycles, media swaps, cable changes) are requested from the maintainer,
never simulated or assumed.

Agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish GitHub Releases; upload release assets; or change a published version without explicit maintainer authorization in the current turn. Generic instructions such as 'finish', 'ship', 'publish', 'integrate', or 'do everything' do not authorize a version/tag/release operation.

## 4. Provenance and publication

- Before a new implementation-bearing path, inspect `tools/provenance_ledger.py` and its
  classifier. Use a genuine path-specific trusted record or stop with
  `PROVENANCE_UNRESOLVED`.
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
- Import rules, maintainer-approved 2026-10-20 and extended 2026-10-22: FFmpeg codec subsets
  (the `src/rt/atrac3p/` pattern), uofw (github.com/uofw/uofw, MIT), and GPL-compatible
  emulator source (PPSSPP, GPL-2.0-or-later; JPCSP, GPL-3.0) may be vendored or adapted when
  they work better or land faster than a rewrite. Each file must be pinned to an exact
  upstream revision, listed in a `PROVENANCE.md` manifest as byte-identical or LOCAL DELTA,
  keep its licence and copyright notices, be credited in `CREDITS.md`, and get a trusted
  record and admission like any new path. An adapted file keeps the upstream copyright
  header and adds a one-line note that it was modified, as the GPL requires; no special path
  or naming is needed beyond that. Emulator-derived units are labelled
  `derived-translated` (`docs/provenance/INDEPENDENCE_MODEL.md`), never "independent".
  Behaviour claims still need tests, and PSP evidence where hardware semantics are claimed.
  Since 2026-10-25 the maintainer pre-approves any source under a licence compatible with
  GPL-3.0-or-later (for example MIT, BSD, Apache-2.0, zlib, or a CC0 or public-domain
  dedication, such as NVIDIA's MIT-licensed dxvk-remix API): it may be vendored or adapted
  under these same rules without a further decision. Source under a licence incompatible with
  GPL-3.0-or-later stays excluded.
- Do not rewrite working inherited code only to make it original: replace it when evidence shows
  it is wrong or an LLE path makes it unnecessary (`docs/INDEPENDENCE_CAMPAIGN.md`, re-scope).
- Do not call code "clean-room" or "independent" unless its trusted record supports that
  label (see `docs/provenance/INDEPENDENCE_MODEL.md`); prefer "project-authored".
- Adding or removing a public path changes `assets/public_source_profile.json`; hosted
  attestation accepts that only with a maintainer policy-delta authority in the private
  authority repository, plus an exact trusted record for a new implementation path.
  `provenance-refresh` and `tools/contrib_refresh.py` refuse added or removed paths and only
  re-key admitted ones. A commit carries the regenerated controls for the paths it changes and
  nothing more; a refresh after merging `main` is a commit of its own. Never hand-edit them.
- Step by step for a pull request that adds a public path, with the failure each skipped step produces:
  [`docs/PUBLICATION_READINESS.md`](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).

## 5. Private and public boundaries

- Never commit or publish retail executables/ISOs/assets, decrypted modules, generated
  retail C, saves, console-unique or user-specific keys, game-byte Ghidra databases, private
  traces/captures, or private paths, except as the presentation-screenshot bullet below allows.
- Built-in game decryption (maintainer decision, 2026-10-27, the way PPSSPP does it): the player
  and tools decrypt the encrypted executable and modules of a game the user owns, locally, from
  the user's own disc image, into the private per-title folder. The public key tables PPSSPP
  ships may be carried, in the reviewed table `src/core/nk_psp_builtin_keys.c` only (the one path
  the publication audit lets hold key literals). Console-bound values (fuse ID, secure-install
  ID, licence files) come only from the user, locally, and are never committed. Decrypted output
  stays on the user's machine under the rules below. Firmware is never decrypted, even privately:
  updater PSAR archives, the IPL, the Media Engine image and system-menu files are refused by
  name before any decryption step runs, and no key only firmware uses is carried.
- Presentation screenshots are the exception (maintainer policy delta, 2026-10-21, following established recomp
  practice in Zelda64Recomp, Unleashed Recompiled and Ship of Harkinian): curated PNGs of a title running in Nakagawa,
  including the launcher or library with it loaded, may be published when each is checked for personal data and carries
  a notice that the game belongs to its publisher, that no game data is in the repository, and that users need their
  own copy. Raw evidence captures, frame dumps, traces and any extracted game asset file (icons, PIC/ICON, textures,
  audio, fonts) stay private.
- Treat `place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and
  local third-party material as sensitive. Never run `git clean -fdx` where inputs exist.
- Public-safe CI and private title routes are different evidence classes. Missing private
  or external-oracle inputs are `NOT_RUN`, `BLOCKED`, or `SKIP`, never silent passes.
- Do not run title or physical-hardware routes for a source/configuration mission unless
  explicitly required. Remote host work stays reversible and non-overlapping.
- Product network rule: offline by default (maintainer decision, 2026-10-22). The player and runtime make no network
  connection unless the user enables a documented, opt-in feature; today the only one is the
  consented prerequisite download. A new egress needs a maintainer decision, a "Network use"
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

- Before switching or removing anything, inspect `git worktree list --porcelain`, branch,
  status, untracked files, stashes, and remote heads. A checkout owning local `main` is
  protected.
- Fast-forward a named canonical checkout's local `main` only when it is clean, has no
  unique untracked/stashed/unpushed work, has no dependent worktree, and an ordinary
  fast-forward is safe. Otherwise leave it untouched and report
  `BLOCKED_CANONICAL_MAIN`.
- Preserve unrelated user changes and unique evidence. Never use reset, checkout, clean,
  or force operations for convenience.
- Treat a temporary worktree as disposable only after inspecting dirty/untracked state,
  preserving unique evidence, and removing disposable build/log output. Use ordinary
  worktree removal, then prune stale administrative records when safe; never establish a
  routine `git worktree remove --force` rule.
- Delete a temporary merged/superseded branch only after verifying it is checked out
  nowhere else, has no unpushed commits, and contains no retained evidence.
- Never write repository-local Git identity. A checkout `[user]` block silently re-authors
  later commits there and in every worktree sharing its Git directory. Use the host's
  existing identity or stop and report.
- From hand-off to integration until merge or explicit hand-back, a PR's branch and worktree
  belong to the integrator, which merges `main`, refreshes provenance and pushes from that same
  worktree. Do not edit, stage, commit, merge or push there.
- Push only to `origin` when the mission explicitly includes a PR. Never reconnect public branches
  to private-history refs.

## 9. Validation and gate routing

`docs/CI.md`, `docs/SETUP.md`, and the live workflow define hosted behavior. Local gates
are not hosted-CI evidence; report each as `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`.

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

- When requested, open one focused Draft PR after the first valid commit. Include exact
  base/head SHAs, scope, new-path provenance, failing-before evidence, tests,
  uncertainty, private/hardware status, and blockers.
- Never mark Ready, merge, enable auto-merge, close other PRs, or bypass required checks
  unless Section 2's autonomous-integration conditions hold; local success never means a
  Draft is hosted-CI green. A substantive correction after Ready returns the PR to Draft
  and requires fresh exact-head validation.
- Before readiness, fetch `origin/main` and merge it when needed; never rebase or
  force-push a pushed branch. Preserve newer-main behavior, regenerate only required
  canonical metadata, rerun affected gates, and freeze the exact validated head.
- Hand a PR to integration only when its exact head is pushed and every review thread, an
  advisory bot's included, is answered and resolved. The integrator lands one PR at a time from
  one landing run. A path-adding PR needs a re-key after another path-adding PR lands unless both
  were batch-admitted ([landing order](docs/PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path)).
  Never hold a ready pull request for an unrelated one.

## 11. Reporting and cleanup

End an implementation mission with exact facts, not a play-by-play:

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

State whether canonical main was fast-forwarded or left `BLOCKED_CANONICAL_MAIN`, what
worktree/branch was retained or removed, and whether any tag, release, asset, version,
hardware, or private-input operation was intentionally not performed. Keep `ISSUES.md`
concise and detailed evidence in the issue or PR. Put build/workspace ergonomics debt in
maintained workspace documentation, not in this enduring contract.
