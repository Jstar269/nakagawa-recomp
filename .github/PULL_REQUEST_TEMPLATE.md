## Problem / evidence

<!-- Link the GitHub issue(s), failing test, trace, upstream reference, or other concrete evidence that motivates this change. -->

Closes/Tracks #

## Change

<!-- Describe the smallest behaviorally meaningful change and why this approach was chosen. -->

## New files and where they came from

<!-- Required when this pull request adds a file; delete this section if it adds none. -->

Every new file needs an answer, because the project cannot accept a file of unknown origin:

- [ ] Every new file is written by me, or derived from a named upstream at a named revision under a
      named license.
- [ ] Each new source file carries `SPDX-License-Identifier: GPL-3.0-or-later` and the project
      copyright line (Markdown and YAML use a comment form where the repository already does).
- [ ] Each new third-party or AI-assisted file is detailed under **Third-party and AI provenance**
      below.

| Path | Written by / derived from | Revision | License | Why it is needed |
| --- | --- | --- | --- | --- |
| | | | | |

A maintainer admits each new path into the publication scope and the protected provenance ledger.
That is expected and is not something you can do yourself.

## Verification

<!-- List the exact commands/routes run and their results. Do not replace evidence with "CI green". -->

Start with the fast path, which runs only the gates your changed files select:

```bash
make contrib-check          # Linux
mingw32-make contrib-check   # Windows
```

- [ ] `make contrib-check` — or the equivalent, with the result pasted below
- [ ] `python -m unittest discover -s tools -p "test_*.py" -v` (for a change to shared tooling)
- [ ] `python tools/publish_audit.py --tracked-only --worktree --provenance-self-consistency`
- [ ] `pre-commit run --all-files` (when available in the environment)
- [ ] Relevant native/runtime test or build gate for the changed area

### Results

<!-- Record exact results, including anything blocked or unavailable. -->

## Generated controls

<!-- These two files are maintained by a maintainer from a private trusted ledger. -->

- [ ] This pull request does not hand-edit `PUBLIC_EXPORT.json` or
      `assets/public_provenance_ledger.json`.
- [ ] A maintainer refreshes them after merge. No action needed from the author.
- [ ] If **Trusted provenance attestation** reported only "a maintainer will admit these N new
      files", that is the expected result for a pull request that adds files; it is not a defect in
      this change.

## Integration and release safety

- [ ] Exact `BASE_SHA` and `HEAD_SHA`, applicable hosted-CI status, review state, and remaining
      uncertainty are recorded below.
- [ ] No tag, GitHub Release, release asset, or published-version operation was performed. Those
      operations require explicit maintainer authorization in the current turn.
- [ ] No private-input, hardware, legal-clearance, provenance-attestation, or human DCO action is
      being represented as completed by this PR.

## Correctness / compatibility scope

- [ ] No unrelated compatibility workaround or fake-success path was introduced.
- [ ] Any new address-specific/game-specific override is narrowly scoped, documented, and linked to evidence/tests.
- [ ] Generated `build/` output is not committed.
- [ ] Private game inputs, decrypted modules, traces, local paths, and proprietary assets are not committed.

## Contributor Rights Attestation (DCO 1.1)

- [ ] All commits include a `Signed-off-by:` certification under Developer Certificate of Origin (DCO 1.1); see [docs/DCO_POLICY.md](../docs/DCO_POLICY.md).
- [ ] Or: these are maintainer / maintainer-directed commits covered by the standing waiver in [docs/DCO_POLICY.md §5.1](../docs/DCO_POLICY.md); a missing trailer is expected and is not a merge blocker.

<!-- Exactly one of the two boxes above applies. The waiver is personal to the maintainer: outside contributions always need the first. Agents must never add a Signed-off-by trailer on anyone's behalf. -->

Third-party and AI disclosure below is required either way — the waiver covers rights attestation only.

## Third-party and AI provenance

- [ ] No new third-party source/data was introduced.
- [ ] Or: new third-party material is identified below with exact source/revision/license and applicable notices preserved.
- [ ] No material AI-assisted translation/reimplementation was used.
- [ ] Or: material AI assistance is disclosed below with the source/provenance needed for review.

### Provenance notes

<!-- Required when either third-party material or material AI-assisted translation/reimplementation is introduced. -->

## Documentation / tracking

- [ ] Maintained documentation was updated when behavior, setup, architecture, or verification changed.
- [ ] The relevant GitHub issue/status tracking was updated when a blocker or acceptance criterion changed.
- [ ] Historical investigation documents were not rewritten as current-state documentation.

## Reviewer notes

<!-- Call out residual uncertainty, legal/provenance questions, private-input validation still required, or intentionally deferred work. -->
