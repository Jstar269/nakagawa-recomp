# AGENTS.md — Nakagawa Recomp

Nakagawa Recomp statically translates a PSP PRX/ELF into C (`tools/codegen.py`), links the
generated translation with the native C runtime (`src/rt/`), and currently targets the PSP release
of *Hot Shots Tennis: Get a Grip*. `interface/` is a separate Next.js dashboard and is not part of
the native runtime build.

## Sources of truth

Use these in this order when project surfaces disagree:

1. **Source code, tests, and Makefile** for implementation behavior.
2. **GitHub Issues** for actionable defects, priorities, acceptance criteria, and partial-resolution state.
3. [`ISSUES.md`](ISSUES.md) as a concise status dashboard linked to those canonical issues.
4. Maintained `docs/` pages — start with [`docs/README.md`](docs/README.md) (index) and
   [`docs/NEXT_SESSION.md`](docs/NEXT_SESSION.md) (current session handoff) before new engineering.
5. Dated investigation/history documents only as historical evidence that must be re-verified before reuse.

Do not rely on `opencode.json` or an auto-loaded Markdown tracker; `opencode.json` is local/ignored
state and the former comment-triggered OpenCode workflow is intentionally disabled pending any
future security review.

Before changing a subsystem, read its canonical GitHub issue plus the relevant source/tests and
maintained documentation. Also inspect open PRs that touch the same files or issue so a new branch
does not unknowingly fork or overwrite newer work.

## Build & run (Windows / MSYS2 UCRT64)

For HST, prefer `hst_manager.ps1`; it supplies HST's required `GAME_BASE=0 GAME_ENTRY=0` and
canonical private-input paths.

- `.\hst_manager.ps1 -Action BuildFull` — regenerate pipeline output and compile.
- `.\hst_manager.ps1 -Action BuildFast` — runtime-focused rebuild where valid.
- `.\hst_manager.ps1 -Action Run` — run with the configured GUI/runtime route.
- `.\hst_manager.ps1 -Action Test` — run the manager's narrower selftest route.
- `.\hst_manager.ps1 -Action Verify` — run the full non-interactive local verification suite.
- `.\hst_manager.ps1 -Action VisualOracle ...` — run a bounded private visual-oracle route when the
  change requires game/runtime evidence.
- Other actions: `Inspect`/`DiffFunc`/`FindSymbol` (per-function work), `Fuzz` (VFPU differential
  fuzzer), `Clean` (local tracking files). `hst.ps1` is the simpler fail-closed entry point
  (`Doctor`/`Build`/`Rebuild`/`Play`/`Verify`/`Manager`); `hst_manager.ps1` is the expert console.

Both managers require `pwsh` 7.6+; Windows PowerShell 5.1 is not a supported host (the Makefile
compile step itself invokes `pwsh` for `copy_build_assets.ps1`).

Do not encode historical build-duration estimates as guarantees; they vary substantially by host,
compiler, storage, and whether generated artifacts already exist.

A direct HST Make invocation is:

```bash
mingw32-make GAME_NAME=hst GAME_ELF=place_game_here/EBOOT.elf GAME_BASE=0 GAME_ENTRY=0 all
```

The Makefile `all` (and the generated-code VFPU fuzz route) is intentionally **two-phase**:
`$(MAKE) pipeline` followed by a second Make invocation that reparses generated chunks.
`CHUNK_OBJS` uses `$(wildcard)` at parse time. Do not collapse this into `all: pipeline compile`;
a clean build can otherwise omit the generated chunk objects.

The supported Windows build uses the PATH-resolved MSYS2 UCRT64 `gcc`. The Makefile deliberately
treats GNU Make's built-in `CC=cc` (`origin=default`) as unset, selects `gcc`, and preserves explicit
environment or command-line overrides. `hst_manager.ps1` relies on the same Makefile policy. Use
`mingw32-make --no-print-directory compiler-info` to inspect the effective value and origin.

Direct Make callers must supply `VULKAN_SDK` (export it or pass on the command line); nothing is
pinned. Resolution order: explicit `-VulkanSdk`/`VULKAN_SDK`, then the LunarG-installed `VULKAN_SDK`
env, then the newest SDK under `C:/VulkanSDK` that actually contains `Include/vulkan/vulkan.h`
(CI builds with `VULKAN_SDK=/ucrt64`). The manager discovers and validates the SDK automatically.

