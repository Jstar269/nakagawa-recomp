# AGENTS.md — Nakagawa Recomp

This is the repository's canonical operating contract for automated agents. It is deliberately
short and enduring; live source, tests, Makefiles, and the maintained documents remain the source
of truth for implementation details.

## 1. Sources and live preflight

- Resolve disagreements in this order: live source/tests/Makefiles, live GitHub Issues, `ISSUES.md`,
  maintained `docs/` contracts, then README/navigation and dated historical evidence.
- Before a mutation, run `git fetch origin`, record the exact `BASE_SHA`, inspect `git status`,
  and verify the task is not already fixed on current `origin/main`.
- Inspect open PRs that touch the issue or files. Record overlaps, expected ownership, new paths,
  provenance disposition, private-input or hardware needs, and the smallest proving evidence.
- Never use stale handoff notes, copied chat summaries, ignored local configuration, or a research
  page as authority when live source or GitHub disagrees.
- A read-only review or diagnosis does not create a branch, worktree, commit, PR, or external
  mutation. Report the boundary and stop when no implementation was requested.

## 2. Operating modes and autonomy

- A mutating mission may use at most one dedicated temporary worktree and branch. Reuse the current
  temporary worktree when it already starts at the required exact base; do not create a second one.
- Persistent agent lanes are `ai/claude`, `ai/codex`, `ai/antigravity`, `ai/opencode`, and
  `ai/freebuff`. Do not commit to a persistent lane unless it is assigned to the mission.
- One active mission PR is allowed per persistent lane. Keep a mission coherent, avoid stacked PRs,
  and do not discard another agent's unmerged work.
- Default autonomy is STOP/REPORT after a bounded implementation, tests, or draft PR. An explicit
  mission may authorize autonomous integration only after all of these hold: the exact head has
  required hosted CI green, no unresolved review/change request remains, provenance/publication
  gates pass, no private-input or hardware ambiguity remains, and no human-only operation is needed.
- A generic request to finish, ship, publish, integrate, or do everything does not expand the
  mission's authority.

## 3. Human-only hard stops

The following always require explicit maintainer authorization in the current turn, even when an
integration mission is otherwise autonomous:

- provenance attestation or legal clearance on a maintainer's behalf;
- inventing a contributor identity, `Signed-off-by:` trailer, DCO attestation, or source lineage;
- destructive history rewriting, shared-branch force-push, or deletion of another agent's work;
- repository security/settings changes unless the mission names the exact setting;
- firmware, flash, NAND, idStorage, PSPLink, physical PSP, or other hardware actions;
- disclosing private inputs, routes, saves, captures, traces, keys, paths, or derived bytes.

"Agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish GitHub Releases; upload release assets; or change a published version without explicit maintainer authorization in the current turn. Generic instructions such as 'finish', 'ship', 'publish', 'integrate', or 'do everything' do not authorize a version/tag/release operation."

No agent may create, edit, publish, unpublish, or attach assets to a release as a workaround for a
blocked PR. Normal code, documentation, and configuration integration remains subject to Section 2.

## 4. Provenance and publication

- Before creating a new implementation-bearing path, inspect `tools/provenance_ledger.py` and the
  classifier. Use a genuine path-specific trusted record or stop with `PROVENANCE_UNRESOLVED`.
- Deterministic documentation/configuration paths and explicitly synthetic tests may use their
  defined narrow classification; do not label project implementation as a synthetic fixture.
- Candidate-controlled policy, ledger, export, or marker bytes cannot authorize themselves. Never
  fabricate a private ledger record, expand a wildcard record, edit hashes to self-attest, or use
  `--no-verify` to bypass a publication failure.
- `--provenance-self-consistency` is a developer tripwire only. It checks coverage, resolution, and
  hashes but does not attest provenance. Release readiness requires an external trusted ledger.
- Preserve SPDX, copyright, upstream, and third-party notices. Agents must never invent a human DCO
  identity or add `Signed-off-by:` on anyone's behalf.
- Keep public source correctness, provenance/publication readiness, private title acceptance,
  physical PSP correctness, visual evidence, and release readiness as separate claims.

## 5. Private and public boundaries

- Never commit or publish retail executables/ISOs/assets, decrypted modules, generated retail C,
  saves, keys, Ghidra databases containing game bytes, private traces/captures, or private paths.
