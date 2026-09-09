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
| `synthetic_fixture` (non-executable data under `fixtures/`, synthetic tests that are pure data) | no, deterministic | no |
| `unresolved` | cannot be published | — |

Deterministic classes describe what a file *is* by its path shape, so they can
never certify executable or security-sensitive content regardless of where it
sits. Executable tests/tools and source/script files (`*.py`, `*.ps1`, `*.sh`,
`*.cmd`/`*.bat`, C/C++ and other source suffixes, anything under `src/` or
`tools/`), CI workflows and actions (`.github/workflows/*`,
`.github/actions/*`), build and packaging fragments (Makefile, CMake,
`meson.build`, `*.mk`), and pre-commit hook/config surfaces are admitted or
refreshed only on implementation-grade authority with an exact record and blob
approval -- a filename such as `tools/test_*.py` can no more certify an
executable test as synthetic data than a `docs/` prefix can certify a script as
documentation (`ADMISSION_CLASS_ESCAPE`).

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
may be paired so a detailed ledger refreshes an existing snapshot. When pairing
them the detailed ledger goes to `--trusted-ledger` and the public snapshot to
`--trusted-baseline-ledger`, not the other way round; `--implementation-ledger`
belongs to the generate flow and is not read here. `--trusted-tree` accepts a Git
tree-ish, so pass the exact `BASE_SHA` the mission recorded. A remote-tracking
ref such as `origin/main` is mutable: once it advances past the commit the
candidate branched from it names a different tree, and the refresh then
compares against unrelated newer changes. A plain exported directory is
rejected outright because it is not a repository. Refreshing an
**implementation** class (`project_authored_attested`, `upstream_derived`,
`generated_from_public_source`) always requires the detailed ledger and an exact
record for that path: a snapshot alone cannot re-attest new bytes, because
historical snapshots still carry entries minted by removed fail-open rules and a
wildcard-derived claim must not follow a path onto content it never described.
Documentation, configuration, public metadata, and synthetic fixture paths may
still refresh from a snapshot alone while their deterministic class is unchanged.

Two consequences are worth stating plainly, because both look like tool faults:

* A refresh aborts with `CANDIDATE_TREE_STALE` when the candidate tree changes any
  path that was not named in `--paths`. Name every changed path. Mixing classes in
  one refresh is supported: given the required implementation records, a single
  invocation refreshes deterministic and implementation paths together, which
  `tools/test_provenance_ledger.py` covers directly. Splitting a branch is a remedy
  for the missing-record case below, not a rule about mixing.
* An implementation path whose public entry exists but which has **no record in the
  detailed ledger** cannot be refreshed by anyone, and fails with `TRUSTED_PATH_MISSING`.
  Such entries exist: they were minted by the retired `tools/*` wildcard expansion and
  the `interface/` configuration prefix, and the fail-closed rules deliberately refuse to
  carry them onto new bytes. Editing one of those paths blocks the publication gate until
  a genuine record is authored for it. Authoring that record is a maintainer attestation
  about who wrote the code; an agent must stop with `PROVENANCE_UNRESOLVED` instead.

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

## Reviewed refresh across an exact publication-policy delta

`refresh-reviewed` also requires the candidate's publication policy to equal
the trusted tree's policy, which makes a candidate that *also* carries a
reviewed policy change unrefreshable by the plain route. That is correct for an
unreviewed substitution, but it is not the same as an **independently blessed
delta**: the baseline was reviewed under the old policy, and a maintainer or
independent reviewer has separately reviewed the new policy's exact bytes and
the exact semantic difference. Issue #156 is the first real case (removing the
phantom `TODO.md` include entry -- the file was never tracked).

To cross such a delta the executor supplies two additional external inputs:
`--trusted-candidate-policy` (the blessed candidate policy bytes, never read
from the candidate tree) and `--policy-delta-authority` (an external document
binding the baseline digest, the blessed candidate digest, and the exact
allowed semantic delta):

```text
python tools/provenance_ledger.py refresh-reviewed \
  --trusted-ledger <external-trusted-ledger> \
  --candidate-tree <clean-candidate-worktree-or-immutable-ref> \
  --trusted-tree <trusted-baseline-worktree-or-immutable-ref> \
  --trusted-policy <external-baseline-policy> \
  --trusted-manifest <external-trusted-manifest> \
  --trusted-candidate-policy <external-blessed-candidate-policy.json> \
  --policy-delta-authority <external-policy-delta-authority.json> \
  --paths <exact-existing-public-path> [<exact-path> ...]
```

```json
{
  "schema_version": 1,
  "kind": "policy-delta-authority",
  "baseline_policy_sha256": "<sha256 of the trusted baseline policy bytes>",
  "candidate_policy_sha256": "<sha256 of the blessed candidate policy bytes>",
  "allowed_delta": {
    "include_added": [],
    "include_removed": ["TODO.md"],
    "exclude_added": [],
    "exclude_removed": [],
    "rule_changes": []
  }
}
```

