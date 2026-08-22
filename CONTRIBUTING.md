# Contributing

Thanks for helping with Nakagawa Recomp. The repository-level project declaration is
GPL-3.0-or-later, while individual source files and inherited components may retain GPL-2.0-or-later
or other upstream-specific terms. Applicable third-party notices must be preserved. Specific
inherited licensing/provenance questions remain unresolved and under qualified review; do not introduce
or relabel third-party-derived code without resolving its actual source/license chain.

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
is Windows 11 x64, PowerShell 7.6+ (`pwsh`), CPython 3.14.x, current MSYS2 UCRT64, and a current
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
- Prefer general behavior/correctness fixes over address-specific compatibility overrides. Any unavoidable game-specific behavior needs evidence, a regression/route, and a retirement criterion.

## Verify

Run checks proportional to the change:

```powershell
.\hst_manager.ps1 -Action Test
.\hst_manager.ps1 -Action BuildFast -TitleManifest assets/titles/hst-ucus98701.json  # runtime-only change
.\hst_manager.ps1 -Action BuildFull -TitleManifest assets/titles/hst-ucus98701.json  # codegen/pipeline change
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
pre-commit install
pre-commit install --hook-type pre-push
pre-commit run --all-files
```

The external-oracle `make verify` path requires inputs that are intentionally not in the repository.
When those inputs are unavailable, report the gate as blocked/unavailable rather than treating it as
a pass.

## Developer Certificate of Origin (DCO 1.1)

To ensure clear contribution rights, all contributions to this project must be submitted under the
**Developer Certificate of Origin (DCO 1.1)**. By submitting a pull request or patch, you certify that:

```text
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

Include a `Signed-off-by:` line in every commit message (e.g. `git commit -s`):

```text
Signed-off-by: Random J Developer <random@example.com>
```

DCO sign-off is a certification of contribution rights, not a copyright assignment.

- **Commit Sign-Off**: The `Signed-off-by:` commit trailer is the canonical DCO attestation mechanism. PR template checkboxes provide documentation and review confirmation.
- **Automated/Bot Submissions**: Automated dependency updates or bot commits must identify their generator and be reviewed/signed off by the merging maintainer.
- **Historical Commits**: Historical commits prior to DCO adoption are retained as original author records; sign-off trailers will not be retroactively fabricated.
- **Maintainer Standing Waiver**: maintainer and maintainer-directed AI/agent commits are covered by the standing waiver in [docs/DCO_POLICY.md §5.1](docs/DCO_POLICY.md), which persists past public launch until explicitly revoked. A missing trailer on such a commit is not a merge blocker and is not corrected by rewriting history. Agents must never add a sign-off on anyone's behalf. Outside contributors are unaffected by the waiver.
- **Licensing Boundaries**: DCO sign-off certifies that the contributor has the authority to submit their work under the project's applicable terms. It does not settle combined-work license questions (such as PGF license review) or grant rights beyond the project's terms.

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
