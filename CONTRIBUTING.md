# Contributing

Thanks for helping with Nakagawa Recomp. This page is the short path to a first contribution.
The rules it links to decide whether a change can land, so read them once.

## Start here

1. **Set up.** Follow [docs/SETUP.md](docs/SETUP.md), the authoritative development baseline. Build
   the player once with those steps before you change anything, so you know your setup works.
2. **Find work.** Look for issues labelled
   [`good first issue`](https://github.com/Jstar269/nakagawa-recomp/labels/good%20first%20issue) (small,
   self-contained, no PSP or retail game needed) or
   [`help wanted`](https://github.com/Jstar269/nakagawa-recomp/labels/help%20wanted) (a contributor can take
   it). Issues labelled `needs-psp-hardware` need a physical PSP run, and `needs-owner-decision` waits on a
   maintainer. Search existing issues before you open a new one. Every issue carries at least one label and a
   milestone: `v0.0.1` (the release's own steps), `v0.0.2` (bugs, HLE and accuracy fixes, tooling), `v0.1.0` (a
   second title, the flight recorder, the hardware oracle), `v0.2.0` (product and distribution) or `v0.3.0`
   (enhancements, diagnostic views, performance, new platforms). If GitHub won't let you set them, say in the
   issue which fit and a maintainer sets them.
3. **Agree the approach.** For anything bigger than a small fix, comment on the issue first.
4. **Make your change.** Fork the repository to your personal account and create a branch. One pull request
   may group related changes, for example several fixes in the same area or a feature with its tests and docs.
   Put unrelated changes in separate pull requests, and say in the description what each change does. Leave
   **Allow edits by maintainers** enabled on the pull request. Maintainers bring your branch
   up to date with `main` before merging. GitHub doesn't let them push to a fork owned by an organisation,
   so a pull request from one is carried in a new pull request. That pull request keeps your commits
   unchanged and credits you as co-author.
5. **Check it.** Run `make contrib-check` (Linux) or `mingw32-make contrib-check` (Windows, MSYS2 UCRT64 on
   `PATH`). It runs the gates that apply to the files you changed. Add a test for any behaviour you changed,
   and paste the exact commands and results into the pull request.
6. **Refresh provenance controls.** If you changed files that already have public entries, run
   `python tools/contrib_refresh.py` (or `make contrib-refresh`) and commit what it stages; usually it
   stages nothing, because an edit changes no claim. Never hand-edit `PUBLIC_EXPORT.json` or
   `assets/public_provenance_ledger.json`. Adding or removing a file is a new public path: the tool refuses
   it, so say so in the pull request and a maintainer admits it. The table below lists each refusal.
7. **Disclose AI help.** If an AI tool helped, follow [docs/AI_USAGE.md](docs/AI_USAGE.md). Add one
   `Assisted-by: <tool> (<model>)` trailer per tool to each commit it helped produce (not `Co-Authored-By:`),
   and say so in the pull request. If you work with an AI agent, point it at [AGENTS.md](AGENTS.md): its
   "Where to start" section is written for your agent and covers the same steps as this list.
8. **Confirm your right to submit.** Tick the box in the pull request template: you wrote the change, or
   have the right to submit it, under GPL-3.0-or-later. A DCO sign-off (`git commit -s`, which adds
   `Signed-off-by: Your Name <you@example.com>`) is welcome but optional, and only the person who wrote the
   change may add it. See [docs/DCO_POLICY.md](docs/DCO_POLICY.md).
9. **Open the pull request.** Use a draft if it is not finished, fill in the template, and say what is left.
   Ask questions in the [Help](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/help) category
   of GitHub Discussions, or comment on the issue you are working on.

## Accessibility

The player's accessibility statement, with what works, what does not and how to report a barrier, is
[ACCESSIBILITY.md](ACCESSIBILITY.md). If you change the launcher or the documentation, follow its
"Contributing accessibly" list and update its tables when a change adds, changes or removes a barrier.
For questions that are not about code, [SUPPORT.md](SUPPORT.md) says where to go.