HST builds compile in `SR_DATA_EXPECTED_COUNT=56672`; a runtime route is only valid evidence when
the extracted-data index reports exactly 56672 files — any other count must be discarded. When the
private `src/rt/pgf.c`/`pgd.c` backends are absent the build fail-closes into
`pgf_unavailable.c`/`pgd_unavailable.c` stubs (`PUBLIC_SAFE=1`, `-DSR_PUBLIC_SAFE`). That is
intentional public-safe behavior, not a missing file; `compiler-info` prints `PUBLIC_SAFE`.

## Private inputs

`place_game_here/`, `logs/`, `memstick/`, `keys/`, `oracle/`, `fs/`, and local `third_party/`
checkouts are ignored and required for full HST runtime routes, but must never enter Git history.
The current canonical `place_game_here/` layout includes:

- `place_game_here/EBOOT.elf` — decrypted flat build input;
- `place_game_here/ISO/` — user-supplied game ISO;
- `place_game_here/EXTRACTED/decrypted/` — required decrypted game PRXs;
- `place_game_here/EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted/` — extracted title assets used by current routes.

`python tools/extract_xb.py` regenerates `xbdata_extracted/`; it requires `third_party/libxb` at
the pinned commit `ce6df78e5ca99241dd2bbbd68ca485e34003d760` (see
`docs/ISSUE196_DIRECT_XB.md`).

Legacy root links can exist locally, but documentation/tests should prefer the canonical ignored
layout unless they are explicitly testing the legacy fallback.

**`git clean -fdx` would delete the irreplaceable private inputs — never run it in this tree.**

Never commit game binaries/assets, decrypted PRXs, private oracle traces, extracted retail content,
Ghidra databases containing game bytes, private version/PGD keys, or local path-bearing runtime
captures.

## Search hygiene

Bare repo-wide Glob/Grep patterns match into ignored private trees (`place_game_here/`,
`third_party/`, `build/`, `logs/` — tens of thousands of files). Scope searches to `src/`,
`tools/`, `docs/`, `interface/`, or a named file, and never echo private-input contents into a
response or commit.

## Generated-code rules

`build/<game>/<game>_recomp_*.c` is generated. Never hand-edit it.

- Change `tools/codegen.py`, analysis/import tooling, or runtime behavior instead.
- The number of generated chunk files is **dynamic**, based on function count and
  `FUNCS_PER_CHUNK`; do not assume HST always has exactly eight chunks.
- Generated translation units intentionally compile with conservative `-O0`/memory-saving flags.
- `ge.c` has its own `-O2` rule. General runtime objects currently use the Makefile's `$(CFLAGS)`,
  whose repository default begins at `-O0`.
- After generator changes, use a full pipeline rebuild and verify that no stale generated object is
  being linked.

## Project rule: no band-aids

Root-cause fixes only. Do not add loop caps, forced returns, latch-flip hacks, fabricated resource
success, or "return 0 to unblock" behavior merely to advance a route.

Address-specific/game-specific compatibility behavior is semantic debt. Any such behavior must be:

- represented in the compatibility-override inventory where applicable;
- linked to concrete evidence and a deterministic regression or route;
- narrowly scoped to the proven contract;
- assigned a retirement criterion.

