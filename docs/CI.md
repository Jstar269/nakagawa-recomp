# Continuous integration and dependency maintenance

The public workflow (`.github/workflows/ci.yml`) is intentionally one
always-present workflow with a cheap classifier followed by job-level
applicability checks. The classifier lives in
[`tools/ci_paths.py`](../tools/ci_paths.py), and its regression tests are in
[`tools/test_ci_paths.py`](../tools/test_ci_paths.py). The stable aggregate is
implemented by [`tools/ci_required.py`](../tools/ci_required.py) and tested in
[`tools/test_ci_required.py`](../tools/test_ci_required.py). It fails closed when
the change set cannot be determined, so an uncertain checkout runs the broader
gates instead of silently skipping them.

Name-status parsing is structural: ordinary records contain exactly one path,
rename/copy records contain both endpoints, and any malformed record becomes a
history-unavailable sentinel that selects the full matrix. `CI required` also
requires every `RUN_*` and `ALLOW_SUBSTANTIVE` output to be an explicit
case-insensitive `true` or `false`; missing or malformed control state is red.

## Workflow topology

| Event/change | Jobs that run | Jobs intentionally skipped |
| --- | --- | --- |
| Draft pull request | the same path-applicable jobs as a ready pull request, plus classification, hygiene/security, and CI required | only jobs irrelevant to the changed paths |
| Ready pull request, docs-only | classification, hygiene/security, Markdown, CI required | Python/native, Windows |
| Ready pull request, native C/build files | classification, hygiene/security, Python tooling, native/translation, Windows, CI required | none of the substantive public gates |
| Ready pull request, recipe-only edit of phony targets in `Makefile` | classification, hygiene/security, Python tooling on the Makefile-coupled modules over two shards, native/translation, Windows, CI required | the Python modules that cannot observe a Makefile recipe |
| Ready pull request, ordinary tools Python file | classification, hygiene/security, Python tooling, CI required | native/translation, Windows |
| Status-only transition (draft <-> ready) | no run: `.github/workflows/ci.yml` does not list `ready_for_review` or `converted_to_draft` | every gate, because the head did not change |
| Workflow/CI configuration | classification, hygiene/security, Python tooling, native/translation, Windows, CI required | none of the substantive public gates |
| Dependency-only metadata (`.github/dependabot.yml`) | classification, hygiene/security, CI required | Python/native, Windows |
| Mixed documentation/native changes | classification, hygiene/security, Python tooling, native/translation, Windows, CI required | none of the applicable product gates |
| Ordinary push to `main` after a validated merge | classification, hygiene/security, Markdown when needed, compact main smoke, CI required | expensive platform matrix; the merged PR carried it |
| Workflow push to `main` | the full applicable validation above plus main smoke | none of the substantive public gates |
| Manual `workflow_dispatch` | the full matrix and full Python tier, regardless of paths | none |
| Nightly schedule (`17 3 * * *` UTC) | the full matrix and full Python tier | none |
| Merge-queue group (`merge_group`, `checks_requested`) | the full matrix, as for a manual dispatch, regardless of paths; every required check reports on the group commit | none |

### Draft pull requests

The OpenSSF Scorecard workflow (`.github/workflows/scorecards.yml`) runs weekly and on pushes to
`main`. It uploads SARIF to GitHub code scanning; publishing results to scorecard.dev is disabled.
Triage findings through the usual issue process. The workflow does not change branch-protection
settings.

A draft pull request now receives the same path-applicable substantive gates as
a ready pull request. No `Ready for review` transition or manual
`workflow_dispatch` is needed to discover whether the exact head passes.
`.github/workflows/ci.yml` declares its `pull_request` trigger list as exactly
`opened`, `synchronize` and `reopened` - the events that can carry a new head -
so a `Ready for review` (or back-to-draft) transition, which cannot change the
head, does not start a second run over a tree that already carries its
validation. That workflow still cancels superseded runs, and docs-only or other
irrelevant jobs remain skipped by the classifier. `workflow_dispatch` remains
available when a maintainer deliberately wants the complete matrix regardless of
changed paths.

The `CI required` job is the stable aggregate status required by branch
protection. It runs with `always()`, accepts an intentionally skipped irrelevant
job, and fails when a classifier-applicable job fails, is cancelled, or is
otherwise incomplete. A failed hygiene/security job or public-export job is never
hidden by the aggregate.

### ClusterFuzzLite C fuzzing

Pull requests run the pinned ClusterFuzzLite workflow in
`.github/workflows/clusterfuzzlite.yml`. Its C target
`tests/fuzzers/test_nk_json_fuzzer.c` calls the public `nk_json_parse` API
with bounded synthetic bytes and exercises nesting at the `NK_JSON_MAX_DEPTH`
boundary. The project configuration is `.clusterfuzzlite/project.yaml`; the
Clang/libFuzzer build is defined by `.clusterfuzzlite/Dockerfile` and
`.clusterfuzzlite/build.sh`. AddressSanitizer code-change fuzzing is capped
at 60 seconds with a 4 KiB maximum input. The target does not use title files,
retail inputs, or hardware.

### Project website (GitHub Pages)

`.github/workflows/pages.yml` publishes the project website,
<https://jstar269.github.io/nakagawa-recomp/>, from [`docs/index.html`](index.html). The
repository's Pages source is "GitHub Actions". The page is one static file with inline CSS and
two small inline scripts (the theme picker, remembered in the visitor's own browser, and the phone
menu), and no web fonts, external scripts or third-party requests. It shows two launcher screenshots,
`docs/media/launcher-library.png` and `docs/media/launcher-building.png`, with their notice, and link
previews show the share card `docs/images/brand/og-card.png`. The artifact also carries the presentation
clip `docs/media/hst-rally.mp4` for the README's link. The build job copies an explicit list of
files into the artifact, so nothing else in the tree is published, and fails when the page
names a local file the list does not copy. Before it uploads the artifact, it runs
`tools/test_site_mobile.py` on the assembled site: headless Chromium (Playwright, hash-pinned)
renders the page at 360x780, 390x844, 430x932 and 1280x800 in Auto with a light system, Auto with
a dark system, picked Light and picked Dark, and fails on sideways overflow, a section whose
background does not match the active theme, text below WCAG AA contrast, a phone tap target
under 44 px, a theme picker that does not switch the whole page, or a missing image or link
target. Locally the browser half reports SKIP when Playwright or its Chromium is missing; the
workflow sets `NK_SITE_TEST_REQUIRED=1` so it cannot skip there. A pull request that touches
those paths, or the test, runs the build job only; a push to `main`, or a manual run on `main`, also deploys through
`actions/deploy-pages`. Only the deploy job holds `pages: write` and `id-token: write`, and
the `github-pages` environment accepts deployments from `main` alone. To preview the site,
open `docs/index.html` from a checkout: its relative image paths resolve the same way. Keep
the page's status text in step with `README.md`, `docs/COMPATIBILITY.md` and the release notes.