## Licensing

The project as a whole is licensed GPL-3.0-or-later, and new files you write yourself use
`SPDX-License-Identifier: GPL-3.0-or-later`. Files that came from somewhere else keep the exact SPDX
identifier and notices recorded for where they came from: don't relabel upstream-derived code as
project-authored, and don't replace its licence identifier. Some questions about inherited licensing and
provenance are still under qualified review, so don't bring in code derived from a third party until you
know exactly where it came from and under which licence.

## Provenance controls

Run `make contrib-refresh`, or leave this to a maintainer. Two files,
`PUBLIC_EXPORT.json` and `assets/public_provenance_ledger.json`, are generated from a private
trusted ledger that contributors don't have, so never edit them by hand. For each published file, the
ledger records where it came from and the evidence for that (its classification and evidence). It
carries no content hash: the maintainer's **Trusted provenance attestation** regenerates every file's
SHA-256 from the private authority at landing and compares the committed claims with it, so editing an
existing file does not change the ledger. The export records the publication policy and its digest.

For the ordinary case — you changed the contents of paths that already have public entries, and you
added and removed nothing — run:

```bash
make contrib-refresh          # Linux
mingw32-make contrib-refresh  # Windows (MSYS2 UCRT64 on PATH)
```

`contrib-refresh` runs `tools/contrib_refresh.py` against `origin/main` (override with
`CONTRIB_BASE`, as with `contrib-check`). It keeps the claim already recorded for each changed path,
prints `claim kept <path>` for each, writes both files and stages them; for an ordinary change neither
file's bytes change. Add `CONTRIB_REFRESH_CHECK=--check` to see what would change without writing
anything.

**After the ledger lost its hashes (#1673).** A branch made before that change may still carry the
older ledger with a `sha256` per entry. When you merge `main` and Git reports a conflict in
`assets/public_provenance_ledger.json`, take `main`'s copy (`git checkout --theirs -- <path>` during a
merge), stage it, finish the merge, then run `make contrib-refresh` and commit what it stages.

This is a convenience, not an authority. It cannot see the private trusted ledger, so it cannot see
historical frozen debt or a changed executable path with no public entry, and it never invents a
claim. The maintainer's **Trusted provenance attestation** re-derives both files from the private
authority at landing, and that re-derivation — not this target — is what admits your change. If
**Trusted provenance attestation** or the publication-safety step in **Hygiene and security**
reports a provenance mismatch the refresh did not resolve, a maintainer refreshes those files on
your branch.

**If the tool refuses, say so in the pull request.** It exits non-zero, prints a code, and writes
nothing. A maintainer then admits, withdraws or resolves the path; you never need to work around the
refusal.

| Refusal | What it means | What to put in the pull request |
| --- | --- | --- |
| `NEW_PUBLIC_PATH` | Your change adds a path with no public entry. | Where the file came from: written by you, or derived from which project, at which revision, and under which license. |
| `REMOVED_PUBLIC_PATH` | Your change deletes a path that has a public entry. | That the path is gone and why; a maintainer withdraws the record. |
| `POLICY_CHANGED` | The source policy profile itself changed (`assets/public_source_profile.json`). | That you changed the profile and why; profile edits are a maintainer decision. |
| `UNRESOLVED_ENTRY` | A changed path's public entry still carries no resolved claim. | Which path it is; a maintainer resolves the record. |
| `CHANGED_PATH_NOT_PUBLIC` | A changed path has no committed public entry in either direction. | Which path it is and where its content came from. |