[#20](https://github.com/Jstar269/nakagawa-recomp/issues/20) tracks retirement/proof of this surface.

## Correctness traps

- **Dispatch misses:** do not count a route as verified if it advanced through an unsafe
  continue-on-dispatch-miss mode. Use the fatal/default diagnostic route (`SR_DISPATCH_FATAL=1`)
  when establishing new correctness evidence.
- **Unknown NIDs:** there is no general permission to convert an unimplemented PSP operation into
  success. Implement/register the real behavior or keep the gap visible.
- **`CpuState` is a load-bearing ABI:** the current runtime structure in `src/rt/recomp.h` has no
  separate `lr` field; MIPS `$ra` is `r[31]`. Coordinate every mirror/consumer when the layout changes.
- **Debug switches:** many legacy Boolean `SR_*` switches are presence-based, so the literal string
  `"0"` can still enable them. Unset those variables unless the implementing code explicitly parses values.
- **Renderer agreement is not an external oracle:** software-vs-Vulkan parity can localize a
  divergence but does not prove PSP hardware behavior.
- **Callbacks/synchronization:** route progress is not proof of PSP kernel fidelity. Callback,
  semaphore, mutex, async-I/O, wait, lifecycle, and timeout semantics have dedicated GitHub issues.
- **Guest-memory hardening:** validating only a starting address is not a whole-span proof. Preflight
  the complete readable/writable span and checked size arithmetic before a bulk host access. Where the
  PSP-visible contract permits, invalid input must be rejected before partial host or guest side effects.
- **Dependency advisories:** changing a version string is not remediation. Confirm the resolved lock
  graph contains no affected version, keep `package.json` and lockfiles synchronized, and exercise the
  real consumer path in addition to package-manager resolution.

## Evidence and claim discipline

A passing test proves only the path it actually executes. Use the strongest accurate evidence label:

1. **Production dispatch:** the test reaches the real registered NID/entry through the production
   dispatch path and asserts PSP-visible return/output/state/wake behavior.
2. **Production helper/white-box:** real implementation code executes, but fixture setup or entry is
   test-specific.
3. **Model/reference:** a separate model or reference implementation is exercised.
4. **Source-shape/static assertion:** text/structure is checked without executing the public behavior.

Do not describe category 2-4 evidence as category 1. In particular, manually placing a thread into a
wait state does not prove the wait syscall entered that state, defining a CB NID does not prove a CB
path executed, and an expansion test that does not assert pointer identity does not prove in-place
reallocation.

Before saying an issue or acceptance criterion is complete:

- inspect the exact base-to-head diff and changed-file list;
- map every claimed criterion to a concrete implementation/test/evidence artifact;
- verify the test contains a failing-before/passing-after assertion for the claimed behavior where
  practical;
- state residual criteria explicitly instead of broadening a partial pass into subsystem completion.

## Architecture entry points

- `src/rt/recomp.c` — generated-code/runtime dispatch and shared execution support.
- `src/rt/sched.c` — cooperative PSP-thread scheduling, lifecycle, waits, virtual time.
- `src/rt/sr_coro.c` — host coroutine abstraction (Windows fibers and supported POSIX path).
- `src/rt/hle.c` — NID/HLE registry plus major PSP kernel/user HLE behavior.
- `src/rt/atrac3p/` + `src/rt/atrac3p_bridge.c` — ATRAC3+ decoder (FFmpeg n4.4-derived,
  LGPL-2.1-or-later) and the HLE decode bridge behind `sceAtracDecodeData` (#32). Any target that
  compiles `hle.c` needs the same `-Isrc/rt/atrac3p/...` include flags, or `avcodec.h` fails on
  `libavutil/attributes.h`.
- `src/rt/ge.c` — software GE comparison rasterizer.
- `src/rt/gpu_sdl3vk/` — SDL3/Vulkan host/input/rendering path.
- `src/rt/iso.c` — UMD/ISO and host-filesystem mapping support.
- `src/ref/` — separate C++ reference interpreter used by verification/selftests, not the normal runtime executable.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the maintained architecture summary.

## Verification

Run tests proportional to the change and state exactly what was executed:

```powershell
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree
pre-commit run --all-files
mingw32-make --no-print-directory compiler-info
```

`publish_audit.py` audits paths from Git but reads content from whichever source you name, and
the two are different trees. `--worktree` reads the bytes on disk and is what an interactive
check means. The bare `--tracked-only` form reads staged blobs — correct for the pre-commit hook,
which stashes unstaged changes so the index *is* the tree, and correct for release-export gates,
but blind to an unstaged edit. Every run prints which source it used; quote that line as evidence
rather than a bare "publication audit: OK".

For a broad or integration candidate, prefer `.\hst_manager.ps1 -Action Verify` as the canonical
non-interactive local aggregate gate (Python unit suite, sched/profiler/heap/asset-index/HLE-thread
selftests, the VFPU table-loader selftest (`vfpu-tables-selftest`, #187), the watchpoints-file
parser selftest (`watchpoints-file-selftest`, #188), `vfpu-interp-selftest`, `src/ref` selftest,
`import_audit_gate.py`, `publish_audit.py` over both content sources
(`--tracked-only` and `--tracked-only --worktree`), `gpu-coherence-selftest` and
`gpu-capture-selftest`; exit 77 = Vulkan/validation layer unavailable → SKIP), then add the
subsystem-specific native/runtime route required by the change (audio: `atrac3p-selftest` and
`atrac3p-bridge-selftest` are source-owned; `atrac3p-title-accept` is the private acceptance route
and exits 77 when the private stream is absent → SKIP).

Docs changes: `npx --yes markdownlint-cli2@0.23.1`. Dashboard changes:
`cd interface && npm ci && npm test && npm run lint && npm run typecheck && npm run build` (plus
`npm audit` for dependency changes). The full `make verify` path needs PPSSPP oracle traces passed
as `CODEGEN_ORACLE`, `MICROTEST_MODULE`, `MICROTEST_ORACLE` and reports **BLOCKED** without them.

Also run the relevant native, scheduler, reference-interpreter, renderer, dashboard, or private-input
route for the changed subsystem. External-oracle gates that lack their private inputs must be
reported **blocked/unavailable**, not silently treated as passing.

Synthetic/reference-interpreter agreement is useful regression evidence. Private-EBOOT or real GUI
routes add different evidence. Neither should be overstated as complete PSP correctness proof.

**Local verification is not GitHub CI.** Hosted Actions is active again (see `ISSUES.md`), but a
branch is "CI-green" only when its exact-head jobs actually executed and passed. Jobs are
path-gated by `tools/ci_paths.py` and aggregated by the `CI required` job (`tools/ci_required.py`);
if Actions are blocked, skipped, or fail before steps run, record the exact-head local gates and
the workflow limitation separately.

## Branch and pull-request discipline

- Start work from the intended authoritative base. Before coding, record `git status`, the base SHA,
  and the current open PRs that overlap the same files/issues.
- Keep unrelated issues in separate branches/PRs. A large integration branch is not a substitute for
  independently reviewable fixes.
- If a PR intentionally depends on another unmerged PR, make the dependency explicit and normally set
  the dependent PR's base to the parent branch. Otherwise rebase/cherry-pick the focused commits onto
  current `main` so the PR does not silently include the parent's entire diff.
- Recheck base/head and overlap immediately before review or merge; another PR may have advanced the
  same source or documentation in the meantime.
- Do not merge automatically. A merge is a separate maintainer decision after exact-head review,
  required gates, DCO/provenance checks, and issue-closure semantics are resolved.

## Work-tracking discipline

- Search GitHub Issues before filing a new defect.
- Put the detailed problem statement, source evidence, acceptance criteria, and partial-resolution
  comments in the canonical GitHub issue.
- Keep `ISSUES.md` concise and update its linked status row in the same change when the current
  milestone materially changes.
- When adding a confirmed defect or known limitation, update the canonical GitHub issue and the
  relevant `ISSUES.md` dashboard link in the same change when applicable; label hypotheses and
  informational notes explicitly.
- Move resolved narratives and superseded hypotheses to `docs/STATUS_HISTORY.md` or leave them in
  the closed issue/PR history; do not maintain competing live trackers.
- Link PRs to their issue(s) and record partial merges when acceptance criteria remain open.

## Licensing, provenance, and DCO

Preserve existing SPDX, copyright, and provenance notices. Do not infer a license merely from a
neighboring file or upstream repository root.

For new or materially derived code/data:

- record the exact upstream source/revision;
- preserve applicable notices;
- disclose material translation/reimplementation lineage, including AI-assisted translation;
- keep third-party and generated data clearly distinguishable from independently authored code.

The PGF PPSSPP/JPCSP licensing chain is a current publication blocker tracked in
[#98](https://github.com/Jstar269/nakagawa-recomp/issues/98), replacement-font licensing in
[#99](https://github.com/Jstar269/nakagawa-recomp/issues/99), the full-history secret/privacy audit
in [#102](https://github.com/Jstar269/nakagawa-recomp/issues/102), and qualified PGD/amctrl
distribution review in [#104](https://github.com/Jstar269/nakagawa-recomp/issues/104). Do not
"fix" any of these by changing SPDX text or generic NOTICE wording without resolving the underlying
provenance evidence. Engineering review packets are evidence for qualified review, not legal
clearance. The repository-level declaration is GPL-3.0-or-later ([LICENSE](LICENSE), `NOTICE.md`)
while many source files retain GPL-2.0-or-later — both are deliberate; preserve per-file SPDX.

**AI tools and automated agents must never invent or add a `Signed-off-by:` identity on behalf of a
human, company, or the tool itself.** DCO sign-off is a contributor rights certification. An agent may
prepare a patch or commit for review, but the actual contributor/maintainer must deliberately make the
required attestation under the project's policy. Do not retroactively fabricate historical sign-offs.

## Publication / security guardrails

- Keep Actions dependencies immutable/pinned according to the repository's publication policy.
- Do not reintroduce the former comment-triggered OpenCode workflow without a new security review of
  actor authorization, token/secret scope, immutable dependencies, and abuse bounds.
- Keep `.pre-commit-config.yaml` shared and reviewed if contributor docs continue to depend on it.
- For npm/dashboard security changes, synchronize `interface/package.json` and
  `interface/package-lock.json`, verify the resolved graph against the current advisory's affected/fixed
  ranges, then run `npm ci`, `npm audit`, tests, lint, typecheck, build, and the actual affected consumer
  path. Never claim an advisory fixed by moving to another version still in the affected range.
- Run repository publication/secret checks before any visibility change, but remember current-tree
  checks do not replace a full Git-history scan.
- Remote GitHub rulesets, security settings, MFA, and visibility controls require explicit owner/
  repository-settings verification; never infer them from source-tree configuration.