### Hygiene beside the substantive gates

The Python, native and Windows jobs wait only for `classify` and the
public-export audit (`needs: [classify, public_export]`). They do not wait for
`hygiene`, which runs beside them. Before this change they waited for hygiene too,
so its all-files pre-commit run, Betterleaks history scan and canaries (98 s median
across 40 recent pull requests, as measured from their job timings) sat in front of
every expensive gate. The public-export audit stays a dependency: it is the
publication-integrity boundary, it runs in about 52 s, and it keeps the candidate
export ordered before any substantive job starts. `CI required` still requires the
hygiene result (`HYGIENE_RESULT`) and the public-export result, so no failure is
hidden. The trade-off is that a pull request that fails hygiene now also spends
runner time on the expensive gates that started beside it. `main_smoke` is not
changed: it runs on pushes to `main` and on manual and merge-queue runs (the full-matrix
events), never on pull requests, and it is not a substantive gate.

The full-tree pre-commit run retains the publication audit and the Betterleaks
current-tree scan; the hook runs Betterleaks in directory mode over the files it
is given (every tracked file under `--all-files`, the staged files at the commit
stage). The hook is commit-stage only: at pre-push pre-commit supplies no file
names, which would turn directory mode into a whole-working-tree scan. Hygiene then runs an explicit Betterleaks reachable-history
scan in git mode and the synthetic canary gate. It also runs the core purity gate,
[`tools/purity_gate.py`](../tools/purity_gate.py): a hexadecimal literal in PSP user RAM
(`0x08800000`-`0x09ffffff`) in `src/rt/` code fails it, outside title-config files, native
self-tests, registered NIDs and the reviewed allowlist in that file (one entry per literal,
with its reason). A fix for one game goes in its title config, not the core. Markdown linting is separate from the native and Windows jobs. Those jobs remain
synthetic/public-input gates; no private game input is put in Actions.

The Windows job also runs `mingw32-make production-smoke` in the existing MSYS2 UCRT64/GCC,
SDL3, and Vulkan environment. That target generates its PSP-shaped input from committed source,
uses the ordinary loader/import/analyzer/codegen pipeline, links the complete public-safe
production runtime and real driver, and then reaches a registered HLE NID through the scheduler.
Its pass condition is a relocation-dependent guest-memory sentinel checked by the production
driver. The link map and runtime markers make reduced `gate_stub` substitution or omitted critical
objects fail closed. This is a production-composition integration test, not PSP-hardware or private
title acceptance evidence.

The Windows job also runs `production-smoke-gap`: the same fixture with its helper omitted from
native emission at build time, proving region A reaches the omitted guest address through the
ordinary production `dispatch()` seam. Analyzer-owned executable-span registration permits only
those guest bytes to enter the fail-closed interpreter; the gate then requires a registered AOT
region-B handoff, real HLE call, and final `0x00001235` production-driver assertion.

