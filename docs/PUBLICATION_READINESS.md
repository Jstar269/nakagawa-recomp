# Public-source publication gates

This is the engineering gate definition for changes to the active sanitized
public repository and for any fresh source candidate or release export derived
from it. It is not legal advice, legal clearance, a binary-release approval, or
evidence that a title runtime is playable. The machine-readable policy and the
exact tree being assessed are authoritative; this page explains the decision
boundary.

## Scope

The active public source tree, and any candidate derived from it, contains
generic recompiler/runtime source, synthetic fixtures, and reviewed third-party
notices. It must not contain game binaries or assets,
decrypted modules, generated retail translation units, saves, keys, captures,
oracle traces, private routes, private repository metadata, or derived bytes.
Title-specific HST configuration and private engineering/review documents are
excluded by `assets/public_source_profile.json`.

The current public source profile also excludes the lineage-sensitive PGF/font and PGD/amctrl
surfaces, plus the reviewed-but-not-yet-cleared sal063-derived ISO/VFS and SDL
audio implementations. Public-safe builds link explicit unavailable boundaries;
they reject the capability and do not fabricate success.

## Required predicates

Every predicate below must pass for the same exact tree and history under review. A local
pass is not a hosted-CI pass, a hardware pass, a visual pass, a DCO attestation,
or a human legal decision.

1. **Exact tree binding.** The audit binds to the `git write-tree` result for the
   audited index, or to an immutable committed tree. A worktree with untracked or
   modified bytes is not publication evidence.
2. **Explicit policy and provenance.** Every included path is enumerated in the
   profile and has a concrete record in the externally trusted provenance ledger.
   Missing, unresolved, substituted, or self-authorized records fail closed.
3. **Candidate export.** `tools/build_public_export.py` produces one deterministic
   single-commit export. The candidate's policy, ledger, manifest, export digest,
   counts, and excluded paths are re-audited from the materialized bytes.
4. **History and object audit.** `tools/history_audit.py` scans every reachable
   commit, tree path, ref, and blob content in the proposed history. A clean tip
   is not sufficient.
5. **Supply-chain inventory.** The release manifest and SBOM cover the expected
   provenance families (sal063, PPSSPP, PSPSDK, FFmpeg/ATRAC3+, SDL3, Vulkan,
   shadcn/ui, and VFPU) with synchronized notices and lock data.
6. **Build and tests.** The public-safe target builds its generic source
   target and runs the source-owned regression gates. Missing private inputs or
   external oracles are reported as blocked/unavailable, never as passes.
7. **Documentation and governance.** Documentation contains no private/counsel
   work product or stale topology claims. Live repository visibility, rulesets,
   Actions behavior, DCO, and maintainer authorization are verified separately
   against the actual destination repository.

## Reproducible local sequence

```text
python tools/provenance_ledger.py
python tools/policy_sync.py --regen-export
python tools/history_audit.py --json
python tools/verify_sbom.py
python tools/build_public_export.py --public-safe-profile --export-dir <staging>
python tools/publish_audit.py --candidate-root <staging> --candidate-tree --public-scope \
  --provenance-ledger assets/public_provenance_ledger.json
```

The first command regenerates the public provenance ledger from the detailed
development ledger, which may stay outside the public tree. Classification is
fail-closed: a path-specific record is the only way an implementation path is
attested, `tools/*`-style wildcard records are never expanded, and the
generator refuses to write a ledger while any included path is unresolved.
The public ledger is therefore not produced until the detailed ledger actually
records the missing paths; `--check` validates the checked-in ledger without
regenerating it.

**The ledger is evidence, never an authorization source.** A candidate's own
checked-in ledger cannot attest its own provenance: a contributor could edit
`assets/public_provenance_ledger.json` to name a plausible detailed-ledger
record that is not present in the trusted release evidence. The audit
therefore fails closed on the trust anchor:

* with `--provenance-ledger <trusted copy>` the audited ledger must match the
externally trusted ledger byte-for-byte, so any self-authored record fails;
* a bare audit (no flag) reports `PROVENANCE_UNVERIFIED` and fails instead of
passing on the candidate's own bytes;
* `--provenance-self-consistency` is the explicitly non-attesting developer
tripwire scope (pre-commit and `hst_manager.ps1 -Action Verify`): coverage,
resolution, and content hashes are enforced against the audited ledger itself,
but no attestation claim is made or cleared.

The release flow above regenerates the ledger from the detailed development
ledger and then attests the export against that regenerated copy. Candidate
hashes prove bytes, not authorization; only a record in the trusted detailed
ledger attests a path.

## Reviewed refresh of an existing public path

