# Trusted provenance merge gate

This document specifies the merge-path control that verifies a pull request's
public provenance ledger against an **externally trusted authority**, rather
than against itself.

It complements, and does not replace,
[`docs/PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md): that document owns
the publication boundary, `publish_audit.py` owns candidate-tree auditing, and
`provenance_ledger.py` owns generation and the maintainer's `refresh-reviewed`
workflow. This gate owns one question only — *does external authority back the
attestations this pull request is asking to merge?*

## The gap this closes

Before this gate, an ordinary pull request could satisfy every provenance check
in CI while asserting provenance nothing outside the pull request had ever
agreed to.

| Existing control | What it proves | Why it is not merge-path authority |
| --- | --- | --- |
| `provenance_ledger.py --check` | the checked-in ledger is structurally valid: coverage, resolution, hashes | it says so itself — without the detailed ledger it "cannot authenticate attestation claims" |
| `publish_audit.py --provenance-self-consistency` | the candidate agrees with the candidate | explicitly non-attesting; a developer tripwire |
| `publish_audit.py --provenance-ledger <trusted>` | the candidate byte-matches a release-controlled snapshot | that snapshot only exists for a tree the release process already blessed, so it cannot run on an arbitrary pull request |
| `provenance_ledger.py refresh-reviewed` (#138) | a maintainer can refresh hashes using only external trusted inputs | it is a *generator* run by a maintainer, not a check on the merge path |

Every one of those either consumes candidate-controlled bytes as its anchor, or
cannot run on an ordinary pull request at all. The merge path therefore
verified self-consistency and called it provenance.

## Threat model

The adversary is a pull request author who knows this implementation exactly.
They can write any bytes into the candidate tree and can make every artifact in
it agree with every other artifact.

| # | Attack | Control |
| --- | --- | --- |
| T1 | Add an implementation file with a well-formed ledger entry naming a plausible but non-existent detailed record | `TRUSTED_RECORD_UNRESOLVED` |
| T2 | Point a new path at a real record that says nothing about it | `TRUSTED_RECORD_UNRESOLVED` — deliberately the same code and wording as T1 |
| T3 | Add a path under a blanket record such as `tools/*` | `CLAIM_UNBACKED` — wildcard records are inert for classification |
| T4 | Relabel an implementation path as `reviewed_configuration` / `reviewed_documentation` / `synthetic_fixture` so no record is needed | `CLAIM_UNBACKED` — the deterministic class must actually fit the path |
| T5 | Upgrade an existing path from `upstream_derived` to `project_authored_attested` | `CLAIM_UNBACKED` — the claim changed, so it must match authority |
| T6 | Commit a forged `docs/provenance/IMPLEMENTATION_PROVENANCE.json` into the candidate tree | the verifier reads authority from outside the repository and refuses a trusted input inside it |
| T7 | Replace the in-tree verifier with a rubber stamp | the verifier that runs comes from the trusted base ref, never from the candidate |
| T8 | Neuter the gate's workflow, or delete it | `TRUSTED_WORKFLOW_WEAKENED` |
| T9 | Declare a second job under the required check's context name so a green candidate-controlled result answers for the gate | `CI_CONTEXT_COLLISION` — the context string is reserved |
| T10 | Change file content without updating the ledger hash | `CONTENT_MISMATCH` |
| T11 | Publish a file with no ledger entry, or keep an entry for a deleted file | `LEDGER_COVERAGE` |
| T12 | Remove an exclusion from the publication policy | `POLICY_SUBSTITUTION` — scope may tighten, never loosen |
| T13 | Edit the ledger without regenerating the export | `EXPORT_FIELD_MISMATCH` |
| T14 | Replay a green result from an earlier head | the verdict is bound to the candidate commit and tree, and the run fails closed if the fetched head is not the head the event named |
| T15 | Merge on a stale base | the ruleset's `strict_required_status_checks_policy` requires the head to be up to date, and the ratchet is evaluated against the base branch tip |
| T16 | Add a symlink whose "content" is a path string | `TREE_INVALID` — provenance is only defined over regular file content |
| T17 | Replace an existing file's implementation bytes, update the public hash coherently, and inherit the claim already recorded for that path | `CONTENT_UNATTESTED` — grandfathering requires the bytes to be frozen too |
| T18 | Rename or copy an attested file so the new path inherits the old path's record | `TRUSTED_RECORD_UNRESOLVED` + `CLAIM_UNBACKED` + `BLOB_UNAPPROVED` — records name paths, and a new path is new |
| T19 | Open a pull request whose **base** is an attacker-controlled branch, so the "trusted" checkout is the attacker's tree and the job runs their verifier with the authority token | the workflow checks out `github.event.repository.default_branch`, never `base.ref`, and declines to run on any other base |
| T20 | Put shell metacharacters or newlines in a path, branch name, PR title or body to break out of the trusted job's scripts | no PR title/body/branch name is referenced at all; the two event values used are validated as numeric / 40-hex and passed through `env:`, never interpolated into a script body |
| T21 | Add `.gitattributes` filters, submodules or gitlinks so a checkout executes something | the candidate is never checked out; `git cat-file` returns raw object bytes, non-blob entries are refused, and the fetch is `--no-recurse-submodules` |
| T22 | Commit enough content to exhaust the runner | `TREE_TOO_LARGE` — explicit path-count and byte ceilings |
| T23 | Ship a candidate action definition under `.github/actions/` for the trusted job to run | the trusted workflow references only SHA-pinned `actions/*`; nothing resolves from the candidate tree |
| T24 | Replace every byte of an **authority-backed** file, keeping its record and classification, and update the public hash coherently | `BLOB_UNAPPROVED` — the authority must name the exact new digest |
| T25 | Write an "approved" digest into the candidate's own public ledger evidence | `BLOB_UNAPPROVED` — approvals are read only from the private authority; the gate hashes the Git object itself |
| T26 | Reuse an approval issued for a different path, or for an earlier revision of the same path | `BLOB_UNAPPROVED` — approvals key on (path, digest) |
| T27 | Cite a blanket record in a blob approval to authorize replacement content | `BLOB_APPROVAL_RECORD_MISMATCH` — the approval must cite the exact record `_class_for` derives, and wildcards never produce one |
| T27b | Repeat one `(path, digest)` approval in the authority so two readings disagree | refused as ambiguous authority; several *different* digests for one path stay legitimate, and only the exact candidate digest matches |
| T28 | Approve a blob under one classification while the ledger claims another | `BLOB_APPROVAL_CLASS_MISMATCH` |
| T29 | Have the gate judge a mutable branch name so two resolutions name different trees | `MUTABLE_REVISION_REFUSED` — CI passes the event's exact 40-hex head and base SHAs, and the base must be an ancestor of the default branch |
| T30 | Swap the authority between "what revision is main" and "read main" | the workflow resolves the authority commit once and reads the ledger **at that SHA**; the revision and the ledger's own SHA-256 are both reported |
| T31 | Change an implementation file, drop it from `include_paths`, delete its ledger entry and regenerate the export, so the gate stops seeing the path | `TRUSTED_SCOPE_VIOLATION` — scope is anchored to the trusted base policy over the trusted base tree and may only widen |
| T32 | Add an exclusion covering a changed implementation path | `TRUSTED_SCOPE_VIOLATION` — the same rule; removing a path from coverage is removing a path from coverage |
| T33 | Add a new implementation file and simply leave it out of the policy | `TRUSTED_SCOPE_VIOLATION` — a new implementation path must enter the universe unless the **trusted** policy excludes it |
| T34 | Relabel an implementation path as documentation, configuration or a fixture to escape content gating | `CLASSIFICATION_DOWNGRADE` — measured against trusted derivation, so correcting a genuine over-claim is still allowed |
| T35 | Forge `policy_sha256`, `included_content_sha256`, counts or exclusion lists while keeping `provenance_ledger_sha256` correct | `EXPORT_FIELD_MISMATCH` — the whole export is recomputed and compared field by field |
| T36 | Repeat a JSON object key so the verifier and a downstream reader disagree about which value wins | the strict parser refuses any duplicate key in the ledger, policy, export or authority before semantics run |
| T37 | Spell a path in NFD, or with a backslash, control character or dot component, so two Git paths collapse to one identity | `TREE_INVALID` — paths must be canonical NFC POSIX relative, and ambiguity is refused rather than normalized |
| T38 | Probe which private record ids exist by watching whether the gate says "absent" or "does not cover" | one `TRUSTED_RECORD_UNRESOLVED` code with one wording, and the claimed id is not echoed |
| T39 | Exhaust the runner with a huge tree, a huge blob or huge path text | `TREE_TOO_LARGE` — ceilings are enforced as the bytes stream in, not after buffering |
| T40 | Merge a pull request whose head is behind base so conflict-resolution bytes appear after verification | `MERGE_BASE_STALE` — the base must already be an ancestor of the head, which makes the head tree the merge result for all three allowed methods |

## Trust boundary

**Trusted.** The verifier `tools/provenance_attest_verify.py`, the workflow
`.github/workflows/provenance-attestation.yml`, the publication policy, and the
previous public ledger — all taken from the base branch, not the pull request.
The detailed implementation ledger, fetched at run time from the private
authority repository into `$RUNNER_TEMP`, outside the workspace.

**Untrusted.** Every blob in the candidate tree, without exception: its source,
its policy, its ledger, its export, its workflows, and its own copy of the
verifier.

The boundary is enforced structurally, not by convention:

* the workflow is `pull_request_target`, so its definition is the base
  branch's. A `pull_request` workflow would hand the definition to the
  candidate, who could keep the required check's name and replace its body;
* the candidate is **never checked out**. Its objects are fetched into the
  trusted checkout and read with `git cat-file`, so no candidate hook, build
  file, workflow, or Python module can execute in the job that holds the
  private-repository token;
* `verify()` refuses to run at all if the trusted detailed ledger resolves to a
  path inside the repository under verification.

## Rule tiers

**Tier A — absolute, whole tree, never grandfathered.** `LEDGER_SCHEMA`,
`LEDGER_COVERAGE`, `CONTENT_MISMATCH`, `RECORD_ABSENT`, `RECORD_NOT_COVERING`,
`POLICY_SUBSTITUTION`, `EXPORT_LEDGER_DIGEST_STALE`,
`TRUSTED_WORKFLOW_WEAKENED`, `CI_CONTEXT_COLLISION`.

**Tier B — the grandfathering predicate.** The trusted authority derives
exactly one claim for a path. A candidate claim that disagrees with it survives
**only while both halves of the reviewed state are frozen**:

> A path's attestation claim may deviate from trusted authority **only if** the
> claim is byte-for-byte what the trusted **base ledger** recorded **and** the
> file's content is byte-for-byte what the trusted **base tree** recorded.
> Break either half and the claim must match what authority derives, exactly.

* `CLAIM_UNBACKED` — the claim is new or changed and authority does not derive it.
* `CONTENT_UNATTESTED` — the claim is inherited unchanged, but the bytes are not
  the reviewed bytes.

Freezing the claim alone was the first design and it was wrong: a candidate
could modify implementation content, update the public hash coherently, leave
classification and record id untouched, and inherit an attestation that was
never made about those bytes. Content identity is part of the tuple, not a
side note.

A path whose claim **agrees** with authority is unconstrained — its content may
change freely. That is the project's provenance model: records carry `paths`,
not digests, so an attested path is attested, not frozen. It also means the
only way to unfreeze a path in debt is to pay the debt, never to except it.

**Tier B2 — exact-blob authorization.** Tier B still allowed one thing it
should not: a path whose claim *agrees* with authority could have its content
replaced wholesale, because records carry `paths`, not digests. Path coverage
is not content approval.

> For every implementation-class path, if the candidate blob is **new or
> differs from the trusted base blob**, the trusted authority must contain a
> `reviewed_blobs` approval naming **exactly this path and exactly this
> SHA-256**, citing the exact record that covers the path and the same
> classification. The gate hashes the candidate blob itself, from the Git
> object — never from any digest the candidate wrote.

* `BLOB_UNAPPROVED` — no approval for these exact bytes at this exact path.
* `BLOB_APPROVAL_RECORD_MISMATCH` — the approval cites a record that is not the
  exact record covering the path. A wildcard record can never satisfy this,
  because `_class_for` derives `record_id` only from exact records.
* `BLOB_APPROVAL_CLASS_MISMATCH` — the approval authorizes a different
  classification than the ledger claims or authority derives.

Only **implementation classes** are content-gated — 289 of the 658 public
paths. Documentation, configuration, fixtures and public metadata are
classified by what a file *is*, re-derived every run, and need no per-revision
approval. **Unchanged blobs keep whatever authorization they already had**, so
adopting the rule does not require approving the existing tree in one go.

### Authority schema addition

A top-level `reviewed_blobs` array in the private detailed ledger, sibling to
`records`. It is a separate document on purpose: records are low-churn prose
attestations, approvals are high-churn digests, and folding digests into
records would make every content review rewrite a provenance statement.

```json
{
  "reviewed_blobs": [
    {
      "path": "src/rt/guest_interp.c",
      "sha256": "<64 lowercase hex of the exact reviewed blob>",
      "classification": "behavior-informed",
      "record_id": "daybreak4-guest-interpreter"
    }
  ]
}
```

The gate validates each approval: exact normalized POSIX path (wildcards
refused), lowercase 64-hex digest, a `record_id` that exists in `records`, and
a classification that maps — through `provenance_ledger._class_for`, so there
is one mapping and it cannot drift — to the claimed public class. Any further
keys (reviewer, date, notes) are **ignored and never printed**.

### Operational consequence

This converts the gate from pure verification into **explicit per-revision
approval for implementation content**. Every pull request that changes one of
the 289 content-gated paths needs a matching approval in the private authority
before it can go green. That is the security property that was asked for, and
it is a real workflow cost: authority first, then the pull request.

### The trusted protected universe

Scope is the foundation every other rule stands on, and it was
candidate-controlled. An independent review reproduced the full bypass against
the third-pass verifier on this repository: change `src/rt/title_config.c`,
remove it from `include_paths`, add an exclusion, delete its ledger entry,
regenerate the export coherently — **PASS, zero findings**.

Scope is now anchored outside the candidate:

* the **trusted universe** is what the *trusted base policy* includes in the
  *trusted base tree*;
* a path in that universe that still exists in the candidate tree must still be
  included by the candidate policy — otherwise `TRUSTED_SCOPE_VIOLATION`. This
  one rule covers `include_paths` removal, exclusion addition, wildcard
  narrowing and parent-directory games alike, because all of them end in the
  same place: the path left coverage;
* a **new** implementation-bearing path must enter the universe unless the
  **trusted** policy's own exclusions cover it. The candidate cannot except
  itself;
* **deletion is allowed** — a path genuinely removed from the tree leaves the
  universe honestly. What it may not do is make changed bytes disappear;
* the universe the gate then reasons over is `(inherited | candidate_included)`,
  so coverage may always widen and never narrow.

### Canonical export recomputation

Verifying one export field bought silence on all the others: `policy_sha256`,
`included_content_sha256`, the counts and the exclusion lists were all accepted
from the candidate as long as the ledger digest was right. The gate now
rebuilds the entire export with `public_export.build_document` — the same
generator the release process uses — from the candidate tree and the policy,
and compares **every field**, reporting `EXPORT_FIELD_MISMATCH` for any
difference, any missing field and any invented one.

`candidate_tree` and `source_tree` are the two advisory exceptions: the release
process records the pre-export tree there, so they are informational. The real
tree binding is the verdict's own `candidate_tree`.

### Strict parsing and path canonicalization

`json.loads` keeps the **last** value for a duplicated key. Two readers that
disagree about which one wins is a place to smuggle a second value past review,
so every security-relevant document — public ledger, policy, export, external
authority, `reviewed_blobs` — is parsed with a hook that refuses a repeated key
outright, before any semantics run.

Git will store almost any byte string as a path. Provenance needs one identity
per file, so this project accepts only **NFC-normalized UTF-8 POSIX relative
paths** with no control characters, no backslash, no CR/LF, no leading or
trailing separator, no empty or dot components, and within explicit length,
component and depth ceilings. Non-canonical spellings are **refused, never
normalized**: folding two distinct Git paths onto one identity is exactly how a
second file would inherit the first one's approval.

### Resource ceilings, enforced on the way in

Measured on this repository: 658 paths, longest path 68 bytes, longest
component 38 bytes, depth 9, largest blob 2 MiB, 14.8 MB of content. The
ceilings sit at least an order of magnitude above that — 50,000 paths, 512-byte
paths, 255-byte components, depth 32, 8 MiB of path text, 64 MiB per blob,
512 MiB per tree, 32 MiB per JSON document, 100,000 approvals — and they are
enforced **as the bytes stream in**: `ls-tree` output is parsed incrementally
and `cat-file` blob sizes are checked from the header before the blob is read,
so an attacker-shaped tree is refused rather than materialized first.

### Merge-tree identity

The repository allows **merge, squash and rebase**. All three produce a final
tree equal to the pull request head tree — but only when the base is already an
ancestor of the head, because then there is nothing to reconcile and no
conflict-resolution bytes can appear after verification. The gate therefore
refuses `MERGE_BASE_STALE` when the head is behind its base.

That makes head-tree attestation sufficient **for the verifier's side**. It is
not a complete merge-time guarantee: nothing stops base from advancing between
verification and the merge button. Closing that needs
`strict_required_status_checks_policy`, which only takes effect once a required
check exists — and enforcement is deliberately still off. Hence PARTIAL.

**Tier C — reported, never fatal.** Paths where both halves are frozen and the
claim still disagrees with authority. Each carries a `backing` value naming
what authority actually says about the path, because that decides the remedy:

| `backing` | meaning | remedy |
| --- | --- | --- |
| `exact` | a trusted record names the path verbatim | correct the public entry to what authority derives |
| `deterministic` | no record needed — documentation/configuration/fixture/metadata by path rule | correct the public entry |
| `blanket` | only a wildcard record covers it, and wildcards are inert | a trusted record must exist first |
| `none` | authority says nothing about the path | a trusted record must exist first |

A wildcard is deliberately not authorization. `provenance_ledger.py` refuses to
expand one so a new file cannot inherit an old blanket attestation; letting a
blanket authorize *replacement content* for an already-listed file would
reintroduce the same hole through the back door.

## What the gate found on `main`

Measured against `origin/main` at `421016b1faf3f6473bbd7c20d67be315aa4302d5`
and the private authority at `private/main`:

* **2 fatal findings.** `src/rt/guest_interp.c` and `src/rt/guest_interp.h`
  name `record_id: daybreak4-guest-interpreter`. That record does not exist in
  the trusted detailed ledger. See
  [`docs/provenance/GUEST_INTERP_ATTESTATION.md`](provenance/GUEST_INTERP_ATTESTATION.md).
* **316 grandfathered disagreements**, split by `backing`:

  | count | backing | what it is |
  | --- | --- | --- |
  | 113 | `deterministic` | over-claims: `tools/test_*.py` and config/doc/metadata files claiming `project_authored_attested` when the generator itself derives a deterministic class |
  | 64 | `blanket` | `tools/` paths whose entries expand the `tools/*` catch-all, which the generator has since made deliberately inert |
  | 139 | `none` | 134 `interface/` dashboard paths classified `reviewed_configuration` although `.ts`/`.tsx`/`.mjs`/`.css` are implementation with no record; 3 root `.ps1` scripts; the 2 `guest_interp` paths |

None of the 316 is a forged anchor: they are historical over-claims that
predate the current classification rules.

**The 113 `deterministic` entries are paid down in their own commit**, kept
separate from the security mechanism so the trust boundary is reviewable
without a large metadata diff on top of it. It is not merely cosmetic: under
exact-blob authorization an implementation-class claim makes a path
content-gated, so leaving 101 `tools/test_*.py` files claiming
`project_authored_attested` would demand a maintainer blob approval for every
test edit. Correcting them to the class the generator itself derives is what
keeps the mechanism usable.

Each is rewritten to exactly the classification and evidence
`provenance_ledger.py` derives for it — 101 test files to `synthetic_fixture`,
7 configuration files, 3 documentation files, 2 title manifests. Every
transition is a downgrade or a lateral move to the derived class; nothing is
upgraded and nothing is invented. That leaves **203 paths in debt**, all of
which need a trusted record before their bytes may change.

### Deployment consequence, measured

Under the Tier B predicate a pull request that modifies a path still in debt
fails with `CONTENT_UNATTESTED`. Measured on this branch, **204 of 658 public
paths are frozen and 454 may change freely**:

| area | frozen | of total |
| --- | --- | --- |
| `interface/` (dashboard) | 134 | 148 |
| `tools/` | 65 | 225 |
| `src/rt/` (runtime) | 2 | 152 |
| root `.ps1` scripts | 3 | 3 |
| `docs/` | 0 | 32 |
| `fixtures/` | 0 | 26 |

Runtime, documentation and fixture work is therefore essentially unaffected —
the two frozen `src/rt` paths are the `guest_interp` pair, which is the finding
this gate exists to surface. The dashboard is effectively blocked, and 65
`tools/` files are blocked. That is the correct security answer and it is a real
cost; it is not something the gate should be relaxed to avoid. Two maintainer
edits to the trusted authority clear almost all of it:

* replace the `tools/*` catch-all with exact per-file paths — unblocks 64;
* add a record covering the dashboard — unblocks 134.

Until then, enabling the required check blocks those two areas. That trade is
the maintainer's to make, which is why the required-check step is ordered last.

## Output discipline

The private ledger is never copied into the public tree, never uploaded as an
artifact, and never printed. The verifier emits only public data: repository
paths, classification names, finding codes, and record ids **that the public
ledger already names**. A trusted record id the public ledger does not carry is
reported as `<withheld: private record id>`; redaction is display-only and can
never change a verdict. The machine-readable verdict retains the trusted-ledger
digest for local binding, but the human/public report emits only aggregate
record and approval counts plus the authority repository's commit SHA.
`--show-debt` keeps the path/backing summary and withholds detailed record
fields. Nothing else about the private ledger crosses the public-log boundary.

## Running it locally

The gate needs a copy of the private authority, so it is a maintainer
operation:

```bash
# TRUSTED_DIR must be outside this repository.
git show private/main:docs/provenance/IMPLEMENTATION_PROVENANCE.json > "$TRUSTED_DIR/ledger.json"
python tools/provenance_attest_verify.py --repo . --candidate HEAD --base origin/main --trusted-ledger "$TRUSTED_DIR/ledger.json" --show-debt
```

Exit status: `0` pass, `1` fatal findings, `2` the verifier's own inputs are
unusable. All three are fail-closed.

CI additionally passes `--require-immutable-revisions` with the event's exact
head and base SHAs, and `--authority-revision <sha>` recording the commit the
authority was read at. A branch name is a moving target; resolving one twice
can name two different trees, so strict mode refuses anything but a 40-hex
commit SHA.

To approve a blob, add one entry to `reviewed_blobs` in the private authority.
The digest is the plain SHA-256 of the file's bytes — the same value the public
ledger already carries for that path, and the same one the gate computes from
the Git object:

```bash
sha256sum src/rt/example.c
```

`tools/test_provenance_attest_verify.py` needs no private input. Every case
builds a real Git repository and commits a *self-consistent* attack — well
formed entry, correct hash, policy updated — and asserts the finding code that
stops it.

## Maintainer configuration

Three steps are outside the repository and can only be done by a maintainer.
They are ordered, and the order matters.

**1. Trusted authority.** The private detailed ledger must actually contain a
record for every path this repository attests. Two are outstanding: the
`src/rt/guest_interp.*` record (see
[`docs/provenance/GUEST_INTERP_ATTESTATION.md`](provenance/GUEST_INTERP_ATTESTATION.md))
and an entry for `tools/provenance_attest_verify.py` on the record the public
ledger names `provenance-tooling`. Until both land, the gate reports
`RECORD_ABSENT` / `CLAIM_UNBACKED` and fails, by design.

**2. Repository secret.** `PROVENANCE_AUTHORITY_TOKEN`, a fine-grained personal
access token scoped to the private authority repository only, with
**Contents: Read-only** and no other permission. If the secret is missing the
gate fails closed rather than skipping. The token is only ever held by a job
that does not execute a single byte of candidate code.

**3. Required status check — DO NOT ADD IT YET.**

The gate ships in **observation mode**. It runs on every pull request, fails
closed, and publishes its findings, but it is deliberately **not** a required
status check and must not be described as enforcement.

The reason is unchanged and unweakened: GitHub matches required checks by
context *name*, both the trusted workflow and any candidate workflow run as
GitHub Actions (`integration_id` 15368), and a candidate job of the same name
that finishes later wins. `CI_CONTEXT_COLLISION` refuses the static case and
puts the attempt in the diff, but it cannot win a reporting race.

Making this a required check is gated on **one** of:

* proving the `workflows` ruleset rule is available on this repository (the
  probe below), which binds enforcement to a workflow file at a pinned ref
  rather than to a name; or
* implementing a distinct GitHub App check producer and pinning its
  `integration_id` in the ruleset.

Until then, adding `{"context": "Trusted provenance attestation",
"integration_id": 15368}` to the ruleset would create the *appearance* of
enforcement without the property. Do not do it.

When the day comes, the ruleset already carries
`strict_required_status_checks_policy: true`, a `pull_request` rule,
`required_linear_history`, and an empty `bypass_actors` list. Whatever context
is eventually required inherits all of that; do not relax any of it.

## Required-check identity: what GitHub can and cannot enforce here

**A context-name-only required check is not tamper-proof, and must not be
described as enforcement.**

GitHub evaluates a required status check by context *name*, taking the most
recent result for that name on the head commit. Both the trusted
`pull_request_target` workflow and any workflow the candidate adds run as the
same app — GitHub Actions, `integration_id` 15368 — so pinning `integration_id`
does not separate them. A candidate can declare a job named
`Trusted provenance attestation`, have it `exit 0`, and delay it (a `sleep` is
enough) so it reports last. `CI_CONTEXT_COLLISION` refuses any candidate
workflow containing the reserved string, which stops the static case and puts
the attempt in the diff where a human sees it, but it cannot win a reporting
race it does not control.

The three GitHub-native primitives that could bind enforcement to identity
rather than to a name were each investigated:

| Primitive | Would it close the spoof? | Available here? |
| --- | --- | --- |
| `required_status_checks.integration_id` | No — trusted and candidate checks are both Actions (15368) | yes, but useless for this |
| `workflows` ruleset rule (`repository_id` + `path` + `ref`/`sha`) | **Yes** — identity is the workflow file at a pinned ref, which a candidate cannot substitute; it supports `pull_request_target` | **Probably not.** GitHub documents ruleset workflows as configured "at the organization or enterprise level". This repository is owned by a **personal account**, so there is no organization ruleset scope. Unconfirmed — see the probe below |
| push ruleset `file_path_restriction` on `.github/workflows/**` | Partly — a candidate that cannot add a workflow cannot collide | **No.** "You can create a push ruleset for private or internal repositories." This repository is public |

**Conclusion: on this repository as currently configured, the spoof cannot be
fully closed with GitHub-native ruleset features.**

### Safe probe for the `workflows` rule

This can be settled without touching `main`. Create a *separate* ruleset that
targets a throwaway branch pattern; if the API accepts the rule, the feature
exists on this account.

```bash
gh api -X POST repos/Jstar269/nakagawa-recomp/rulesets --input - <<'JSON'
{ "name": "workflows-rule-probe", "target": "branch", "enforcement": "disabled",
  "conditions": { "ref_name": { "include": ["refs/heads/ruleset-probe/**"], "exclude": [] } },
  "rules": [ { "type": "workflows", "parameters": { "workflows": [
    { "repository_id": 1330183186, "path": ".github/workflows/provenance-attestation.yml",
      "ref": "refs/heads/main" } ] } } ] }
JSON
```

`enforcement: "disabled"` and a branch pattern nothing matches make this inert.
Delete it afterwards. If it is accepted, prefer the `workflows` rule over
everything below.

### Minimum secure design if it is not available

A dedicated **GitHub App as check producer**. It needs no server:

1. Register a GitHub App owned by the maintainer with **Checks: read & write**
   and no other permission; install it on this repository only.
2. Store its App ID and private key as repository secrets. They are read only
   by the trusted `pull_request_target` job — the job that never executes a
   byte of candidate code.
3. The trusted job mints an installation token and creates a check run named
   `provenance/trusted-attestation` **as the App**, with the verifier's verdict.
4. The ruleset requires that context pinned to the App's `integration_id`.

A candidate's Actions job can still create a check run with that name, but it
carries `integration_id` 15368, not the App's, so it does not satisfy the rule.
The candidate cannot obtain the App's identity because it cannot read the
secrets and cannot run code in the job that holds them.

**This design is specified, not built.** Nothing in this branch creates an App,
mints a token, or posts a check run.

### What may safely merge now, and what must not be claimed

| | Status |
| --- | --- |
| The verifier, its tests, the trusted workflow, this documentation | complete and safe to merge; they add a real control and remove none |
| The gate as a **signal** — it runs on every pull request, fails closed, and puts findings in the PR | complete once the secret exists |
| The gate as **tamper-proof merge enforcement** against a hostile author who can add workflow files | **NOT complete.** Requires the `workflows` rule or the GitHub App above |

Do not describe the required status check alone as enforcement. Until identity
binding exists, it is a strong control against error and against an author who
does not attack the check itself, and that is all.

## Other residual risk

**The authority is only as good as its contents.** This gate verifies that a
claim is backed by `private/main` and that the exact bytes were approved there;
it does not and cannot verify that the record or the approval is *true*. Both
remain human attestations.

**Approval is per-blob, not per-review-depth.** A `reviewed_blobs` entry says
the maintainer approved that digest. It cannot express how carefully. Approving
a large diff is one line of JSON, so the discipline of actually reading the diff
stays a human responsibility.

**Existing blobs are grandfathered by non-change.** Adopting the rule did not
require approving the 289 content-gated paths already on `main`; they keep
their path-level authorization until their bytes change. That is deliberate —
the alternative was an unusable day-one gate — but it means the current tree's
content has not been digest-approved, only its future changes will be.