The Windows job also runs a headless staging smoke after it links the player (#308). Because the
runner has no PSPDEV, `python fixtures/showcase/showcase.py stage-iso` wraps the source-owned
display guest in the showcase's synthetic TEST00007 identity. The built `nakagawa_player.exe
--stage-only` must then report `STAGING_RESULT status=PASS` within 60 seconds. LOCALAPPDATA,
APPDATA and USERPROFILE point under the runner temporary directory. The Linux `native_tools` job
stages the PSPDEV-built scene instead. Neither run is PSP-hardware or consumer-ISO evidence.

When `tools/ci_paths.py` routes the native jobs, both hosts run `psmf-media-selftest`, the
source-authored PSMF/H.264 fixture behind the `sr_h264` decoder seam (#283), each on its own
backend. The Windows job runs it in its `selftests` part on Media Foundation
(`src/rt/h264_mf.c`), then requires `psmf-media-selftest-csc-mutant` to be killed. The Linux
`native_tools` job runs it on the vendored FFmpeg subset (`H264_FFMPEG=1`). When no decoder
instance can be created, the selftest reports `SKIP` for its decode checks and still exits 0, so
the Windows step fails on any `SKIP` line. Neither run is private media acceptance or
PSP-hardware evidence.

The Windows job also runs `cosim-selftest` and `cosim-mutants`. The first executes the same
source-owned guest bytes twice — once as generated native code, once through the production
interpreter floor — and reports the first difference in the canonical instruction trace, the
ordered guest writes, the guest memory window, or the architectural state vector. The second
rebuilds that comparator against deliberately mutated copies of the interpreter and requires each
defect class to fail the gate; a mutant that only breaks the build is rejected as `INVALID`, not
counted as a kill. Both are source-owned and need no game input. See
[`fixtures/cosim/README.md`](../fixtures/cosim/README.md) for the comparison contract and the
limits of the evidence.

Both hosts run `hle-thread-selftest`, the executable production-HLE suite that links
`src/rt/hle.c` (#306): the Windows job in its `selftests` part, and the Linux `native_tools` job
after the pinned SDL3 install, because the suite links SDL3 for the scheduler's host clock. Each
host runs its own arms of `hle.c`'s file, directory and host-data seams. A few checks exist on
Windows only, because what they check is Win32: the `FindFirstFileW` error classifier, the
`CreateProcessW` command-line quoting of the exit-game respawn, and the `mklink` junction (Linux
plants a symbolic link in its place).

Most Linux jobs run on Ubuntu 26.04. The `native_tools` job stays on Ubuntu 24.04 because its
translation gate installs `gcc-mipsel-linux-gnu`, which is absent from the Ubuntu 26.04 package
archive ([Ubuntu 24.04 package](https://packages.ubuntu.com/noble/gcc-mipsel-linux-gnu)). The job
archive ([Ubuntu 24.04 package](https://packages.ubuntu.com/noble/gcc-mipsel-linux-gnu)). It builds
SDL3 from its pinned 3.4.18 release commit with X11 and Vulkan enabled, and SDL3_ttf 3.2.2 from upstream commit
`a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b`, with vendored FreeType and HarfBuzz enabled.
The SDL3 and SDL3_ttf staged installs are cached under keys naming their pinned source versions
and commits, the SDL3 pin where applicable, the gcc and cmake versions, and the workflow file
hash. A cache hit skips the build while the pkg-config and runtime feature checks still run. The
SDL3_ttf build asserts that CMake selected vendored FreeType and HarfBuzz; a runtime probe prints
their versions and fails if either backend is unavailable. The job installs Mesa's Vulkan drivers, Vulkan tools,
the Khronos validation layer, Xvfb, and the X11/XTest development libraries needed by the pinned
SDL3 build. `vulkaninfo --summary` selects the lavapipe ICD and must list llvmpipe. The
`gpu-capture-selftest` runs through Xvfb with SDL's X11 driver and the validation layer. Its
terminal present-fault cases each get a fresh process, because the renderer must not reuse a
device after a terminal failure; an exit 77 skip fails the gate. A separate step runs
`gpu-coherence-selftest` the same way on lavapipe: the Vulkan backend's render-target coherence,
texture cache, per-draw state template, filtering and blend cases against the software GE, where
an exit 77 skip also fails. The job also runs
`make CC=gcc player-ui-tests` with SDL's dummy video and software render drivers, configures and
builds the CMake player with `BUILD_PLAYER=ON`, runs CTest, and requires the `nakagawa_player
--help` usage marker. It downloads the PSPDEV archive identified by
[`pspdev.lock.json`](../assets/upstream/pspdev.lock.json), checking its
SHA-256 before use. The job runs `make CC=gcc showcase-linux` to build and boot the two
source-owned showcase packages with dummy SDL video and audio drivers. A failed smoke uploads its
`runtime.log` files and frame captures as the `showcase-smoke-linux-attempt-<n>` artifact. After a
passing smoke, the job passes the generated
TEST00007 ISO to the CMake-built player's bounded `--stage-only` route and requires
`STAGING_RESULT status=PASS`. This is build, test, and headless staging evidence for the named
synthetic fixtures. Separately, the job runs `make CC=gcc player-ui-regressions`: scripted native
player UI event and recovery flows, which are UI evidence rather than staging evidence. The test
harness (`tests/native/test_player_ui.py`) forces SDL's dummy video and software render drivers for
every player it spawns, so the gate is headless without any step environment. The target runs the
suite as four parallel shards (`PLAYER_UI_SHARDS`, `NAKAGAWA_PLAYER_UI_SHARD`) that together run
every test exactly once; each test already uses its own temporary data roots. The CMake player is configured with CMake's default, unoptimized build type, so
the gate covers only that configuration: an optimized Release configuration fails under the global
`-Werror` on existing truncation warnings in `src/core/nk_launch.c`, a separate defect, and this
job does not claim that configuration builds. Consumer ISO compatibility remains in the works
(#306); these checks do not establish PSP hardware acceptance or interactive desktop presentation.

## Local readiness before opening a pull request

Discover the available build and verification surfaces first:

```bash
mingw32-make --no-print-directory help
```

For the public checkout's inner loop, run the public-safe composition before
the strict authority-bound gate:

```bash
mingw32-make --no-print-directory check
```

`check` covers documentation and policy checks, the provenance record and
disposition gap check, both publication-audit legs, the native host-core tests,
and a fast Python subset. It does not replace `make readiness`: readiness
additionally verifies the exact candidate against the external detailed ledger
and therefore remains `BLOCKED` when `NK_TRUSTED_LEDGER` is unavailable.
`make provenance-refresh` is the single local command for regenerating the
tracked public controls. It calls the same `generate_ephemeral_controls()`
implementation as the hosted provenance attestation, reads the trusted public
ledger from the exact base commit, and requires the external detailed ledger
through `NK_TRUSTED_LEDGER`. Stage the intended candidate changes first. The
target stages the generated controls and the profile when `--apply-policy` is
requested; it does not stage the rest of the worktree. The committed ledger
stays a per-path document (no `refresh` audit block), and the export is computed
from those generated ledger bytes in the same invocation, so a second pass is
not needed. For contributors without access to the private trusted ledger,
`python tools/contrib_refresh.py` allows refreshing both public controls for
changed existing public paths from the public controls alone (PR #849).

The `provenance_record_gap.py --check` gate (`make provenance-record-gap-check`)
enforces that every upstream-derived production code path (under `src/` or
`tools/`, excluding `.md`/`.txt` notice and format documents) has an explicit
disposition in [`docs/INDEPENDENCE_CAMPAIGN.md`](INDEPENDENCE_CAMPAIGN.md). Paths with
`status=missing` fail closed. A reviewed baseline (`KNOWN_DISPOSITION_GAPS`)
allows existing gaps to shrink but never grow; stale baseline entries fail
closed. The check is wired into `make check`, `make readiness`, pre-commit, and
hosted CI (`native_tools` and `main_smoke`).

The base defaults to `merge-base(HEAD, origin/main)`. When the pull request's
exact base differs, set `PROVENANCE_BASE_SHA` to its full 40-character commit
SHA before running `mingw32-make provenance-refresh`. A missing detailed
authority or base ledger is a named failure; the command does not construct
either trusted input. A changed publication profile also requires both
`PROVENANCE_TRUSTED_CANDIDATE_POLICY` and
`PROVENANCE_POLICY_DELTA_AUTHORITY`, each naming an external reviewed input.

Run the strict aggregate, then the Python modules for the files you changed. Treat any
failure as blocking. Hosted CI runs the full Python suite; run it locally only when
hosted CI cannot run for the change ([what hosted CI does not run](#what-hosted-ci-does-not-run)):

```bash
NK_TRUSTED_LEDGER=<external detailed ledger> make readiness
```

`make readiness` runs the publication/policy checks in cheapest-first order and stops
at the first failure. Prefer it to assembling the checklist by hand.

### What `readiness` runs, if you need a step alone

```bash
python tools/policy_sync.py
python tools/lint_docs.py
python tools/provenance_record_gap.py --check
python tools/publish_audit.py --tracked-only --public-scope --provenance-self-consistency
python tools/publish_audit.py --tracked-only --worktree --public-scope --provenance-self-consistency
python tools/provenance_attest_verify.py --repo . --candidate <exact HEAD sha> --base <exact BASE sha>     --require-immutable-revisions --trusted-ledger <external detailed ledger>     --workdir <scratch outside the repo>
git diff --check <exact BASE sha>..HEAD
```

Pass **exact commit SHAs**, not `origin/main`. A moving ref stops naming the
branch point as soon as it advances, and the hosted job passes the immutable
SHAs from the pull-request event for exactly that reason.

### publish_audit passing is not the attestation gate passing

These two are routinely confused, and the confusion is the single most common
reason a branch looks finished and is not:

| Gate | Compares against | Can detect an unapproved path or content change |
| --- | --- | --- |
| `publish_audit` | the candidate's **own checked-in ledger** | **no** — the candidate supplies both sides |
| `provenance_attest_verify` | the **external private authority** | yes; normal mode checks path authority and content binding; strict mode also checks legacy blob approvals |

A tree can report `publication audit: OK (<N> tracked files)` and
`verdict: FAIL (<N> fatal findings)` at the same commit. "Gates green" that means
only the first is not evidence that the branch can merge.

The attestation verifier is also the one most easily forgotten. Existing
implementation paths remain authorized across ordinary revisions; automated
content binding and CI validate each new commit without a second private blob
approval. A genuinely new implementation path still needs exact external path
authority, and `--require-reviewed-blobs` remains available for high-assurance
legacy runs. See [`docs/PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) for
the authority boundaries and what each class needs.