A legitimate edit to an already-qualified public path needs a new
content hash, but the candidate must not be able to turn that edit into its own
provenance claim. The maintainer-controlled refresh workflow is:

```text
python tools/provenance_ledger.py refresh-reviewed \
  --trusted-ledger <external-trusted-ledger-or-detailed-ledger> \
  --candidate-tree <clean-candidate-worktree-or-immutable-ref> \
  --trusted-tree <trusted-baseline-worktree-or-immutable-ref> \
  --trusted-policy <external-trusted-policy> \
  --trusted-manifest <external-trusted-manifest> \
  --paths <exact-existing-public-path> [<exact-path> ...]
```

The trusted ledger and policy must be outside the candidate checkout. When the
trusted baseline is supplied as a worktree, it must also be outside the
candidate checkout; the refresh outputs may not overwrite that trusted tree. A public
ledger snapshot supplies the existing public entry objects; the command also
accepts an external detailed ledger with exact `records` entries, and the two
may be paired so a detailed ledger refreshes an existing snapshot. Refreshing an
**implementation** class (`project_authored_attested`, `upstream_derived`,
`generated_from_public_source`) always requires the detailed ledger and an exact
record for that path: a snapshot alone cannot re-attest new bytes, because
historical snapshots still carry entries minted by removed fail-open rules and a
wildcard-derived claim must not follow a path onto content it never described.
Documentation, configuration, public metadata, and synthetic fixture paths may
still refresh from a snapshot alone while their deterministic class is unchanged.
The command never treats the candidate's
`assets/public_provenance_ledger.json`, policy, manifest, or export as trusted.
The candidate's current ledger and export may differ from the trusted baseline
because they are the two generated outputs of this operation; those bytes are
ignored as inputs and replaced by the deterministic outputs below. The policy
and manifest remain independently checked against their external trusted copies.

Before writing either generated artifact, the command verifies all of the
following: the candidate worktree is clean; the candidate and trusted trees
have the same path set; every unrequested blob is byte-identical to the trusted
baseline; the candidate policy matches the external policy; the requested paths
are explicit exact files already present in the trusted tree; their trusted
ledger class is either an implementation class backed by an exact detailed
record or a matching deterministic public class; and the trusted public ledger
covers the complete trusted public tree. Wildcards, directory authorizations,
missing implementation records, new paths, stale candidates, policy
substitutions, and private or unclassified tree content fail closed. Existing
documentation, configuration, public metadata, and synthetic fixture paths may
refresh their hash only when their deterministic class remains unchanged. Only
the `sha256` values for the listed paths are changed. The output records the
trusted and candidate tree IDs and the exact refreshed path set without
inventing a person, DCO trailer, or provenance attestation.

That `refresh` block is audit ancestry, not an identity claim about the tree
that carries the ledger. The candidate tree it names is the tree read *before*
the regenerated ledger and export were written, so it can never equal the tree
that then contains them; a snapshot is instead bound to a tree by content, since
validation requires every non-control entry hash to equal that tree's blob.
A generated public ledger is therefore reusable as the next trusted baseline
once its outputs are committed, and a snapshot from any other tree still fails
closed on the first hash that disagrees.

For each changed or new path, use this disposition before invoking the command:

| Disposition | Meaning |
| --- | --- |
| `PASS` | The path is unchanged from the trusted tree; no refresh is needed. |
| `TRUSTED_REFRESHABLE` | The path already exists in the trusted tree and has either an exact trusted implementation record or an unchanged deterministic class. |
| `PROVENANCE_RECORD_REQUIRED` | The path is new or implementation-bearing without an exact trusted detailed record; create or confirm that record in the trusted ledger first. |
| `UNRESOLVED` | The trusted class is unresolved, substituted, or disagrees with the detailed record; stop and report the missing fact. |

The command accepts only `TRUSTED_REFRESHABLE` paths. A new implementation
path is always `PROVENANCE_RECORD_REQUIRED`, even if a candidate adds a public
ledger entry for it. Candidate-controlled ledger, policy, manifest, and export
bytes never change these dispositions.

The resulting ledger and `PUBLIC_EXPORT.json` are mechanical outputs, not
authorization. The release process must copy the refreshed ledger to its
trusted location, run `publish_audit.py` against that external copy and the
trusted manifest, and then run the non-attesting
`--provenance-self-consistency` tripwire. A dashboard source file such as
`interface/src/components/studio/test-lab-panel.tsx` cannot use a
`reviewed_configuration` record; it remains blocked until a maintainer creates
or confirms an exact trusted implementation record. The refresh command does
not merge or otherwise authorize an unrelated dashboard change.

The repository or export is not cleared merely because these commands are
available. Record the exact commit/tree, outputs, and remaining human/hosted
gates.