Two policy contexts are kept strictly separate. The **trusted baseline** (its
tree, its public snapshot) is validated under the **baseline** policy; the
**candidate output** (tree boundary, refreshed paths, policy ledger entry,
`PUBLIC_EXPORT.json`) is validated under the **blessed candidate** policy. The
old snapshot is never re-validated under the new policy, and the new bytes are
never judged by the old one. The semantic delta is classified element-by-element
into `include_added`, `include_removed`, `exclude_added`, `exclude_removed`,
and `rule_changes`, and the computed delta must equal the authority's
`allowed_delta` exactly -- an extra include removal, an include addition, an
exclusion change, or any rule/control change (which V1 refuses on its face,
`POLICY_DELTA_AUTHORITY_INVALID`) fails closed. The policy's own ledger entry is
then updated to the blessed bytes and `PUBLIC_EXPORT.json` is regenerated under
the blessed policy; both are mechanical outputs of the trusted inputs, and the
candidate's own policy ledger entry is never read. A blessed file that equals
the baseline (`POLICY_DELTA_EMPTY`), an authority whose digests do not match
the actual files, unpaired flags, candidate-controlled inputs, or a candidate
whose policy differs from the blessed bytes all fail closed.

The policy file itself is one of the generated outputs of this operation, and
both it and the ledger/export are written as one transaction (see below); the
committed policy is then used as the next baseline for later refreshes.

## Transactional control outputs

Every mutating provenance command computes and validates all generated output
bytes first, stages each file next to its target, promotes the whole group only
after every stage succeeds, and rolls already-promoted files back to their
original bytes if any promotion fails. On a *detected* promotion failure the
affected worktree therefore ends in the complete old state, never a hybrid of a
new policy with an old ledger. This is staged replacement with rollback, not
crash or power-loss durability: if the rollback itself fails, or the process
dies between promotions, a hybrid can survive on disk. That residual is
fail-closed downstream rather than trusted -- the external attestation
re-validates the ledger/export/policy digest cross-checks, so partially written
controls are rejected instead of being read as authority. The generated control
set is:

* `assets/public_provenance_ledger.json` and `PUBLIC_EXPORT.json` for a
  `refresh-reviewed`;
* those two plus `assets/public_source_profile.json` for an
  `admit-new-reviewed` and for a `refresh-reviewed` crossing a blessed policy
delta.

A generated control is written to the literal path the publication policy
names, or not at all. The candidate's own control files are exempt from the
unrequested-change rule -- they are this operation's outputs -- so a candidate
*can* commit whatever it likes at those names, including a symlink. Containment
already refuses an output that leaves the candidate worktree; the write path
additionally refuses a symlinked component anywhere below the candidate root
and any target that is a symlink or not a regular file, so an in-worktree alias
can never steer a mechanical write onto another candidate file
(`REFRESH_OUTPUT_INVALID`, `ADMISSION_OUTPUT_INVALID`). Staging files are
created inside the target's own directory, written through the descriptor that
created them rather than reopened by name, and swept whether the write
succeeds, fails, or is rolled back.

Ledger ancestry stays a single current record, not a growing second history:
an `admit-new-reviewed` overwrites any prior `admission` block with the newest
admission, and a subsequent `refresh-reviewed` drops the `admission` block
entirely (its `refresh` block records the operation). The admission transaction
audit lives in the external authority document and the commit that carried it,
not inside the canonical ledger.

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
documentation, configuration, public factual metadata, and non-executable
synthetic data fixtures -- is admitted from the independent
exact-path/exact-bytes review alone, and the deterministic classifier derives
the class; the entry carries no record id. Executable tests/tools, source and
script files, CI workflows/actions, build/packaging fragments, and pre-commit
hook/config surfaces can never use a deterministic class even when their path
resembles one (`ADMISSION_CLASS_ESCAPE`). An implementation/source path
additionally requires an exact `records` entry and an exact `reviewed_blobs`
approval naming that path and that digest in the external trusted detailed
ledger, plus an authority statement declaring `origin_kind`
(`authored_from_scratch`, `derived_adapted`, or `third_party`), an origin
statement, and a license basis. The statement's `record_id` must equal the
exact covering detailed record's id *and* the blob approval's record id, so the
admission text cannot drift from the trust anchor it claims to cite. Its
`origin`/`license` fields are descriptive reviewer metadata: they prove the
reviewer considered origin and license, but they never authorize anything --
the public entry's class, origin, and license claims derive from the trusted
detailed record alone. Without the private-ledger record and blob approval the
command refuses (`BLOB_UNAPPROVED`, `TRUSTED_PATH_MISSING`, or
`TRUSTED_RECORD_REQUIRED`); a documentation-class authority can never admit
implementation, and anything the trusted policy excludes is unadmittable
through this route (`ADMISSION_PATH_EXCLUDED`).
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