### Regenerating every generated file at once

Several committed files are generator output, and a test or gate fails when the
committed copy differs from a fresh generation. Run one command instead of the
generator each failure names:

```bash
python tools/regen_all.py            # rewrite and stage the stale files
python tools/regen_all.py --check    # exit 1 if any is stale; write nothing
python tools/regen_all.py --list     # the steps, their outputs and what is skipped
```

Every staleness check prints `python tools/regen_all.py` as its fix. The steps
run in a fixed order:

| Step | Generator | Committed output |
| --- | --- | --- |
| recomp helper inventory | `tools/recomp_helper_inventory.py` | `docs/RECOMP_HELPER_INVENTORY.md` |
| NID name table | `tools/gen_nidnames.py` | `src/rt/nid_names.h` |
| HLE baseline and census block | `tools/hle_manifest.py` | `tools/import_audit_baseline.json`, the generated block in `docs/HLE_AND_WORKAROUND_INVENTORY.md` |
| waits census | `tools/waits_census.py` | `docs/PSP_INTR_WAITS_CENSUS.md` |
| public title catalog | `tools/title_catalog_codegen.py` | `src/core/generated/nk_title_catalog.{h,c}` |
| PGF golden vectors | `tools/test_pgf_validate_parity.py --regenerate` | `tests/native/pgf_golden_vectors.h` |
| provenance controls (last) | `tools/contrib_refresh.py` | `assets/public_provenance_ledger.json`, `PUBLIC_EXPORT.json` |

