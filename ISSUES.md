# Project status dashboard

**Candidate status: not republished.** This checkout is a locally constructed
public-safe candidate. It is not itself a public-repository announcement,
legal clearance, PSP-correctness claim, or hosted-CI result.

## At a glance

| Priority | State | Public work item |
| --- | --- | --- |
| P0 | Open | Publication candidate: exact-tree, provenance, history, and security gates must all pass before any cutover. |
| P1 | Open | PSP DMA copy semantics: [issue #23](https://github.com/Jstar269/nakagawa-recomp/issues/23) remains an implementation lane. |
| P1 | Open | Unified PSP clocks, waits, and interrupt delivery remain source-owned runtime work. |
| P1 | Open | Direct XB archive/VFS tooling is documented in [`docs/ISSUE196_DIRECT_XB.md`](docs/ISSUE196_DIRECT_XB.md); title inputs remain local-only. |
| P1 | Open | The versioned title-manifest/toolkit boundary is described in [`assets/titles/README.md`](assets/titles/README.md); only generic and synthetic manifests are public-scope. |
| P1 | Blocked | PGF/font and PGD/amctrl surfaces remain excluded pending qualified provenance and distribution review. |

## Evidence boundary

The public candidate contains source-owned runtime/tooling, synthetic fixtures,
and explicit provenance controls. It excludes decrypted modules, generated
retail translation, title assets, saves, captures, private routes, private
oracles, keys, unresolved PGF/PGD implementations, the extended ISO/audio
backends, and historical private work products. A public-scope build therefore
proves only that the fail-closed generic boundary compiles; it does not prove
that a private title boots or plays.

Local tests and selftests are evidence for the paths they execute. They are not
hosted CI, PSP hardware acceptance, visual acceptance, DCO attestation, or legal
approval. Missing private inputs and external oracles remain unavailable rather
than passing.

## Publication controls

The machine-readable policy in [`assets/public_source_profile.json`](assets/public_source_profile.json)
is authoritative. [`PUBLIC_EXPORT.json`](PUBLIC_EXPORT.json), the provenance
ledger, release manifest, SBOMs, and `tools/publish_audit.py` must describe the
same exact bytes. The candidate cannot select a relaxed private audit mode or
self-authorize provenance.

Do not treat this dashboard as a release announcement. Update it only when the
candidate disposition changes and keep private investigation narratives outside
the public source tree.
