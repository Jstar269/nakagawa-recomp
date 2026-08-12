# Public-source profile

`assets/public_source_profile.json` is the only publication-eligibility policy.
It uses explicit inclusion, explicit exclusions, and `REJECT` for every unknown
path. Exclusion always wins. A path is not eligible because it looks generic,
because a hook says so, or because a candidate asks for a relaxed mode.
The active sanitized public repository uses this same profile; a passing profile
check is an engineering containment result, not legal clearance.

## Excluded surfaces

- Title-specific HST configuration and private/game-derived documentation.
- PGF parser/font payloads and PGD/amctrl implementation/tooling pending
  qualified provenance and distribution review.
- The sal063-derived ISO/VFS and SDL audio backends pending their separate
  public review. `iso_unavailable.c` and `audio_unavailable.c` provide explicit
  public-safe link boundaries.

The excluded files remain local development material. They are not reconstructed
by the public candidate, and the public-safe Makefile cannot silently select them
when they are absent.

## Trust model

Publication commands should pass the intended policy, provenance ledger, and
manifest from an external trusted source. The auditor compares those bytes with
the candidate and verifies each included-file hash. Candidate-controlled policy,
ledger, manifest, hook text, or a marker file cannot grant publication clearance.

The tracked pre-commit hook runs the strict public-scope audit. There is no
candidate-selectable private-development or relaxed audit flag.

## Candidate construction

Build a new candidate with:

```text
python tools/build_public_export.py --public-safe-profile --export-dir <staging>
```

The generator archives one reviewed commit, filters the profile, writes the
deterministic `PUBLIC_EXPORT.json`, creates a single fresh candidate commit, and
then runs the candidate-tree audit. It does not publish, change repository
visibility, copy private refs, or move private inputs.

## Boundary claims

This profile is an engineering containment measure. It does not resolve the
underlying provenance or legal questions, prove clean-room authorship, sanitize
an unrelated history, establish PSP hardware correctness, or authorize a binary
release. Any future inclusion requires a new explicit record, notice, regression,
and human review of the actual candidate tree.