The other codes are about the invocation, not about a path: `CANDIDATE_INDEX_STALE` means you
have unstaged edits (run `git add`); `TRUSTED_BASE_REQUIRED`, `TRUSTED_BASELINE_MISSING` and
`TRUSTED_BASE_NOT_ANCESTOR` mean the base commit could not be resolved or is not an ancestor of
your head (pass `--base <commit>`); `CANDIDATE_TREE_INVALID` means the staged tree could not be
read; `GENERATED_LEDGER_INVALID` means the regenerated ledger failed the public ledger's own
validator, which is a bug in the tool rather than in your change, so report it. Exit codes: 0
refreshed (or `--check` reported), 1 refused, 2 the verifier rejected an input, 3 the refreshed
files could not be staged.

**Expect Trusted provenance attestation to go red on a pull request that adds a file.** That is
normal, not something you broke: it stays red until a maintainer admits the new path.

**What a provenance failure means for you.** When the only findings are
`TRUSTED_PATH_MISSING`, `TRUSTED_PATH_UNQUALIFIED`, or a new path the publication scope has not
admitted yet, the job summary now says so in one line: *a maintainer will admit these N new files;
nothing for you to do*. That is the gate's real answer, not a reprieve — the check still fails
closed until the admission lands. Any other finding (`CONTENT_MISMATCH`, `CLAIM_UNBACKED`,
`EXPORT_FIELD_MISMATCH`, a policy or CI finding) is about your change and is yours to fix. The
summary distinguishes the two so a new file never reads as a mistake you made.

### What the pull-request checks mean

| Check | What it verifies | If it fails |
| --- | --- | --- |
| Classify change | Which areas your change touches, so only relevant jobs run | Rarely fails; ask a maintainer |
| Hygiene and security | pre-commit hooks: Ruff, whitespace and encoding, publication safety, secret scanning | Run `python -m pre_commit run --files <your files>` locally |
| Markdown validation | markdownlint on changed Markdown | Run `npx --yes markdownlint-cli2@0.24.0 <file>` |
| Python tooling gates (0–3) | The `tools/` unit tests, split into four shards | Run the failing test module with `python -m unittest tools.<module>` |
| Native and translation gates | Strict C builds (`-std=c99`/`c11 -Werror`) and runtime selftests on Linux | Compile the changed C file with the flags shown in the log |
| Windows runtime compile gate | The runtime builds with MSYS2 UCRT64 on Windows | Check Windows-only APIs and headers |
| Trusted provenance attestation | Your change against the private trusted ledger | Provenance mismatches are handled by a maintainer |
| dependency-review, OSV, CodeQL | Dependency and static security scans | Read the finding; ask if unsure |
| Public export candidate audit | The publication gates and the public-safe export candidate, which must fail closed without the trusted ledger and leave nothing at the requested path | Read the failing step in the job log. A provenance finding in the publication audit (a missing or unresolved claim) stops the job before the candidate audit, so the step then says it failed for a reason other than the named fail-closed candidate audit; fix that finding first |
| Main integration smoke | The compact import and publication smoke (`tools/import_audit_gate.py`) and the provenance record gap check. It runs on pushes to `main`, manual runs and merge-queue groups, and is skipped on pull requests | Run `python tools/import_audit_gate.py` and `python tools/provenance_record_gap.py --check` |
| CI required | The aggregate of the required jobs above | Fix the failing job it names |

Kilo Code Review is an advisory AI review and never blocks a merge.

## Before changing code

1. Read [AGENTS.md](AGENTS.md) and the maintained documentation relevant to your subsystem.
2. Search **GitHub Issues**, which are the canonical source of truth for actionable work and acceptance criteria, before opening a duplicate.
3. Use [docs/STATUS.md](docs/STATUS.md) as the concise project status summary, not as a competing detailed issue tracker.
4. Never submit game binaries/assets, decrypted PRXs, private oracle traces, generated asset hashes, local databases, logs containing private paths, or files under the private-input directories documented by the project.
5. Do not edit generated `build/<game>/<game>_recomp_*.c`; change the generator/runtime and rebuild.
6. Follow [docs/AI_USAGE.md](docs/AI_USAGE.md) when using AI-assisted development tools.
7. Confirm in the pull request template that you wrote the change or have the right to submit it under GPL-3.0-or-later. A **Developer Certificate of Origin (DCO 1.1)** sign-off (`git commit -s`) is optional. See [docs/DCO_POLICY.md](docs/DCO_POLICY.md) for complete details.

