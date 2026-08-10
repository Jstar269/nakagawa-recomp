# Dual-repository sync contract — private archive ↔ public source

Nakagawa Recomp is developed across two GitHub repositories with **intentionally
unrelated Git histories**. This document is the operational contract for keeping
them from diverging. It is an engineering process document, not legal advice, and
it does not change any publication gate in
[`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md).

| | Repository | Visibility |
| --- | --- | --- |
| Public source | `Jstar269/nakagawa-recomp` | **public** |
| Historical archive / development | development archive | **private** |

The public repository was created on 2026-08-10 from an approved `public-safe-v1`
export as a brand-new single-root history. It shares **no commit ancestry** with the
archive, and that is deliberate — see the topology rationale in
[`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) and
[`KEY_HISTORY_SCRUB.md`](KEY_HISTORY_SCRUB.md).

## Hard prohibitions

These are not style preferences. Each one exists to prevent a specific, concrete
failure:

- **Never merge the two histories.** No `git merge --allow-unrelated-histories`, no
  graft, no replace, no subtree join. Joining them would drag archive ancestry —
  including the pre-scrub key history — into a public repository.
- **Never mirror.** No `git push --mirror`, `--all`, or `--tags` between them.
- **Never push a private ref to public.** Not a branch, not a tag, not a PR ref, not
  an Actions ref, not an orphan object.
- **Never make the archive public**, and never publish by flipping visibility.
  GitHub exposes Actions history and logs on a visibility change and disables push
  rulesets.
- **Never rewrite either history** to make a checker happy.
- **Never add the other repository as a persistent remote** in a working clone. The
  risk is a stray `git push` going to the wrong place. Both repositories have been
  configured so that the *same* URL means different things than it did before the
  rename; confirm `git remote -v` before any push.

## What each side is canonical for

**PUBLIC is canonical for:**

- publishable generic source (recompiler tooling, generic runtime, synthetic tests);
- public documentation;
- public contributor pull requests;
- public CI and security state (Dependabot alerts, secret scanning, rulesets).

**PRIVATE is canonical for:**

- HST-derived and private evidence;
- private oracle material, routes, captures, framebuffer dumps;
- private game-specific development;
- historical private records, issues and PRs;
- the excluded PGF and PGD/amctrl implementations.

## Direction: PUBLIC → PRIVATE (backport)

When a fix lands publicly that also belongs in development source:

1. Classify the public commit:

   | Class | Meaning | Action |
   | --- | --- | --- |
   | **A** | generic/source fix | backport |
   | **B** | public-only presentation/governance | **do not** copy |
   | **C** | security/dependency fix | backport |
   | **D** | unclear | investigate before acting |

2. Branch from **current private `main`**.
3. **Transplant content, not ancestry.** Re-apply the change here — by editing the
   file, or by re-running the tool that produced it (for a lockfile, run
   `npm update <pkg>` in this tree rather than copying the public file). Then verify
   the result matches the public content.
4. Record the public commit SHA in the private commit message as provenance.
5. Run the private gates (below) and open one narrowly scoped PR.

Class B is the trap worth naming: public documentation that describes *the reader's
own repository* is legitimately public-only. Copying the public
`PUBLICATION_READINESS.md` banner into this archive would assert that this archive is
the public repository, which is false.

## Direction: PRIVATE → PUBLIC (export)

Generic work developed here reaches the public repository **only** through the
export path:

1. Generate a candidate: `python tools/build_public_export.py --public-safe-profile --export-dir <tmp>`.
2. Require `[EXPORT CLEARED]` and audit the exact candidate bytes with
   `python tools/publish_audit.py --candidate-root <tmp> --candidate-tree --public-scope`.
3. Land the change through a normal **public** PR from a public working clone.
4. Never push a private branch or ref directly to public.

## Outside public contributions