The provenance refresh runs last because the ledger hashes the staged tree, so
the tool stages the files it regenerated before it runs. Stage your own changes
first (`git add`); with unstaged edits the refresh refuses with
`CANDIDATE_INDEX_STALE`. It also refuses a new or removed public path and a
changed publication profile. Those need maintainer authority, so the tool
reports them as `SKIP` and points at
[`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md#step-by-step-a-pull-request-that-adds-a-public-path).

The tool never refreshes `tools/import_audit_baseline.json` over a coverage
downgrade (a dedicated or controlled-unsupported NID becoming fake-success or
missing). It fails and names the NIDs; fix the handler, or review the downgrade
and run `python tools/hle_manifest.py --write-baseline` yourself.

Some generators are listed but not run, and `--list` says why:

- embedded shaders (`make shaders GLSLC=...`): regenerating needs `glslc`, whose
  version the manifest records; `make shader-verify` checks the committed files;
- profile-zero prebuilt digests: rebuilding needs the pinned PSPDEV toolchain;
  CI checks `SHA256SUMS`;
- the import-stub census and the benchmark report: both need private inputs
  (decrypted title images, the local telemetry database), and neither output is
  committed.

The hand-kept lists the tests pin, such as the fake-success exceptions in
`tools/test_hle_manifest.py`, are review decisions, not generator output, so the
tool does not touch them.

### The generated control files must be in the commit

Adding a tracked file, or changing one, makes three generated files stale:
`assets/public_source_profile.json` (classification),
`assets/public_provenance_ledger.json` (content hashes) and `PUBLIC_EXPORT.json`
(policy digest and included-content digest). Regenerating them in the working
tree is not enough — they have to be **staged and committed** with the change they
describe (a refresh after merging `main` is its own commit; see `REVIEW.md`), or the
commit ships a source change without the evidence for its own contents and fails with
`POLICY_UNCLASSIFIED`, `UNRESOLVED_PUBLIC`, `POLICY_EXPORT_STALE` and
`PROVENANCE_CONTENT_MISMATCH` together.

`make readiness` re-runs the export generator at the end and fails if that
changes anything, which catches precisely this.

These checks have no local equivalent and are never implied by a local pass:
CodeQL, `dependency-review`, the Betterleaks history scan, the Windows runtime
compile gate, and the main integration smoke.

## Text, encoding, and line-ending contract

The repository's text bytes are **UTF-8 without BOM, LF, with a final newline**.
`.gitattributes` (`* text=auto eol=lf`) and `.editorconfig`
(`charset = utf-8`, `end_of_line = lf`, `insert_final_newline = true`) are the
authority; this section says how to *produce* bytes that satisfy them.

Git normalises on commit, so the index is clean by construction — a scan of all
tracked files finds no CRLF, no BOM and no UTF-16. The damage happens
elsewhere: in working trees and in generated artifacts. A checkout polluted with
CRLF makes every touched file's content hash disagree with the provenance
ledger, and the resulting wall of `PROVENANCE_CONTENT_MISMATCH` names the
symptom rather than the cause. That is why `publish_audit` now reports
`TEXT_LINE_ENDING_CRLF`, `TEXT_ENCODING_BOM`, `TEXT_ENCODING_UTF16` and
`TEXT_FINAL_NEWLINE` directly, skipping anything binary.

**Python.** Text mode translates `
` to the host newline unless told otherwise,
so on Windows `write_text(s, encoding="utf-8")` silently emits CRLF and a
different SHA-256 than the same code on Linux. Every writer that produces
canonical, tracked, or hash-participating text must pin it:

```python
path.write_text(text, encoding="utf-8", newline="
")   # canonical text
path.write_bytes(canonical_bytes)                        # already-canonical bytes
```

`CanonicalWriterTests` asserts this for the canonical writers, so the rule
cannot quietly regress.

**PowerShell.** Use PowerShell 7.4+, which the project already requires: in
Windows PowerShell 5.1 `-Encoding utf8` means UTF-8 **with** BOM, and in 7+ it
means without. `Set-Content` and `Out-File` also join with the host newline. For
anything byte-exact, bypass the text pipeline entirely:

```powershell
[System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllBytes($path, $bytes)
```

**Never route exact bytes through a text pipeline.** `git show ... | Set-Content`
and `<binary> | Out-File` re-encode and re-line-end their input. To extract exact
repository bytes use `git archive`, `git cat-file`, or a redirect from a
binary-safe shell. No tracked script does this today; keep it that way.

**Bash and WSL** must likewise preserve LF and must not rewrite bytes
incidentally — a heredoc that reflows content is a rewrite.

**Generated evidence outside the worktree** follows the same contract whenever it
participates in a SHA-256 manifest or a forensic byte comparison, for the obvious
reason: a manifest that changes with the shell that produced it proves nothing.
Historical raw or binary capture evidence is never normalised.

The invariant, stated once: **the same logical generated text must produce the
same bytes and the same SHA-256 whether it came from PowerShell, Windows Python,
Bash, or WSL.**

## Python test sharding

`python_tools` runs `tools/test_*.py` on parallel runners. The module lists come
from [`tools/ci_test_shards.py`](../tools/ci_test_shards.py), not from the file
names: it packs modules longest-first onto the least-loaded shard (ties broken by
name and then shard index) using the measured per-module seconds in
[`tools/ci_test_weights.json`](../tools/ci_test_weights.json). A module without a
measurement uses the file's `default_seconds`, so a new test needs no data edit to
run. Modules in one `separate` group never share a shard; `test_title_catalog` and
`test_publication_policy_gate` deliberately mutate tracked repository state and
form such a group. The plan depends only on the tracked module names and the data
file, so every runner computes the same partition independently.

`tools/test_ci_test_shards.py` runs the planner exactly as the workflow does and
proves that every `tools/test_*.py` module is scheduled on exactly one shard, that
the separated modules stay apart, and that the workflow captures the planner's
output before using it (a failing planner must stop the step instead of handing
`unittest` an empty module list, which would fall back to discovery). Refresh the
weights when the suite's cost moves materially: run each module once, serially,
and record its wall-clock seconds; `python tools/ci_test_shards.py --check` rejects
weights for modules that no longer exist, and `--plan` prints the resulting
partition with per-shard estimates.

### Python test tiers

When the classifier routes the Python suite, every PR runs all discovered modules
in the PR tier. Only cases marked `NIGHTLY_TIER:` are deferred: the composite
public-export gate report, the profile-zero package/runtime route, the slow
make-built platform-ladder runtime checks, the full-history report generation,
and the matrix inventory report. Their constituent legal-blocker, publication,
fail-closed NID/dispatch, cosimulation, provenance, import-audit, and security
gates remain in their normal PR jobs and tests. `python_tools` includes a
one-line summary of skips by `NIGHTLY_TIER`, `WINDOWS_ONLY`, and `PRIVATE_INPUT`.

The nightly `schedule` and manual `workflow_dispatch` both force the full CI
matrix and set `NK_TEST_TIER=full`, so every nightly case runs. The normal local
command uses the PR tier; set the environment variable to run the full tier:

```bash
NK_TEST_TIER=full python -m unittest discover -s tools -p "test_*.py"
```

PowerShell:

```powershell
$env:NK_TEST_TIER = "full"
python -m unittest discover -s tools -p "test_*.py"
Remove-Item Env:NK_TEST_TIER
```

### What hosted CI does not run

Hosted CI runs the Python suite on Linux (`python_tools`) when `tools/ci_paths.py` routes
Python: the full suite for `python_scope` `all`, and only the Makefile-coupled modules when
a recipe-only `Makefile` change is the only Python-routing change (`python_scope`
`makefile`). An exact head whose required hosted CI is green, including `python_tools` when
the change routes Python, needs no local full run; a change that routes no Python needs none
either.

No hosted job runs the Windows-only cases in `tools/test_asset_index_c.py`,
`tools/test_scratch_dir.py`, `tools/test_player_package_route.py`,
`tools/test_manager_safety.py`, `tools/test_visual_oracle.py`, `tools/test_library_sweep.py`
and `tools/test_stage_install.py`. A change under `tools/`, `src/core/`, `src/player/`, the
`Makefile` or any `.ps1` file (including the root `nk_manager.ps1`) runs those modules locally
on Windows. The full local suite builds the native fixtures and takes over an hour on
Windows. Make-based tests need a POSIX `sh` on `PATH` (MSYS2 `usr/bin` or Git for Windows
`bin`).

## Coverage

Python coverage is reported as XML and C coverage is collected from raw gcov data
whenever their jobs run; neither can change a gate's result.

- **Python tooling (`python_tools`).** Each shard runs the module list it always
  ran, through `python -m coverage run --parallel-mode --source=tools -m unittest
  -v ...`, then combines its own data and writes `coverage-python.xml`. The
  measured surface is the tooling, not the tests: `--source=tools` and
  `omit = ["tools/test_*.py"]` in `[tool.coverage.run]` (`pyproject.toml`) keep
  the test modules themselves out of the number. Coverage records the run and
  exits with unittest's own status, so a failing shard still fails the job.
- **C runtime (`native_tools`).** One additional step builds and runs the same
  `native-core-tests` target in its own `$RUNNER_TEMP/native-core-coverage`
  `BUILD_DIR`, with `--coverage` appended to the Makefile's own `CFLAGS` (read
  back through `compiler-info`, so the measured build uses the default
  configuration) and to `LDFLAGS`. The same step then runs `gcov -p -b -r` once per
  object from the repository root, where the objects' recorded `src/...` source
  paths resolve, and collects the reports in `$RUNNER_TEMP/native-core-gcov`. The
  action's own `gcov` plugin is not used (`plugins: noop`): it runs gcov from the
  build directory, cannot open any source there, and uploads header-only reports
  that Codecov rejects. One run per object keeps a source shared by several
  selftests from getting duplicate line records, `-r` keeps system headers and
  generated fixtures out, and reports for the selftests' own `*_selftest.c` sources
  are dropped, as `tools/test_*.py` is for Python. The unprofiled
  `make CC=gcc native-core-tests` gate still runs first and is unchanged. The
  `CFLAGS`/`LDFLAGS` route instruments the targets that compile through those
  variables; a target that hard-codes its own compile flags is not measured, so
  the C number is a lower bound for `src/` rather than a complete statement about
  it.

Both reports are uploaded with `codecov/codecov-action` (pinned by commit SHA)
under `flags: python` and `flags: c`, authenticated with the job's OIDC identity
(`use_oidc: true`, which is why those two jobs -- and only those two -- declare
`id-token: write` beside `contents: read`). `fail_ci_if_error: false` keeps an
upload or Codecov outage from failing the job.

**The numbers are informational.** [`.github/codecov.yml`](../.github/codecov.yml) marks the
project and patch status checks `informational: true`, so neither can block a
merge, and sets `carryforward: true` for both flags, so a run that measures only
one surface keeps the other flag's last report. The OpenSSF Best Practices
Silver criterion "statement coverage >= 80%" is the target these numbers are
watched against; it is not a gate, and promoting either status to a required
check is a separate maintainer decision. Python's `coverage` package is installed
from `tools/requirements-lock.txt` with `--require-hashes`; the C reports come
from the runner's own `gcov` and add no Python dependency. A change to `.github/codecov.yml` matches no classifier
predicate, so it fails closed to the full matrix like any other unknown path.

## Classifier invariants

`tools/ci_paths.py` decides which gates run. The only failure that matters is a
**false negative** — a build-affecting change classified as documentation or
tooling and therefore skipping a native or Windows gate. These invariants exist
to prevent that, and `tools/test_ci_paths.py` asserts each one:

- **Unknown paths fail closed.** Any path matching no predicate forces the full
  matrix. Adding a new kind of file makes CI more expensive, never less.
- **Every change type counts.** The changed-file query uses name-status without
  a narrowing diff filter, and retains both sides of a rename. Filtering to
  `ACMR` or keeping only a rename's new name can drop a build-affecting source,
  making a commit that removes or renames C code classify as docs-only and skip
  the native and Windows compile gates.
- **An empty or unobtainable file list forces the full matrix**, so a shallow
  clone or an unusual event payload cannot quietly narrow the run.
- **A merge-queue group is never path-gated.** `FULL_MATRIX_EVENTS` in
  `tools/ci_paths.py` (`workflow_dispatch`, `merge_group`, and `schedule`) forces
  the complete matrix, including the main smoke, and allows substantive work. A group's change
  set is its payload range (`merge_group.base_sha` to `head_sha`), and a payload
  without that range fails closed to the full matrix. The classifier output for a
  group is identical to a manual dispatch for the same paths, which
  `MergeGroupRoutingTests` pins.
- **Draft status does not suppress substantive validation.** A draft and a ready
  pull request receive the same path classification and applicable gates, so
  progress does not depend on a status transition or manual dispatch. The
  classifier still exports `draft` for diagnostics and keeps the main-push
  suppression policy separate. Because the gates do not depend on draft status,
  `.github/workflows/ci.yml` lists only the head-changing events in its
  `pull_request` types (`opened`, `synchronize`, `reopened`): a status transition
  would duplicate gates over a tree that already ran them.
- **`hygiene` is ungated, and that is load-bearing.** The all-files pre-commit
  run — which includes `publish_audit --provenance-self-consistency`,
  `policy_sync`, and the Betterleaks scan — executes on every event, so the
  security and publication boundary is never path-gated. This is what makes the
  cheap paths safe: every tracked file is inside the published surface and the
  provenance ledger hashes each one, so *any* change invalidates the generated
  ledger and `PUBLIC_EXPORT.json` until they are refreshed. A docs-only change
  may therefore skip the Python suite only because the audit still runs here.
  `PublicationCoverageInvariantTests` pins both halves so this cannot regress
  into a path-gated audit.
- **Only a structurally proven recipe-only `Makefile` edit narrows the build
  route.** `makefile_recipe_only_targets` parses the base and head `Makefile` and
  accepts the change only when every changed line is a comment, a blank line, or a
  recipe line of an explicit rule whose targets are all literal `.PHONY` names, and
  every unchanged line keeps its kind and owning targets. A variable, flag,
  prerequisite, rule header, pattern rule, file-producing recipe, conditional,
  include or `.PHONY` edit keeps `build_system`, as does a `Makefile` that either
  revision cannot provide. A recipe-only edit still runs the native and Windows
  gates, which execute recipes. Python runs only the modules whose transitive tool
  closure reads the `Makefile` or runs `make` (`ci_test_shards.py --select
  makefile`), on two shards; any other Python-routing change in the same diff
  restores the full four-shard suite. That closure covers most of the suite's
  weight, because most heavy modules build through `make`, so the saving is the
  unrelated modules and two runners rather than the Python job as a whole.
- **The published surface is derived, not listed.** `_is_public_surface` asks the
  publication policy instead of maintaining a second list that can drift; it
  fails closed to "published" when the policy cannot be read. The `public_surface`
  output is exported so a local readiness check can route the same decision,
  where no ungated hygiene equivalent runs.

Test modules use their logical implementation subject for classification:
`tools/test_<subject>.py` is evaluated through the same subsystem predicates as
`tools/<subject>.py`. This keeps build-relevant HST, title, codegen, and native
tool tests on the native and Windows gates without making every Python test
expensive. A new native-relevant tool should therefore be named and classified
like its implementation; add a predicate only when the implementation itself
belongs to a new subsystem.

## Cost and caching rules

GitHub-hosted Windows time is billed at a higher multiplier than Linux time. The
workflow therefore gates the Windows runner behind the cheaper Linux hygiene and
native gates, cancels superseded PR runs, and avoids repeating the full matrix on
ordinary main pushes. The workflow uses dependency/tool caches only (pip);
compiled runtime objects and generated shader/code output are not cached across
jobs or runs, so the repository's content-addressed invalidation and freshness
checks remain the source of truth. No volatile dollar figure is part of the
repository contract.

Inside one `windows_runtime` part, repeated compiles of the same sources (the
platform ladder builds the runtime once per rung, the smoke part once per smoke)
go through a job-local `ccache` (#735 item 2). It starts empty under
`RUNNER_TEMP`, keys each entry on the compiler, the command line and the contents
of every header, and is never restored, uploaded or shared with another job or
run. Make still decides what to build; the cache only returns an object for an
identical compile. `gcc` is a native symlink to the UCRT64 toolchain's
`ccache.exe`, so `CC` stays `gcc`, and SDL3 discovery and the toolchain check
(`tools/nk_doctor_checks.py`) follow the link into `/ucrt64/bin`. Each part's last
step prints the cache statistics and fails if no compile went through the cache.

Hosted GitHub Actions execution is active. The `main` ruleset requires `CI required`,
`OSV Vulnerability Scan`, `dependency-review`, `Hygiene and security`, and
`CodeQL` on exact pull-request heads. Path-gated workflows also run the applicable
classifier, Markdown, native/translation, main-smoke, Python, and
Windows gates. A green public-safe run proves only the paths it executes; it is
not a complete private-title gameplay route, and local verification remains
local-only.

The required `CodeQL` check is the code-scanning result GitHub reports after it
processes the SARIF that `.github/workflows/codeql.yml` ("Trusted CodeQL")
uploads; no upload means no check. GitHub runs that workflow from the default
branch: a `workflow_run` after the `CodeQL wake` workflow
(`.github/workflows/codeql-wake.yml`) completes on a pull request (forks
included) or on a merge-queue group, a push to `main`, a weekly schedule, or a
manual dispatch naming a pull request. The wake run starts with the pull request
head or the group commit, so the trusted
analysis runs alongside CI rather than after the whole CI workflow finishes. CI
itself is not a trigger: the analysis does not wait for the slowest CI job, and
CI's result never reaches the trusted run.

The wake run is a signal only. It is a `pull_request` run, or a `merge_group` run for
a merge-queue group (candidate-controlled, like every pull request workflow), declares
`permissions: {}`, checks nothing out,
runs no repository code, and has no secrets for fork pull requests. Its result is
never evidence: the trusted workflow resolves the exact pull request from GitHub's
API and refuses any head that no longer matches. A pull request can decide whether
its own wake run exists, and so cause an analysis of its own exact head or none at
all (no `CodeQL` check, so the merge stays blocked). It cannot make the trusted run
analyse another commit or repository, run its own code in that context, or supply
the required check's evidence. Its trusted-run concurrency group is the head
repository and branch, so a newer head of a branch cancels the analysis of the
older one; repositories never share a group, so forks cannot cancel one another.
The wake workflow's name is part of the trust path: `codeql.yml` names it in
`workflows:`, and `tools/test_public_ci_wiring.py` pins that the two agree.

A pull request whose head predates `codeql-wake.yml` has no wake run, so it
receives no `CodeQL` check until it merges `main` (which carries the wake workflow).

The trusted workflow never checks out or executes candidate files. For a pull request it reads the
exact merge tree as blobs and binds the upload to the merge ref; for a push or
schedule it scans the `main` commit. For a merge-queue group it reads the group
commit's tree as blobs and binds the upload to the queue ref and the group commit
(see [Merge-queue groups](#merge-queue-groups)). It analyzes `actions`, `c-cpp` and `python`
without uploading partial results and uploads all three in one SARIF. Each run's
`automationDetails.id` is `/language:<lang>/`: code scanning stores a run under the
part of the id before its last `/`, so the trailing slash keeps the three languages
in separate categories instead of letting one replace the others. TRAP caching is off:
a run that restored the cache was observed to extract no C/C++ file at all. An edit to this
workflow reaches pull requests only after it lands on `main`, or through a manual
dispatch run with `--ref` naming the branch.

## Merge-queue groups

A merge queue lands each reviewed pull request on a *group commit*: the trunk, every
queued pull request ahead of it, and this one. Each group is a ref in this repository
(`refs/heads/gh-readonly-queue/main/pr-<n>-<sha>`), and its required checks must pass
before `main` moves. These workflows carry a `merge_group` trigger (`checks_requested`).
`codeql.yml` does not: CodeQL on a group comes from the group's CodeQL wake run, as described below.

| Required context | Behaviour on a group commit |
| --- | --- |
| `CI required` | `ci.yml` runs the complete matrix, as for a manual dispatch. `hygiene` and `public_export` run ungated. |
| `Hygiene and security` | Ungated, as on every event. |
| `OSV Vulnerability Scan` | `osv-scanner.yml` already triggers on `merge_group`. |
| `dependency-review` | Passes `base-ref` and `head-ref` from the group payload. On a pull request they are empty, which the action reads as unset. |
| `CodeQL` | The group's CodeQL wake (`merge_group` run) starts the trusted `codeql.yml` through `workflow_run`, which analyses the group commit (below). |

**Trust argument.** A group exists only because a user with write access enqueued a
pull request that was reviewed. `codeql.yml` runs from the default branch for both
pull requests and merge groups: its `workflow_run` definition is the trusted
default-branch copy, whichever wake started it. The wake is only a signal. It declares
no permissions, checks nothing out, and its result is never read as evidence. The
trusted workflow reads the queue ref from GitHub's API, requires the queue branch to
belong to this repository, and requires the ref to name the commit the wake ran on
before analysis and again before upload. A queued change can therefore alter its own
group's wake run, but not the trusted definition, the commit analysed, or the evidence.
The other merge-group checks (`ci.yml`, `dependency-review.yml`, `osv-scanner.yml`) run
their definitions from the group commit, so a queued change to `.github/workflows/` is
still reviewed for that change and enqueued alone. Whether GitHub reads those
definitions from the group commit is an assumption to confirm in the trial.

**CodeQL on a group.** The wake's `merge_group` run starts the trusted workflow. Its
prepare job accepts the wake only when the head repository is this repository, the head
branch is a `gh-readonly-queue/` branch, and the queue ref names exactly the wake's
commit. The ref is read through the reserved `{+ref}` expansion, because `{ref}` would
percent-encode its slashes. The group tree is read as blobs with the same validator as
the pull-request route. The upload is bound to the queue ref and the group commit, with
the same languages, query configuration and per-language categories. Before the upload
the queue ref is checked again, so a group that was removed or moved during analysis
uploads nothing. The `CodeQL` check is the code-scanning result for the group commit.

**Latency.** The wake is a short signal run that starts when the group is created, so
CodeQL starts about as early as `CI` and runs alongside it. `CI`'s result never reaches
the trusted run. Serialising CodeQL after `CI`, by a `workflow_run` of the merge-group
`CI` run, would cost about 5.6 minutes median per group (round-1 measurement). That
alternative is not used.

**Provenance on a group.** `.github/workflows/provenance-attestation-merge-group.yml`
observes each group commit with the default-branch provenance verifier. It runs as a
`workflow_run` of the group's `CI` run, filtered to `merge_group` runs of this
repository's `gh-readonly-queue/` branches, and reads the group commit as data only,
without checking it out. Its verdict base is the merge base of the group with the
default branch, because a group's parent may be an earlier group that never landed. It
is observation only and not a required check; the hosted `Trusted provenance
attestation` remains the pull-request gate.

## Windows hosted runner policy

The Windows job intentionally remains on `windows-2022`. Hosted execution is
active, but that floating label is a GitHub-hosted Windows Server image rather
than an end-user support promise; the supported developer platform is Windows 11
x64 as documented in [SETUP.md](SETUP.md). A `windows-2025` migration is a
separate hosted-validation decision. Consult the current
[Windows 2022 image inventory](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md)
instead of treating a dated image version or tool inventory as an evergreen
repository guarantee.

## Windows Python and PATH contract

Repository tools in hosted jobs must not depend on which Python happens to be
first on `PATH` (#294). The contract is explicit:

| Job / environment | Python running `tools/*` and the fixture generators |
| --- | --- |
| Linux jobs (`classify`, `hygiene`, `public_export`, `markdown`, `python_tools`, `native_tools`, `main_smoke`, `ci_required`) | `actions/setup-python` CPython 3.15.1 |
| `windows_runtime` (MSYS2 UCRT64 shell) | Windows CPython 3.15.1 from `actions/setup-python`, passed through `PYTHON`; MSYS2's UCRT64 Python 3.14.8 is not used for repository tools |
| Local Windows runs | Windows CPython 3.15.x from the python.org installer; set `PYTHON` explicitly when MSYS2 comes first on `PATH` |

Stages that invoke the repository's PowerShell 7.4 asset-copy script opt into
the runner's Windows `PATH` explicitly with `MSYS2_PATH_TYPE: inherit` and assert
`command -v pwsh` themselves. Under `inherit` the MSYS2 UCRT64 directories still
precede the inherited Windows `PATH`; `windows_runtime` passes Windows CPython's
absolute path through `PYTHON` and converts it to an MSYS path only for direct
shell invocations. No hosted step relies on undeclared `PATH` order.

That same MSYS2 shell has no `git` on `PATH`, so the Makefile cannot resolve a
revision there. `windows_runtime` therefore sets `SR_SOURCE_COMMIT` to
`github.sha` for every step: the identity the binaries of that job record
(flight recorder `build.build_id`, #532) is the revision `actions/checkout`
actually checked out, and the build never depends on finding `git`. The Linux
jobs run with `git` on `PATH` and resolve the same revision themselves, through
one guarded resolution that yields an empty identity -- and no build-log noise --
on a host that has none. A build with no identity is not a silent pass:
`tools/flight_diff.py` refuses such a bundle fail closed.

### Comparator comparability contract (#320)

`tools/flight_diff.py` compares two flight-recorder bundles under an explicit
comparability contract, so a MATCH is only ever printed for two captures that
can actually vouch for each other. The CLI verdicts map to exit codes
`MATCH` 0, `DIVERGENCE` 1, error 2 (unchanged), and `INCOMPARABLE` 3:

- **Comparable and matching (MATCH):** every compared field agrees. Build
  identity (`build_id`, `source_date_epoch`, `compiler`, `pointer_bits`) and
  the recorder limit never block a comparison; both identity lines are printed
  on stdout, because comparing different builds is a legitimate use.
- **Incomparable (INCOMPARABLE):** differing bundle schema versions, runtime
  blocks, enabled event classes, or armed trigger policies; any dropped event
  on either side (the ring slides, so a divergence inside the dropped window
  is invisible).
- **Divergent (DIVERGENCE):** the first differing event (sequence or class
  alignment), then the terminal reason/sequence/kind/arg0, then, for schema 5,
  the refusal block's count, first_nid, first_pc, first_sequence and nids. Same
  retained events with a different terminal outcome or refusal block is a
  divergence, not a MATCH. A terminal difference is never an incomparability: a
  `running` terminal is a mid-run cut rather than a ring overflow, so over a
  complete window it hides no events and stays an ordinary compared value.
- **Malformed (error):** a bundle that fails schema validation, sanitization,
  or the recorder truncation invariants (retained sequences exactly
  `dropped+1..recorded`, `dropped == recorded - min(recorded, limit)`) exits 2
  on stderr as before.

MATCH is a statement about the capture, never about the whole program. It
certifies exactly these things: that the two runs agree on every event retained
for the enabled classes over a complete window that dropped nothing, that both
reached the same terminal outcome, and, for schema 5, that both recorded the
same refusal block. It does not certify behaviour outside
the enabled classes, where a class one side never recorded is silence rather
than agreement; behaviour before the recorded window; or behaviour the recorder
does not model at all. Two builds that MATCH here can still differ anywhere the
recorder was not looking, so the CLI prints the enabled class list that bounds
the verdict next to the two identity lines.

A bundle that records any dropped event can never produce a MATCH against any
other bundle; re-capture with a larger limit when a comparison is the goal.

## Public release-path gates (#294)

The hosted matrix now exercises the same public, source-owned release path a
developer runs locally, without private inputs:

- `windows_runtime` links the native player (`mingw32-make player`), runs
  `player-ui-tests`, executes headless native player UI regressions
  (`mingw32-make --no-print-directory CC=gcc VULKAN_SDK=/ucrt64 player-ui-regressions`,
  `smoke` part only; the harness forces SDL's dummy video and software render drivers),
  runs the complete platform ladder
  (`mingw32-make --no-print-directory platform-ladder`), and runs
  `profile-zero-e2e` for the two profile-zero manifests. That gate
  validates the guest ProgramImage, generates a public AOT package, launches it
  through the headless production runtime, and checks all seven named
  guest-service cases for each manifest. It runs in the `routes` part of the
  25-minute `windows_runtime` job and took about 123 seconds locally for both package
  builds and runtime launches. The guest bytes are built from the fixture's own
  `main.c` and `Makefile` by the `profile_zero_fixture` job (pinned PSPDEV, checked
  against the committed `fixtures/profile_zero/prebuilt/SHA256SUMS`) and downloaded
  as an artifact, so the repository stores no generated executable (#1100) and the
  Windows runner still executes the whole route; where PSPDEV is installed, the
  gate rebuilds the fixture itself and fails if any byte differs from `SHA256SUMS`. Public
  runtime link dependencies remain external; when they are unavailable, the
  test reports an explicit `SKIP` with the printed missing-dependency reason.
  The job also runs the production smoke with its executable staged into a fresh
  directory outside the build tree (`production-smoke-staged`).
- The `public_export` job ("Public export candidate audit") is ungated and runs
  in parallel with `hygiene` (#735 item 5). Its step "Exercise public-export
  generation and candidate audit" runs on `security_publication` changes and every
  manual `workflow_dispatch`, and the job reports success when the step is skipped.
  `ci_required` requires the job to succeed. One
  `build_public_export.py --export-dir ... --public-safe-profile` invocation
  runs the publication gates, generates the public-safe candidate, and audits
  the candidate-tree staging bytes. Hosted CI holds no trusted ledger, so the
  export must fail closed with `PROVENANCE_UNVERIFIED` and promote nothing; the
  step asserts that named boundary and the #293 immutability rule (no candidate
  at the requested path). Clearing a candidate belongs to the release flow and
  requires the release-controlled trusted ledger.

## Dependabot policy

`.github/dependabot.yml` checks GitHub Actions and root pip and
pre-commit ecosystems monthly. Minor and patch updates are grouped per ecosystem;
major updates remain standalone because they can change APIs, runners, or build
semantics. Security updates remain enabled and are not suppressed by the routine
groups. The open-PR limits keep routine maintenance from crowding out focused
engineering work.
