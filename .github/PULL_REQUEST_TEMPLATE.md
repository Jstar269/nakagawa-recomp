## Problem / evidence

<!-- Link the GitHub issue(s), failing test, trace, upstream reference, or other concrete evidence that motivates this change. -->

Closes/Tracks #

## Change

<!-- Describe the smallest behaviorally meaningful change and why this approach was chosen. -->

## Verification

<!-- List the exact commands/routes run and their results. Do not replace evidence with "CI green". -->

- [ ] `python -m unittest discover -s tools -p "test_*.py" -v`
- [ ] `python tools/publish_audit.py --tracked-only --worktree`
- [ ] `pre-commit run --all-files` (when available in the environment)
- [ ] Relevant native/runtime test or build gate for the changed area
- [ ] Dashboard test/lint/typecheck/build when `interface/` changes

### Results

<!-- Record exact results, including anything blocked or unavailable. -->

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
