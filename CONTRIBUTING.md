# Contributing

Thanks for helping with Nakagawa Recomp. The repository-level project declaration is
GPL-3.0-or-later. New project-authored source files use `SPDX-License-Identifier: GPL-3.0-or-later`.
Inherited and third-party files retain the exact SPDX identifier and notices recorded for their source
lineage; do not relabel upstream-derived code as project-authored or replace its license identifier.
Specific inherited licensing/provenance questions remain under qualified review; do not introduce
third-party-derived code without resolving its actual source/license chain.

## Your first contribution

1. Pick an issue labelled
   [`good first issue`](https://github.com/Jstar269/nakagawa-recomp/labels/good%20first%20issue) or
   [`help wanted`](https://github.com/Jstar269/nakagawa-recomp/labels/help%20wanted). For anything
   larger, comment on the issue first so the approach can be agreed.
2. Fork, create a branch, make one focused change, and sign off each commit (`git commit -s`).
3. Run the checks for the area you touched (see [Verify](#verify)), then open the pull request and
   fill in the template.
4. Leave **Allow edits by maintainers** enabled on the pull request.

**Provenance controls: run `make contrib-refresh`, or leave them to a maintainer.** Two files,
`PUBLIC_EXPORT.json` and `assets/public_provenance_ledger.json`, are generated from a private
trusted ledger that contributors don't have. Never edit them by hand; they are not hand-written
files. The ledger records, for each published path, the classification and evidence the public
export claims for it and the SHA-256 of its bytes; the export is the public-safe inventory and
digest built from that ledger.

For the ordinary case — you changed the contents of paths that already have public entries, and you
added and removed nothing — run:

```bash
make contrib-refresh          # Linux
mingw32-make contrib-refresh  # Windows (MSYS2 UCRT64 on PATH)
```

`contrib-refresh` runs `tools/contrib_refresh.py` against `origin/main` (override with
`CONTRIB_BASE`, as with `contrib-check`). It copies the claim already recorded for each changed
path, rehashes only those paths, prints the old and new hash for every path it rehashed, and stages
both files. Add `CONTRIB_REFRESH_CHECK=--check` to see what would change without writing anything.

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
| Markdown validation | markdownlint on changed Markdown | Run `npx --yes markdownlint-cli2@0.23.3 <file>` |
| Python tooling gates (0–3) | The `tools/` unit tests, split into four shards | Run the failing test module with `python -m unittest tools.<module>` |
| Native and translation gates | Strict C builds (`-std=c99`/`c11 -Werror`) and runtime selftests on Linux | Compile the changed C file with the flags shown in the log |
| Windows runtime compile gate | The runtime builds with MSYS2 UCRT64 on Windows | Check Windows-only APIs and headers |
| Trusted provenance attestation | Your change against the private trusted ledger | Provenance mismatches are handled by a maintainer |
| dependency-review, OSV, CodeQL | Dependency and static security scans | Read the finding; ask if unsure |
| Public export candidate audit | The publication gates and the public-safe export candidate, which must fail closed without the trusted ledger and leave nothing at the requested path | Read the failing step in the job log. A provenance mismatch in the publication audit stops the job before the candidate audit, so the step then says it failed for a reason other than the named fail-closed candidate audit; fix the mismatch first |
| Main integration smoke | The compact import and publication smoke (`tools/import_audit_gate.py`) and the provenance record gap check. It runs on pushes to `main`, manual runs and merge-queue groups, and is skipped on pull requests | Run `python tools/import_audit_gate.py` and `python tools/provenance_record_gap.py --check` |
| CI required | The aggregate of the required jobs above | Fix the failing job it names |

Kilo Code Review is an advisory AI review and never blocks a merge.

## Before changing code

1. Read [AGENTS.md](AGENTS.md) and the maintained documentation relevant to your subsystem.
2. Search **GitHub Issues**, which are the canonical source of truth for actionable work and acceptance criteria, before opening a duplicate.
3. Use [ISSUES.md](ISSUES.md) as the concise project status summary, not as a competing detailed issue tracker.
4. Never submit game binaries/assets, decrypted PRXs, private oracle traces, generated asset hashes, local databases, logs containing private paths, or files under the private-input directories documented by the project.
5. Do not edit generated `build/<game>/<game>_recomp_*.c`; change the generator/runtime and rebuild.
6. Follow [docs/AI_USAGE.md](docs/AI_USAGE.md) when using AI-assisted development tools.
7. Sign off every commit using standard **Developer Certificate of Origin (DCO 1.1)** (`git commit -s`), unless the maintainer standing waiver applies to you. See [docs/DCO_POLICY.md](docs/DCO_POLICY.md) for complete details.

## Contributor Rights Attestation (DCO 1.1)

Outside contributions to Nakagawa Recomp require a **Developer Certificate of Origin (DCO 1.1)** sign-off line in every commit message:

```text
Signed-off-by: Real Name <email@example.com>
```

- Use `git commit -s` to automatically append this line.
- The maintainer's own commits — including work prepared under the maintainer's direction by AI assistants or agents — are covered by a standing waiver that stays in force until the maintainer explicitly revokes it, on the public repository as well as this one. That waiver is personal to the maintainer and changes nothing for outside contributors. See [docs/DCO_POLICY.md §5.1](docs/DCO_POLICY.md).
- DCO sign-off certifies that you authored the change or have the right to submit it under the project's applicable terms.
- DCO is **not** a copyright assignment—you retain ownership of your original contributions.
- Disclose third-party source origins and AI-assisted generation separately in pull request descriptions. Commits that an AI tool helped produce end with one `Assisted-by: <tool> (<model>)` line per tool; see [docs/AI_USAGE.md](docs/AI_USAGE.md#disclosing-ai-help-in-commits).
- For complete policy details, bot exceptions, and sign-off correction steps, see [docs/DCO_POLICY.md](docs/DCO_POLICY.md).

## Development setup

Follow the [authoritative development baseline in docs/SETUP.md](docs/SETUP.md). The core toolchain
is Windows 11 x64, PowerShell 7.4+ (`pwsh`), CPython 3.14.x, current MSYS2 UCRT64, and a current
auto-detected Vulkan SDK/loader. The native player is the repository's only user interface.

## Make a focused change

- Runtime C: follow `.clang-format`, use `sr_` for public symbols and `s_` for file-static state, and preserve the `CpuState` ABI.
- Python: follow `pyproject.toml`; update or add a focused test when changing codegen/tooling behavior.
- Documentation: update the maintained document, not an archived investigation. Keep `ISSUES.md`
  concise and link the canonical GitHub issue. When adding a confirmed defect or known limitation,
  update the canonical issue and its `ISSUES.md` status entry in the same change when applicable;
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

Every gate reports one of four outcomes, and they are not interchangeable:

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
.\nk_manager.ps1 -Action BuildFast -TitleManifest assets/titles/hst-ucus98701.json -GameName hst  # runtime-only change
.\nk_manager.ps1 -Action BuildFull -TitleManifest assets/titles/hst-ucus98701.json -GameName hst  # codegen/pipeline change
python -m unittest discover -s tools -p "test_*.py" -v
python tools/publish_audit.py --tracked-only --worktree
```

Pass `--worktree` when auditing by hand: it reads the bytes on disk. The bare `--tracked-only`
form reads staged Git blobs, which is correct for the pre-commit hook (it stashes unstaged
changes first) but silently skips anything you have edited and not staged.

For documentation changes:

```powershell
npx --yes markdownlint-cli2@0.23.3
```

The repository also provides shared pre-commit hooks:

```powershell
python -m pip install pre-commit
python -m pre_commit install
python -m pre_commit run --all-files
```

`install` sets up both the commit and the push hook (`default_install_hook_types` in
`.pre-commit-config.yaml`). The publication audit runs at both stages on purpose: the push stage
audits the index, not each pushed commit, and a pushed branch is public, so only the commit stage
checks every commit before it can leave the machine.

The external-oracle `make verify` path requires inputs that are intentionally not in the repository.
When those inputs are unavailable, report the gate as blocked/unavailable rather than treating it as
a pass.

## Developer Certificate of Origin (DCO 1.1)

To ensure clear contribution rights, all contributions to this project must be certified under the **Developer Certificate of Origin (DCO 1.1)** via a `Signed-off-by:` line on each commit (`git commit -s`). The complete text of DCO 1.1, author obligations, maintainer standing waiver details, bot submission rules, and sign-off correction procedures are maintained in [`docs/DCO_POLICY.md`](docs/DCO_POLICY.md).

## Governance and roles

Nakagawa Recomp is a maintainer-led project.

- **Maintainer** ([@Jstar269](https://github.com/Jstar269)): sets direction, reviews and merges pull requests, owns
  releases, handles security reports ([SECURITY.md](SECURITY.md)) and enforces the
  [code of conduct](CODE_OF_CONDUCT.md). Decisions are made in public issues and pull requests whenever they are not
  security-sensitive.
- **Contributors**: anyone who opens an issue or pull request under this guide and the DCO.
- **AI agents**: used by the maintainer as assistants under [`AGENTS.md`](AGENTS.md); their commits say so with an
  `Assisted-by:` trailer, and a human maintainer remains accountable for everything merged.
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

Use a descriptive branch and commit message with DCO sign-off (`git commit -s`). The repository PR template prompts for the required
evidence. In every substantive pull request:

- explain the problem and approach;
- link the canonical GitHub issue(s) or concrete evidence;
- list exact tests/routes and results;
- call out anything blocked or unavailable;
- keep generated `build/` output and private/proprietary inputs out of Git;
- update the linked `ISSUES.md` status when the current milestone materially changes;
- certify DCO 1.1 sign-off status;
- disclose any new third-party source/data with exact source/revision/license; and
- disclose material AI-assisted translation/reimplementation so provenance can be reviewed.

A partial PR does not close an issue merely because CI is green. Record which acceptance criteria
remain unresolved in the issue/PR discussion.

Follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) in all project spaces. Report security issues
through [SECURITY.md](SECURITY.md), not a public issue when the reporting channel is available and
appropriate to the repository's current visibility.
