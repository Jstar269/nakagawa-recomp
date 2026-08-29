# `src/rt/guest_interp.c` / `.h` — attestation disposition

**Status: UNBACKED on `main`.** The public claim is live; the trusted record it
names is not in the authority. This is a real finding, not documentation drift.

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

`private/main:docs/provenance/IMPLEMENTATION_PROVENANCE.json` does **not**
contain a record with the id `daybreak4-guest-interpreter`, and no record in it
names `src/rt/guest_interp.c` or `src/rt/guest_interp.h` by any path, exact or
wildcard. The nearest records the public ledger already names —
`cpu-core-runtime`, `reference-interpreter`, `vfpu-interpreter` — cover
different files and say nothing about these two.

The public artifact derived from the record merged into public `main` with
`7d404dc`, "runtime: add production AOT-gap interpreter floor (#118)". The
authority it derives from did not arrive with it. Nothing in the public
repository could detect that, which is the point: the entry is internally
perfect, and every public gate compared the candidate only to itself.

## Disposition

The record was authored by the human maintainer and exists outside
`private/main`. The resolution is to promote **that existing record, verbatim**,
into the trusted authority — not to author a replacement.

That is a maintainer action, deliberately not taken by tooling or by an agent.
The record carries a human attestation. An agent transcribing a human
attestation into the authority in order to turn its own gate green would be
performing exactly the bypass this gate exists to prevent: the gate must not be
made to pass by the class of action it forbids. The same applies to the record
that must gain `tools/provenance_attest_verify.py` — this change cannot
authorise its own new tool, and it does not.

Until the record is in `private/main`, `tools/provenance_attest_verify.py`
reports:

```text
FAIL  RECORD_ABSENT: src/rt/guest_interp.c: ledger names record_id
      'daybreak4-guest-interpreter', which does not exist in the trusted
      detailed implementation ledger; the attestation is unbacked
FAIL  RECORD_ABSENT: src/rt/guest_interp.h: ...
```

The alternative disposition — downgrading the public entries — is not
available. `unresolved` is rejected by `validate_ledger(require_resolved=True)`
and by `publish_audit`, and no deterministic class fits an implementation file.
Either the authority holds a record for these paths, or the paths are not
publishable.

## The record alone is no longer sufficient

Since exact-blob authorization landed (see
[`docs/PROVENANCE_MERGE_GATE.md`](../PROVENANCE_MERGE_GATE.md)), a record
authorizes a path and a `reviewed_blobs` approval authorizes the bytes. The
record's arrival will therefore clear `RECORD_ABSENT` and nothing more: these
two files keep their current authorization only while their bytes are
unchanged, and **the first change to either one will require the maintainer to
approve that exact digest**.

The maintainer's eventual approval should bind the exact blobs currently in the
public tree, whose digests the public ledger already carries:

```text
src/rt/guest_interp.c  1e40b7627b60e435e5fec8fc12200d5fe4c010ecad702c0095743db1dcd99019
src/rt/guest_interp.h  9ae21d305f8ad7e741843da58a476af41fd6270989f0832d5117b1b24596de2d
```

Approving those digests is a separate, deliberate human act from writing the
record. Neither has been performed by tooling or by an agent.

## Why this is not "documentation drift"

The public tree asserted, to anyone reading it, that an identified provenance
record supports these two files. No such record existed in the authority the
public tree names as its source. The assertion was unverifiable by
construction, and no gate could have caught it, because every gate compared the
candidate to itself. That is the defect
[`docs/PROVENANCE_MERGE_GATE.md`](../PROVENANCE_MERGE_GATE.md) closes.
