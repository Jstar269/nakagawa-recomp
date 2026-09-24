# `src/rt/guest_interp.c` / `.h` — attestation disposition

**Status: BACKED (resolved 2026-09-24, #332).** The trusted detailed
implementation authority contains the maintainer-authored record
`daybreak4-guest-interpreter` for both paths, and the trusted verifier resolves
the public claim to it. The sections below keep the original finding as history.

This document is deliberately limited to facts that are already public. The
detailed implementation ledger stays private, so its record bodies, evidence
text, and the private repository's branch and commit identifiers are not
reproduced here. The maintainer resolution path is handed over separately.

## What the public tree claims

`assets/public_provenance_ledger.json` on `origin/main` at
`421016b1faf3f6473bbd7c20d67be315aa4302d5` carries, for both paths:

```json
{
  "classification": "project_authored_attested",
  "evidence": {
    "source": "docs/provenance/IMPLEMENTATION_PROVENANCE.json",
    "record_id": "daybreak4-guest-interpreter",
    "evidence_tier": "S",
    "authorship": "independent implementation record",
    "upstream_attribution": "ppsspp"
  }
}
```

This is the strongest shape a public entry can take: a named record, an
explicit tier, a specific upstream attribution. It is exactly what a
machine-generated, fully-backed entry looks like.

## What the trusted authority holds

The trusted detailed implementation authority now contains the record
`daybreak4-guest-interpreter`. It names `src/rt/guest_interp.c` and
`src/rt/guest_interp.h` by exact path, classifies them as behavior-informed with
evidence tier `S`, and records its owner lane as issue #116. The record was
authored by the human maintainer and committed to the authority by the
maintainer, who confirmed on 2026-09-24 that it is their original record. No
agent authored, transcribed, or rewrote it.

With the record present, the trusted verifier
(`tools/provenance_attest_verify.py` against the external authority) no longer
reports `RECORD_ABSENT` for either path. The generated public ledger entries
resolve to that record through the same trusted pipeline as every other attested
path.

## History: the original finding

When this document was written, the authority checkout the public tree named as
its source did not contain the record, and no record in it named these paths.
The public artifact derived from the record merged into public `main` with
`7d404dc`, "runtime: add production AOT-gap interpreter floor (#118)", without
its authority. Nothing in the public repository could detect that, because
every public gate compared the candidate only to itself.

The disposition was that the maintainer, not tooling or an agent, would promote
the existing human-authored record into the trusted authority verbatim.
Downgrading the public entries was not available: `unresolved` is rejected by
`validate_ledger(require_resolved=True)` and by `publish_audit`, and no
deterministic class fits an implementation file. That maintainer action is what
resolved the finding.

## The record establishes path authority

The trusted record authorizes these implementation paths across ordinary
revisions. The verifier still binds every candidate to the trusted record,
checks the candidate content and export, and fails closed when path authority
is absent or unqualified. A routine byte change therefore does not require a
second private `reviewed_blobs` approval. That legacy exact-digest check remains
available only through `--require-reviewed-blobs` for high-assurance runs.

The public ledger carries the current content digests for reference:

```text
src/rt/guest_interp.c  1e40b7627b60e435e5fec8fc12200d5fe4c010ecad702c0095743db1dcd99019
src/rt/guest_interp.h  9ae21d305f8ad7e741843da58a476af41fd6270989f0832d5117b1b24596de2d
```

The one-time admission of a new implementation path still requires external
authority to bind the exact path and source lineage. Neither private authority
nor legal, title, or hardware acceptance can be inferred from automated tests.

## Why this was not "documentation drift"

The public tree asserted, to anyone reading it, that an identified provenance
record supported these two files. No such record existed in the authority the
public tree names as its source. The assertion was unverifiable by
construction, and no gate could have caught it, because every gate compared the
candidate to itself. That is the defect
[`docs/PROVENANCE_MERGE_GATE.md`](../PROVENANCE_MERGE_GATE.md) closes.
