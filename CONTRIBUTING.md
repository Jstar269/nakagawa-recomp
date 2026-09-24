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

**You don't need to handle provenance controls.** Two files, `PUBLIC_EXPORT.json` and
`assets/public_provenance_ledger.json`, are generated from a private trusted ledger that
contributors don't have. Don't edit them by hand. If **Trusted provenance attestation** or the
publication-safety step in **Hygiene and security** reports a provenance mismatch, a maintainer
refreshes those files on your branch. If you add a new file, say in the pull request where it came
from: written by you, or derived from which project, at which revision, and under which license. A
maintainer then admits the path.

### What the pull-request checks mean

| Check | What it verifies | If it fails |
| --- | --- | --- |
| Classify change | Which areas your change touches, so only relevant jobs run | Rarely fails; ask a maintainer |
| Hygiene and security | pre-commit hooks: Ruff, whitespace and encoding, publication safety, secret scanning | Run `python -m pre_commit run --files <your files>` locally |
| Markdown validation | markdownlint on changed Markdown | Run `npx --yes markdownlint-cli2@0.23.1 <file>` |
| Python tooling gates (0–3) | The `tools/` unit tests, split into four shards | Run the failing test module with `python -m unittest tools.<module>` |
| Native and translation gates | Strict C builds (`-std=c99`/`c11 -Werror`) and runtime selftests on Linux | Compile the changed C file with the flags shown in the log |
| Windows runtime compile gate | The runtime builds with MSYS2 UCRT64 on Windows | Check Windows-only APIs and headers |
| Dashboard checks | Only when `interface/` changes | Run the dashboard commands below |
| Trusted provenance attestation | Your change against the private trusted ledger | Provenance mismatches are handled by a maintainer |
| dependency-review, OSV, CodeQL | Dependency and static security scans | Read the finding; ask if unsure |
| CI required | The aggregate of the required jobs above | Fix the failing job it names |

Kilo Code Review is an advisory AI review and never blocks a merge.

## Before changing code

1. Read [AGENTS.md](AGENTS.md) and the maintained documentation relevant to your subsystem.
2. Search **GitHub Issues**, which are the canonical source of truth for actionable work and acceptance criteria, before opening a duplicate.
3. Use [ISSUES.md](ISSUES.md) as the concise status dashboard, not as a competing detailed issue tracker.
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
- Disclose third-party source origins and AI-assisted generation separately in pull request descriptions.
- For complete policy details, bot exceptions, and sign-off correction steps, see [docs/DCO_POLICY.md](docs/DCO_POLICY.md).

## Development setup

Follow the [authoritative development baseline in docs/SETUP.md](docs/SETUP.md). The core toolchain
is Windows 11 x64, PowerShell 7.4+ (`pwsh`), CPython 3.14.x, current MSYS2 UCRT64, and a current
auto-detected Vulkan SDK/loader. The separate dashboard uses npm and Next.js.

## Make a focused change

- Runtime C: follow `.clang-format`, use `sr_` for public symbols and `s_` for file-static state, and preserve the `CpuState` ABI.
- Python: follow `pyproject.toml`; update or add a focused test when changing codegen/tooling behavior.
- Dashboard: keep changes inside `interface/` and do not make the core build depend on Node.js.
- Documentation: update the maintained document, not an archived investigation. Keep `ISSUES.md`
  concise and link the canonical GitHub issue. When adding a confirmed defect or known limitation,
  update the canonical issue and its `ISSUES.md` dashboard link in the same change when applicable;
  label hypotheses and informational notes explicitly.
- Preserve existing SPDX, copyright, and provenance notices. For a new file, use an SPDX identifier only when its origin/license are actually known; do not invent a copyright owner or provenance claim.
- For project-authored source, use `GPL-3.0-or-later`; for inherited/upstream-derived source, preserve the SPDX identifier and notices required by its provenance record.
- Prefer general behavior/correctness fixes over address-specific compatibility overrides. Any unavoidable game-specific behavior needs evidence, a regression/route, and a retirement criterion.

## Verify

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

For dashboard changes:

```powershell
cd interface
npm ci
npm test
npm run lint
npm run typecheck
npm run build
```

For documentation changes:

```powershell
npx --yes markdownlint-cli2@0.23.1
```

The repository also provides shared pre-commit hooks:

```powershell
python -m pip install pre-commit
python -m pre_commit install
python -m pre_commit install --hook-type pre-push
python -m pre_commit run --all-files
```

The external-oracle `make verify` path requires inputs that are intentionally not in the repository.
When those inputs are unavailable, report the gate as blocked/unavailable rather than treating it as
a pass.

## Developer Certificate of Origin (DCO 1.1)

To ensure clear contribution rights, all contributions to this project must be certified under the **Developer Certificate of Origin (DCO 1.1)** via a `Signed-off-by:` line on each commit (`git commit -s`). The complete text of DCO 1.1, author obligations, maintainer standing waiver details, bot submission rules, and sign-off correction procedures are maintained in [`docs/DCO_POLICY.md`](docs/DCO_POLICY.md).

## Pull requests

Use a descriptive branch and commit message with DCO sign-off (`git commit -s`). The repository PR template prompts for the required
evidence. In every substantive pull request:

- explain the problem and approach;
- link the canonical GitHub issue(s) or concrete evidence;
- list exact tests/routes and results;
- call out anything blocked or unavailable;
- keep generated `build/` output and private/proprietary inputs out of Git;
- update the linked `ISSUES.md` dashboard state when the current milestone materially changes;
- certify DCO 1.1 sign-off status;
- disclose any new third-party source/data with exact source/revision/license; and
- disclose material AI-assisted translation/reimplementation so provenance can be reviewed.

A partial PR does not close an issue merely because CI is green. Record which acceptance criteria
remain unresolved in the issue/PR discussion.

Follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) in all project spaces. Report security issues
through [SECURITY.md](SECURITY.md), not a public issue when the reporting channel is available and
appropriate to the repository's current visibility.