- Review them **publicly** first, on their merits, in the public repository.
- If accepted and useful to private development, backport their **content** into this
  archive with provenance (author, public PR number) preserved in the commit message.
- Do not expose private context — oracle traces, game-derived evidence, private paths
  — to the contributor.
- Outside contributors remain subject to the documented contributor policy in
  [`DCO_POLICY.md`](DCO_POLICY.md). The §5.1 maintainer standing waiver applies to
  maintainer and maintainer-directed work only, and does not weaken requirements for
  third parties.

## Working-tree hygiene

Use **separate working clones or worktrees** for the two repositories. Do not
check out public branches inside the private clone or vice versa. This repository's
primary worktree is contended by concurrent sessions; prefer
`git worktree add worktrees/<topic>` over switching branches in place.

## Drift check

[`tools/sync_drift_check.py`](../tools/sync_drift_check.py) answers one question:

> Would exporting private `main` today regress a generic fix that already landed on
> public `main`?

```bash
python tools/build_public_export.py --public-safe-profile --export-dir <export-dir>
git clone https://github.com/Jstar269/nakagawa-recomp.git <public-clone>
python tools/sync_drift_check.py --export-dir <export-dir> --public-dir <public-clone>
```

Every path lands in exactly one category:

| Category | Meaning | Verdict |
| --- | --- | --- |
| `EXPECTED_PUBLIC_ONLY` | on the curated public-only allowlist | pass |
| `EXPECTED_PRIVATE_EXCLUSION` | excluded by the profile, absent from both sides | pass |
| `GENERIC_DRIFT` | a path that should be identical is not | **fail** |
| `UNKNOWN` | cannot be confidently placed, incl. an excluded path actually present | **fail** |

It is **fail-closed**: unknown is always a failure, and the tool never copies or
repairs anything. Ancestry equality is deliberately not a criterion.

### Limitations

- Content hashing cannot tell which side is *older*. A differing text file is
  reported as `GENERIC_DRIFT` with both digests and needs human reading. The npm
  lockfile is the exception — package versions are ordered, so the tool reports an
  explicit `EXPORT BEHIND` verdict there.
- Text files are compared with line endings normalized to LF. The exporter
  materializes its working tree with host line endings while a fresh clone applies
  `.gitattributes` (`* text=auto eol=lf`), so byte-exact comparison reports every CRLF
  file as drift. Binary files (any NUL byte) are still compared byte-exactly, so
  pinned VFPU LUTs and fonts are never silently equated.
- New generic files that exist privately but have not been exported yet report as
  `UNKNOWN`. That is intended: private `main` being *ahead* of public is still a
  divergence the maintainer should resolve by exporting, not by silencing the tool.
- A clean report means the trees agree on generic content **at that instant**. It is
  not a publication gate and does not replace `publish_audit.py`.
- The public-only allowlist is curated by hand. A new legitimately-public-only file
  will report as `GENERIC_DRIFT` until it is added, which is the intended
  fail-closed direction.

## Private gates before a sync PR

Private Actions are disabled, so local gates are the evidence — never describe a
private branch as "CI-green":

```bash
python -m unittest discover -s tools -p "test_*.py"
python -m pre_commit run --all-files
python tools/publish_audit.py --tracked-only
python tools/publish_audit.py --tracked-only --worktree
python tools/import_audit_gate.py
python tools/verify_sbom.py
```

Plus, when the change touches them: the dashboard suite in `interface/`
(`npm ci && npm audit --audit-level=high && npm test && npm run test:db && npm run lint && npm run typecheck && npm run build`)
and the native selftests. A Makefile change affecting the PGF/PGD backend selection
must be proved in **both** modes, since public CI can only ever exercise
`PUBLIC_SAFE=1`:

```bash
mingw32-make --no-print-directory compiler-info          # expect PUBLIC_SAFE=0 here
mingw32-make GAME_NAME=ci BUILD_DIR=build/check CC=gcc hle-thread-selftest-build
```
