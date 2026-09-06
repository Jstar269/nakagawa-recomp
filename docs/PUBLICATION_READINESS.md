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

## Two authority tiers

Provenance is not one check. It is two, answering different questions, and a
change can satisfy one while failing the other.

| Tier | Question | Where it lives | Failure code |
| --- | --- | --- | --- |
| Path authority | may this path be published, and as what class? | `records` in the trusted detailed ledger, plus the deterministic classifier | `TRUSTED_PATH_MISSING`, `TRUSTED_PATH_UNQUALIFIED` |
| Blob authority | were these exact bytes at this exact path reviewed? | `reviewed_blobs` in the trusted detailed ledger | `BLOB_UNAPPROVED` |

The distinction is load-bearing. A record authorizes a *path*; it does not
authorize arbitrary new bytes placed at that path. Without the blob tier an
author could replace every byte of an authority-backed file and inherit its
attestation. Path coverage is not content approval.

So changing an implementation-bearing published file needs **both** an exact
record and a new `reviewed_blobs` entry naming its SHA-256. The record is usually
already present; the blob approval is a fresh human decision every time the bytes
change. `tools/provenance_attest_verify.py` enforces the blob tier and is
runnable locally — run it before claiming a candidate is ready.

An unchanged file needs no approval, so most changes touch this tier not at all.

| Class | Exact record? | Blob approval when bytes change? |
| --- | --- | --- |
| `project_authored_attested`, `upstream_derived`, `generated_from_public_source` | yes | **yes** |
| `reviewed_documentation`, `reviewed_configuration`, `public_factual_metadata`, `reviewed_other` | no, deterministic | no |
| `synthetic_fixture` (`tools/test_*`) | no, deterministic | no |
| `unresolved` | cannot be published | — |

An approval must cite the exact record already covering that path and a
classification agreeing with what authority derives; a mismatch is refused rather
than accepted. Never create a record just to turn a check green — if no record
covers the path, that is the finding.

## Every tracked change touches the published surface

Every tracked file in this repository is inside the published surface and the
provenance ledger carries a content hash for each one, so **any** tracked change
invalidates the ledger and `PUBLIC_EXPORT.json` until they are refreshed —
including a documentation-only edit. That is why the publication audit runs
ungated in the CI hygiene job on every event rather than being path-routed, and
why a cheap docs-only classification is nonetheless safe.

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

## Trusted admission of a genuinely new public path

`refresh-reviewed` can only re-attest bytes on a path a trusted baseline
already authorizes. A genuinely new path has no baseline authority at all, so
admitting one needs a second, deliberately distinct command:
`provenance_ledger.py admit-new-reviewed`. It is the initial trusted authority
for an exact path; `refresh-reviewed` is the subsequent content refresh. The
two commands fail closed against each other's inputs:

* a path already present in the trusted tree is refused by `admit-new-reviewed`
  with a pointer to `refresh-reviewed`;
* a path absent from the trusted tree is refused by `refresh-reviewed` with a
  pointer to `admit-new-reviewed`;
* a batch mixing existing and new paths is refused outright -- separate the
  refresh batch from the admission batch.

The trust boundary is the same as refresh, plus one new external input. All of
the following must live outside the candidate checkout: the trusted ledger
(public snapshot and/or the detailed development ledger), the trusted policy,
the optional trusted manifest, and an **admission authority** document the
independent reviewer produces after reviewing the exact candidate bytes:

```text
python tools/provenance_ledger.py admit-new-reviewed \
  --trusted-ledger <external-trusted-ledger-or-detailed-ledger> \
  --admission-authority <external-admission-authority.json> \
  --candidate-tree <clean-candidate-worktree> \
  --trusted-tree <trusted-baseline-worktree-or-immutable-ref> \
  --trusted-policy <external-trusted-policy> \
  --trusted-manifest <external-trusted-manifest> \
  --paths <exact-new-public-path> [<exact-path> ...]
```

The admission authority is the candidate-independent review decision. Each
statement binds one exact repository-relative path to one exact lowercase
SHA-256 of the bytes the reviewer approved, and names the public classification:

```json
{
  "schema_version": 1,
  "kind": "admission-authority",
  "reviewed_new_paths": [
    {"path": "docs/research/competitive/COMPETITIVE_GAP_AUDIT_2026-09-05.md",
     "sha256": "<64 lowercase hex>",
     "classification": "reviewed_documentation",
     "origin": "newly authored Nakagawa research documentation reviewed against public sources"}
  ]
}
```

Candidate bytes can never create their own authority. The command verifies all
of the following before it writes anything: the candidate worktree is clean;
the candidate differs from the trusted tree by exactly the admitted path set
and nothing else; every unrequested blob is byte-identical to the trusted
baseline; the candidate policy is semantically exactly the external trusted
policy plus include entries for the admitted paths; each path is absent from
the trusted tree and present (tracked) in the candidate; each authority digest
equals the candidate blob's SHA-256; and the trusted public ledger covers the
complete trusted public tree. The policy include, the ledger entry, and
`PUBLIC_EXPORT.json` are then regenerated mechanically from the external
inputs -- never accepted from the candidate -- and the ledger records the
`admission` block (workflow, trusted/candidate trees, admitted paths, and a
SHA-256 of the authority document) as audit ancestry.

Authority classes are not one size. Deterministic public material --
documentation, configuration, public factual metadata, synthetic fixtures and
source-owned tests -- is admitted from the independent exact-path/exact-bytes
review alone, and the deterministic classifier derives the class; the entry
carries no record id. An implementation/source path additionally requires an
exact `records` entry and an exact `reviewed_blobs` approval naming that path
and that digest in the external trusted detailed ledger, plus an authority
statement declaring `origin_kind` (`authored_from_scratch`, `derived_adapted`,
or `third_party`), an origin statement, and a license basis. Without that
private-ledger record and blob approval the command refuses (`BLOB_UNAPPROVED`,
`TRUSTED_PATH_MISSING`, or `TRUSTED_RECORD_REQUIRED`); a documentation-class
authority can never admit implementation, and anything the trusted policy
excludes is unadmittable through this route (`ADMISSION_PATH_EXCLUDED`).
Wildcards, directories, prefix/extension authority, duplicate paths,
path-traversal spellings, an authority naming different paths or different
bytes than the candidate carries, candidate-controlled trusted inputs, and
unrequested candidate mutations all fail closed.

Because this command creates the *initial* entry, the independent reviewer
must have reviewed the exact bytes before producing the authority document.
The implementer who authored the candidate does not create the authority;
separation of duties is a process rule enforced by the machine checks above.
After admission, commit the regenerated policy/ledger/export, then the release
process must still copy the resulting ledger to its trusted location, run
`publish_audit.py` against that external copy and the trusted manifest, and
run the non-attesting `--provenance-self-consistency` tripwire. Git
Issues/Projects/Milestones remain the live authority for which paths are
awaiting admission; this page only defines the route.

The repository or export is not cleared merely because these commands are
available. Record the exact commit/tree, outputs, and remaining human/hosted
gates.