## Contribution rights

A contribution to Nakagawa Recomp is provided under the project's license, GPL-3.0-or-later (GitHub's Terms of Service, "inbound = outbound"). Every pull request confirms, with a box in the template, that its author wrote the change or has the right to submit it under that license.

A **Developer Certificate of Origin (DCO 1.1)** sign-off is welcome but optional. If you add one, put a sign-off line in every commit of the pull request:

```text
Signed-off-by: Real Name <email@example.com>
```

- Use `git commit -s` to automatically append this line. Only the person who wrote a commit may sign it off, and no tool or agent adds one for anyone.
- A pull request without sign-offs is not blocked for that reason; the maintainer may merge it (maintainer decision, 2026-10-30). The same goes for a box you forgot to tick or a disclosure you left out: the maintainer asks in review instead of closing the pull request. Only code from another project has a firm rule, because the license requires it: name where it came from, and it must be under a GPL-compatible license.
- Neither the confirmation nor a sign-off is a copyright assignment—you retain ownership of your original contributions.
- Disclose third-party source origins and AI-assisted generation separately in pull request descriptions. Commits that an AI tool helped produce end with one `Assisted-by: <tool> (<model>)` line per tool; see [docs/AI_USAGE.md](docs/AI_USAGE.md#disclosing-ai-help-in-commits).
- For complete policy details, bot exceptions, and how to add sign-offs to your own open pull request, see [docs/DCO_POLICY.md](docs/DCO_POLICY.md).

## Development setup

Follow the [authoritative development baseline in docs/SETUP.md](docs/SETUP.md). The core toolchain
is Windows 11 x64, PowerShell 7.4+ (`pwsh`), CPython 3.15.x, current MSYS2 UCRT64, and a current
auto-detected Vulkan SDK/loader. The native player is the repository's only user interface.

## Contributing without a game disc

Most work needs no game, no disc and no PSP. The repository builds and tests with synthetic fixtures that it
generates itself, so you can set up, change code and prove the change without anyone's retail data. The
make targets below are the real ones; run them from the repository root with MSYS2 UCRT64's `bin` folder on
`PATH` (Windows) or with `make CC=gcc` (Linux). Times are wall-clock on the maintainer's Windows 11 machine
(24 threads) on the first, cold build of a fresh worktree; later runs reuse built objects and are faster.

| Command | What it proves | Time |
| --- | --- | --- |
| `python tools/nk_doctor.py --scope build` | Your toolchain is complete. | about 2 seconds |
| `mingw32-make --no-print-directory display-smoke` | Builds the whole pipeline (generator, compiler, runtime) on a synthetic fixture and checks the presented frame. | about 3.5 minutes |
| `mingw32-make --no-print-directory native-core-tests` | The native runtime self-tests (scheduler, memory, GE, UI core). | about 6 minutes |
| `python -m unittest tools.test_<name>` | One Python tooling module. Pick the module that matches the file you changed. | seconds to a minute |
| `python -m unittest tools.test_lint_docs tools.test_dco_policy` | The documentation and DCO policy gates, for a docs change. | about 15 seconds |
| `mingw32-make contrib-check` | The gates that apply to the files you changed (see below). | about 1 minute for a docs change |

Two more routes also need no game:

- `mingw32-make production-smoke`, `platform-ladder` and `cosim-selftest` exercise the two-phase pipeline,
  relocations, the scheduler and AOT-versus-interpreter parity on synthetic inputs
  ([docs/SETUP.md](docs/SETUP.md#public-synthetic-verification-routes)).
- The source-owned PSP demos and the oracle fixtures under `fixtures/` build with PSPDEV, the PSP toolchain,
  which on Windows runs inside WSL 2 (`mingw32-make showcase showcase-smoke`; setup in
  [docs/SETUP.md](docs/SETUP.md#showcase-demos-on-windows-wsl-2-and-pspdev)). They need no disc either.

The full Python suite (`python -m unittest discover -s tools -p "test_*.py"`) takes about 77 minutes on this
machine because it also runs heavy native fixtures. Hosted CI runs it in shards on every pull request, so run
only the modules for the files you changed.

What does need your own copy of a game: building or running a title (`nk_manager.ps1 -Action BuildFull` with a
title manifest, the library sweep, `make verify` and the private-title routes) and matching work in a
decompilation project. What needs a physical PSP is labelled `needs-psp-hardware` and you can skip it. Issues
labelled [`no-disc-needed`](https://github.com/Jstar269/nakagawa-recomp/labels/no-disc-needed) are written to be done entirely with the commands above; [#1811](https://github.com/Jstar269/nakagawa-recomp/issues/1811)
to [#1816](https://github.com/Jstar269/nakagawa-recomp/issues/1816) are the first six.

## Make a focused change

- Runtime C: follow `.clang-format`, use `sr_` for public symbols and `s_` for file-static state, and preserve the `CpuState` ABI.
- Python: follow `pyproject.toml`; update or add a focused test when changing codegen/tooling behavior.
- Documentation: update the maintained document, not an archived investigation. Keep `docs/STATUS.md`
  concise and link the canonical GitHub issue. When adding a confirmed defect or known limitation,
  update the canonical issue and its `docs/STATUS.md` status entry in the same change when applicable;
  label hypotheses and informational notes explicitly.
- Preserve existing SPDX, copyright, and provenance notices. For a new file, use an SPDX identifier only when its origin/license are actually known; do not invent a copyright owner or provenance claim.
- For project-authored source, use `GPL-3.0-or-later`; for inherited/upstream-derived source, preserve the SPDX identifier and notices required by its provenance record.
- Prefer general behavior/correctness fixes over address-specific compatibility overrides. Any unavoidable game-specific behavior needs evidence, a regression/route, and a retirement criterion.

## Verify

### The fast path: one command

```bash
make contrib-check          # Linux
mingw32-make contrib-check   # Windows (MSYS2 UCRT64 on PATH)
```

`contrib-check` looks at the files your branch changed against `origin/main` and runs only the
gates that apply to them — Ruff, the Python test modules matching the changed `tools/` modules, a
strict `-Werror` C compile of the changed C, markdownlint and the documentation-freshness lint for
changed Markdown, and the publication-safety audit. It finishes in minutes, and a documentation
change usually takes seconds. It is a *subset*: `make check` and `make readiness` remain the
authoritative gates, and CI still runs the hosted matrix.

It is a local pre-push helper with its own smaller gate set — not a representation of the hosted
matrix, and not evidence that a pull request is ready to merge. A green `contrib-check` does **not**
stand in for:

- **Windows runtime compile gate** — the runtime build, `native-core-tests` and the selftests on
  MSYS2 UCRT64. `contrib-check`'s strict compile goes no further than `gcc -fsyntax-only` over the
  changed C files on your own host: it does not build the Windows runtime objects, run
  `native-core-tests`, or run the Windows selftests. It compiles against whatever headers your host
  has, so a unit that includes an SDK header (SDL3, for example) needs that SDK installed locally.
- **CodeQL, dependency-review and OSV** — the hosted static and dependency security scans.
- **Trusted provenance attestation** — your head commit checked against the private trusted
  ledger, which only a maintainer can run.
- **CI required** — the aggregate status that branch protection actually requires.

`contrib-check` is a `Makefile` target, not a workflow: no hosted job invokes it (the pull-request
template points contributors at it instead). The evidence a pull request needs is the hosted check
set on its exact head commit — see [docs/CI.md](docs/CI.md) for which jobs run for which changed
paths and what each one means.

Every gate reports one of these outcomes, and they are not interchangeable:

| Outcome | Meaning |
| --- | --- |
| `PASS` | The gate ran and the change satisfies it. |
| `FAIL` | The gate ran and the change does not satisfy it. This fails the target. |
| `SKIP` | The tool is not installed in this environment. **Not a pass** — install it and re-run. |
| `NOT_RUN` | Your change does not touch that surface. |
| `MAINTAINER-SIDE` | Publication findings only a maintainer can clear, by regenerating the controls. Printed, never hidden, and never counted as your failure. |

Path routing is `tools/ci_paths.py`, the same classifier the hosted workflow uses, so the local
selection cannot drift from the hosted one. Set `CONTRIB_BASE` to measure against a different base
(`CONTRIB_BASE=upstream/main make contrib-check`).

### The full gates

Run checks proportional to the change:

```powershell
.\nk_manager.ps1 -Action Test  # uses the public synthetic manifest by default
mingw32-make --no-print-directory native-core-tests  # runtime change
mingw32-make --no-print-directory display-smoke      # codegen/pipeline change: full build of a synthetic fixture, no game needed
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree
```

To rebuild a game you own, pass its local title manifest:
`.\nk_manager.ps1 -Action BuildFast -TitleManifest <your manifest> -GameName <name>` for a runtime-only change, or
`-Action BuildFull` after a codegen or pipeline change ([docs/SETUP.md](docs/SETUP.md#3-build)). Title manifests
that name a retail game, such as the maintainer's Hot Shots Tennis one, stay local and are not in the repository.

Pass `--worktree` when auditing by hand: it reads the bytes on disk. The bare `--tracked-only`
form reads staged Git blobs, which is correct for the pre-commit hook (it stashes unstaged
changes first) but silently skips anything you have edited and not staged.

For documentation changes:

```powershell
npx --yes markdownlint-cli2@0.24.0
```

The repository also provides shared pre-commit hooks:

```powershell
python -m pip install pre-commit
python -m pre_commit install
python -m pre_commit run --all-files
```

What the hooks check. Both stages run `ruff-check`, the generic hygiene hooks (trailing whitespace,
end of file, YAML, JSON and TOML syntax, merge-conflict markers, private-key detection, files over 25 MB),
`documentation-freshness` (`tools/lint_docs.py`), `publication-policy-sync` (`tools/policy_sync.py`),
the publication audit (`tools/publish_audit.py`), the modified-file notice audit and the provenance-record gap
check. The commit stage adds a secret scan (`betterleaks`) and a staged-whitespace check. The hooks never need
a game, but they do read the whole tree, so a new file that carries a game name, disc ID, key or private path is
rejected before it leaves your machine. Never bypass them with `--no-verify`; fix what they report.

`install` sets up both the commit and the push hook (`default_install_hook_types` in
`.pre-commit-config.yaml`). The publication audit runs at both stages on purpose: the push stage
audits the index, not each pushed commit, and a pushed branch is public, so only the commit stage
checks every commit before it can leave the machine.

The external-oracle `make verify` path requires inputs that are intentionally not in the repository.
When those inputs are unavailable, report the gate as blocked/unavailable rather than treating it as
a pass.

### What admission means for your pull request

A new file is a new public path, and only the maintainer can admit one: the provenance record that covers it is
private. In your pull request, say who wrote each new file (and with which AI tool, if any) or, for code from
another project, the project, exact revision and licence. Open the pull request as a draft anyway and list the new
files there; do not edit `PUBLIC_EXPORT.json` or the ledger to make the check pass. The maintainer admits the exact
bytes of each new file (so change a new file after admission only by asking), then runs or asks you to run
`make contrib-refresh` and commit what it stages. The **Trusted provenance attestation** check stays red until the
admission, which is expected: a pull request that is otherwise green with only this check red is ready for review.
Edits to existing files need no admission.

## Developer Certificate of Origin (DCO 1.1)

Contributors confirm their right to submit in the pull request template; a **Developer Certificate of Origin (DCO 1.1)** sign-off via a `Signed-off-by:` line (`git commit -s`) is optional. The complete text of DCO 1.1, the maintainer standing waiver, bot submission rules, and how to add sign-offs are maintained in [`docs/DCO_POLICY.md`](docs/DCO_POLICY.md).

## Governance and roles

Nakagawa Recomp is a maintainer-led project.

- **Maintainer** ([@Jstar269](https://github.com/Jstar269)): sets direction, reviews and merges pull requests, owns
  releases, handles security reports ([SECURITY.md](SECURITY.md)) and enforces the
  [code of conduct](CODE_OF_CONDUCT.md). Decisions are made in public issues and pull requests whenever they are not
  security-sensitive.
- **Contributors**: anyone who opens an issue or pull request under this guide.
- **AI agents**: used by the maintainer and by contributors as assistants under [`AGENTS.md`](AGENTS.md); their
  commits say so with an `Assisted-by:` trailer. The person who submits a change is accountable for it, and a human
  maintainer for everything merged.
- **Automated reviewers and CI**: required checks gate every merge, but they advise; they do not decide.

**Access to sensitive resources.** Only the maintainer, [@Jstar269](https://github.com/Jstar269), has admin
access to this repository: settings, rulesets, Actions secrets, security advisories, releases, and the private
provenance repository. Dependabot and the review bots can open pull requests or comments but cannot merge or
change settings, and the branch ruleset has no bypass for anyone.

**Granting elevated access.** Before anyone receives write, admin or secret access, the maintainer reviews
their record of sustained, reviewed contributions, requires two-factor authentication on their account,
grants the lowest role that does the job, and records the change in a public issue. Access is removed when it
is no longer used.

## Dependencies

- **Selecting.** A new dependency must be maintained, under a licence compatible with GPL-3.0-or-later, and
  worth more than writing the code; prefer the standard library and existing dependencies. Upstream code is
  vendored only under [`AGENTS.md`](AGENTS.md) section 4 (pinned revision, `PROVENANCE.md`, licence kept).
- **Obtaining.** Python packages come from the hash-pinned
  [`tools/requirements-lock.txt`](tools/requirements-lock.txt) (`pip install --require-hashes`); the PSP
  toolchain from the release and SHA-256 pinned in
  [`assets/upstream/pspdev.lock.json`](assets/upstream/pspdev.lock.json); GitHub Actions by full commit SHA.
- **Tracking.** Dependabot, GitHub dependency review and OSV-Scanner flag outdated or vulnerable
  dependencies on every change, and `tools/generate_sbom.py` produces the software bill of materials.

## Pull requests

Use a descriptive branch and commit message (a DCO sign-off with `git commit -s` is optional). The repository PR template prompts for the required
evidence. In every substantive pull request:

- explain the problem and approach;
- link the canonical GitHub issue(s) or concrete evidence;
- list exact tests/routes and results;
- call out anything blocked or unavailable;
- keep generated `build/` output and private/proprietary inputs out of Git;
- update the linked `docs/STATUS.md` status when the current milestone materially changes;
- confirm your right to submit the change under GPL-3.0-or-later;
- disclose any new third-party source/data with exact source/revision/license; and
- disclose material AI-assisted translation/reimplementation so provenance can be reviewed.

A partial PR does not close an issue merely because CI is green. Record which acceptance criteria
remain unresolved in the issue/PR discussion.

Follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) in all project spaces, including Discussions. Report
security problems privately through [SECURITY.md](SECURITY.md), never in a public issue or discussion.
