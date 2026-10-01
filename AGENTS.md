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
- Inspect open PRs touching the issue or files. Record overlap, expected ownership, new
  paths, provenance disposition, private-input or hardware needs, and smallest proof.
- Stale handoffs, copied chat summaries, ignored local configuration, and research pages
  are not authority when live source or GitHub disagrees.
- A generated, scoped agent-instruction file is not a policy source, even if a more-specific
  path would normally win. This is the only agent authority.
- Read-only review or diagnosis creates no branch, worktree, commit, PR, or external
  mutation; report the boundary and stop if no implementation was requested.

## 2. Operating modes and autonomy

- A mutating mission uses at most one dedicated temporary worktree and branch. Reuse it
  when it already starts at the required exact base; never create a second.
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
- firmware, flash, NAND, idStorage, PSPLink, physical PSP, or other hardware actions;
- disclosing private inputs, routes, saves, captures, traces, keys, paths, or derived bytes.

Agents must not create, move, push, or delete Git tags; create, edit, delete, publish, or unpublish GitHub Releases; upload release assets; or change a published version without explicit maintainer authorization in the current turn. Generic instructions such as 'finish', 'ship', 'publish', 'integrate', or 'do everything' do not authorize a version/tag/release operation.

No agent may create, edit, publish, unpublish, or attach assets to a release to work
around a blocked PR. Normal code, documentation, and configuration integration remains
subject to Section 2.

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
- Preserve SPDX, copyright, upstream, and third-party notices. Agents must never invent a
  human DCO identity or add `Signed-off-by:` on anyone's behalf.
- Keep public source correctness, provenance/publication readiness, private title
  acceptance, physical PSP correctness, visual evidence, and release readiness as
  separate claims.
- Write implementation from specifications, public architecture references, and
  project-owned measurements. Never copy or line-by-line translate emulator or other
  upstream source (PPSSPP, JPCSP, sal063, or similar) into a new path.
- Do not call code "clean-room" or "independent" unless its trusted record supports that
  label (see `docs/provenance/INDEPENDENCE_MODEL.md`); prefer "project-authored".
- When a public path is added or removed, update `assets/public_source_profile.json` and
  have the maintainer admit the policy delta and any trusted record in the private
  authority repository. Then run
  `mingw32-make provenance-refresh` against that trusted ledger and commit the regenerated
  `PUBLIC_EXPORT.json` and public ledger. Never hand-edit generated controls.

## 5. Private and public boundaries

- Never commit or publish retail executables/ISOs/assets, decrypted modules, generated
  retail C, saves, keys, game-byte Ghidra databases, private traces/captures, or private
  paths.
- Treat `place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, `build/`, and
  local third-party material as sensitive. Never run `git clean -fdx` where inputs exist.
- Public-safe CI and private title routes are different evidence classes. Missing private
  or external-oracle inputs are `NOT_RUN`, `BLOCKED`, or `SKIP`, never silent passes.
- Do not run title or physical-hardware routes for a source/configuration mission unless
  explicitly required. Remote host work stays reversible and non-overlapping.

## 6. Correctness and evidence

- Prefer a small source-level root-cause fix with a failing-before production-path
  regression. Never add loop caps, sleeps, forced returns, fake success, latch hacks,
  arbitrary state writes, invented assets, or VFS aliases that hide missing behavior.
- Unknown NIDs and dispatch misses remain visible. Correctness evidence uses fail-closed
  dispatch (`SR_DISPATCH_FATAL=1` or the current equivalent) and asserts PSP-visible
  results, state, and wakes.
- Validate complete guest spans and checked size arithmetic before forming bulk host
  pointers.
  `src/rt/recomp.h` is a load-bearing ABI: MIPS `$ra` is `r[31]`, with no separate `lr`.
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
- **LLE Phase 1** (`src/rt/cpu_lle.*`, `src/rt/domain_mode.*`): `CpuState` ABI v2 carries
  COP0 and transfer metadata. Exception entry, `eret`, and the import seam are opt-in via
  `tools/codegen.py --lle-cpu` and `--lle-import-seam`; the seam implies `--lle-cpu`.
  Default output remains byte-identical to pre-LLE output. Values without hardware
  measurements stay behind fail-closed paths and are labelled synthetic until measured on
  a physical PSP.

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
- Push only to `origin` when the mission explicitly includes a PR. Never reconnect public branches
  to private-history refs.

## 9. Validation and gate routing

`docs/CI.md`, `docs/SETUP.md`, and the live workflow define hosted behavior. Local gates
are not hosted-CI evidence; report each as `PASS`, `FAIL`, `SKIP`, `BLOCKED`, or `NOT_RUN`.

| Changed surface | Focused first gate |
| --- | --- |
| `tools/codegen.py`, analysis, or imports | Python suite; relevant `tools/test_codegen_*.py` and import audit |
| `guest_interp`, `recomp`, or dispatch | `tools/test_dispatch_c.py`, `tools/test_dispatch_call_boundary.py`, cosim tests, then native selftests |
| scheduler/HLE lifecycle | `tools/test_sched_invariants.py` and `mingw32-make --no-print-directory sched-selftest` |
| FPU/VFPU conversion | relevant `tools/test_*fpu*.py` plus `fp-convert-selftest`/`vfpu-interp-selftest` |
| publication/provenance | `tools/policy_sync.py`, both publication-audit legs, and `tools/modified_file_notice_audit.py` |
| docs and agent policy | `tools/lint_docs.py`, Markdown lint, focused policy tests, and public-link audit |
| workflow/configuration | `tools/test_ci_paths.py`, pre-commit, and the full applicable hosted matrix |
| LLE CPU / domain modes | `tools/test_cpu_lle.py`, `tools/test_domain_mode.py`, then `mingw32-make native-core-tests` |

`native-core-tests` also runs on hosted Linux, so native selftests must build and link
there without Windows-only libraries. Check with a Linux shell such as WSL before pushing.

At minimum, when applicable, run:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
python -m pre_commit run --all-files
```

`workflow_dispatch` has no narrowing inputs. `tools/ci_paths.py` forces the full matrix
for a manual run and sets `allow_substantive=true`; draft status does not suppress
substantive gates. Verify these facts against the live workflow before documenting them.

## 10. PR and integration authorization

- When requested, open one focused Draft PR after the first valid commit. Include exact
  base/head SHAs, scope, new-path provenance, failing-before evidence, tests,
  uncertainty, private/hardware status, and blockers.
- Never mark Ready or merge unless Section 2's autonomous-integration conditions hold.
  Cheap local success never means a Draft is hosted-CI green.
- Before readiness, fetch `origin/main` and merge it when needed; never rebase or
  force-push a pushed branch. Preserve newer-main behavior, regenerate only required
  canonical metadata, rerun affected gates, and freeze the exact validated head.
- Never merge, enable auto-merge, close other PRs, or bypass required checks without the
  authority in Section 2. A substantive correction after Ready returns the PR to Draft
  and requires fresh exact-head validation.

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