- Treat `place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and local
  third-party material as sensitive. Never run `git clean -fdx` in a checkout containing inputs.
- Public-safe CI and a private title route are different evidence classes. Missing private or
  external-oracle inputs are `NOT_RUN`, `BLOCKED`, or `SKIP`, never silent passes.
- Do not run a title route or physical hardware route for a source/configuration mission unless it
  is explicitly required. Remote host work must remain reversible and non-overlapping.

## 6. Correctness and evidence

- Prefer a small source-level root-cause fix with a failing-before production-path regression.
  Do not add loop caps, sleeps, forced returns, fake success, latch hacks, arbitrary state writes,
  invented assets, or VFS aliases that hide missing behavior.
- Unknown NIDs and dispatch misses remain visible. Correctness evidence uses fail-closed dispatch
  (`SR_DISPATCH_FATAL=1` or the current equivalent) and asserts PSP-visible results/state/wakes.
- Validate complete guest spans and checked size arithmetic before forming bulk host pointers.
  `src/rt/recomp.h` is a load-bearing ABI: MIPS `$ra` is `r[31]`, with no separate `lr`.
- Generated `build/<game>/<game>_recomp_*.c` is never hand-edited. Change the generator, analysis,
  or runtime source and regenerate; chunk count is dynamic and `FUNCS_PER_CHUNK` is not a fixed ABI.
- A private title run proves only its qualified route. A white-box fixture is not a production
  syscall proof; software/Vulkan agreement is not a PSP hardware oracle.
- Mutations count as behavioral evidence only when they generate, compile, execute, and fail for the
  intended semantic reason. Build-only or in-place tracked-file mutations are invalid kills.

## 7. Current execution contract

The following concise facts are the current routed contract. Re-check live source when changing it;
the issue numbers are navigation, not a substitute for source evidence:

- **#118:** production interpreter is a fail-closed AOT-gap correctness floor. Only analyzer-owned
  executable spans have executable authority; unsupported interpreter forms fail closed. It is not
  an all-Allegrex interpreter.
- **#126:** a computed `jr`/`jalr` target is latched at transfer before link writes and the delay
  slot, so later register mutation cannot change the selected target.
- **#127:** linked calls carry explicit `target` and `resume_pc` through `dispatch_call`; CALL and
  TAIL crossings are distinct, and live `$ra` is not used as a resume descriptor.
- **#128:** source-owned cosimulation compares AOT/interpreter traces, writes, memory, and
  architectural state, including cross-tier CALL/TAIL cells. The negative corpus fails closed;
  a build-only mutant is an invalid semantic kill.
- Nested interpreted calls outside the documented floor are not implied to be supported. Do not
  claim broader coverage without a new source-owned contract and regression.

## 8. Workspace, branch, and worktree lifecycle

- Inspect `git worktree list --porcelain`, branch, status, untracked files, stashes, and remote
  heads before switching or removing anything. A checkout that owns local `main` is protected.
- For a named canonical checkout, fast-forward local `main` only when it is clean, has no unique
  untracked/stashed/unpushed work, has no dependent worktree, and an ordinary fast-forward is safe.
  Otherwise leave it untouched and report `BLOCKED_CANONICAL_MAIN`.
- Preserve unrelated user changes and unique evidence. Do not use reset, checkout, clean, or force
  operations to make a worktree look convenient.
- A temporary worktree is disposable only after inspecting dirty/untracked state, preserving unique
  evidence, and removing disposable build/log output. Remove it with ordinary worktree removal,
  then prune stale administrative records when safe; do not introduce a routine
  `git worktree remove --force` rule.
- Delete a temporary merged/superseded branch only after verifying it is not checked out elsewhere,
  has no unpushed commits, and contains no retained evidence. Never delete an active persistent lane.
- Push only to `origin` when the mission explicitly includes a PR. Never reconnect public branches
  to private-history refs.

## 9. Validation and gate routing

`docs/CI.md`, `docs/SETUP.md`, and the live workflow define exact hosted behavior. Local gates are
not hosted-CI evidence; report each as `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`.

| Changed surface | Focused first gate |
| --- | --- |
| `tools/codegen.py`, analysis, or imports | Python suite; relevant `tools/test_codegen_*.py` and import audit |
| `guest_interp`, `recomp`, or dispatch | `tools/test_dispatch_c.py`, `tools/test_dispatch_call_boundary.py`, cosim tests, then native selftests |
| scheduler/HLE lifecycle | `tools/test_sched_invariants.py` and `mingw32-make --no-print-directory sched-selftest` |
| FPU/VFPU conversion | relevant `tools/test_*fpu*.py` plus `fp-convert-selftest`/`vfpu-interp-selftest` |
| publication/provenance | `tools/policy_sync.py`, both publication-audit legs, and modified-file notice audit |
| docs and agent policy | `tools/lint_docs.py`, Markdown lint, focused policy tests, and public-link audit |
| workflow/configuration | `tools/test_ci_paths.py`, pre-commit, and the full applicable hosted matrix |

At minimum, when applicable, run:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
pre-commit run --all-files
```

The workflow's `workflow_dispatch` has no narrowing inputs. `tools/ci_paths.py` forces the full
matrix for a manual run, and `allow_substantive` is true there; draft pull requests suppress
substantive jobs until ready. Verify those facts against the live workflow before documenting them.

## 10. PR and integration authorization

- Open one focused Draft PR after the first valid commit when the mission requests a PR. Include
  exact base/head SHAs, scope, new-path provenance, failing-before evidence, tests, uncertainty,
  private/hardware status, and blockers.
- Do not mark Ready or merge unless Section 2's explicit autonomous-integration conditions hold.
  Never call a Draft locally CI-green merely because cheap gates passed.
- Before readiness, fetch `origin/main`, rebase once if needed, resolve conflicts preserving newer
  main behavior, regenerate only required canonical metadata, rerun affected gates, and freeze the
  exact validated head. Do not continuously rebase a healthy Draft.
- Do not merge, enable auto-merge, close other PRs, or bypass required checks without the authority
  described above. A substantive correction after Ready returns the PR to Draft and needs fresh
  exact-head validation.

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

State whether the canonical main checkout was fast-forwarded or left as
`BLOCKED_CANONICAL_MAIN`, what worktree/branch was retained or removed, and whether any tag,
release, asset, version, hardware, or private-input operation was intentionally not performed.
Keep `ISSUES.md` concise; put detailed evidence in the issue or PR. Record build/workspace
ergonomics debt in the maintained workspace documentation instead of growing this contract into a
history notebook.
