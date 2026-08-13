# Project status dashboard

**Repository status: active sanitized public source.**
`Jstar269/nakagawa-recomp` is the public source repository. Its public history
deliberately begins with the sanitized restoration lineage; the former
development history is not ordinary `main` ancestry and must not be reconnected.
This status is not legal clearance, a release announcement, or a PSP-correctness
claim.

## At a glance

| Priority | State | Public work item |
| --- | --- | --- |
| P0 | Ongoing | Public-source safeguards: exact-tree, provenance, history, security, and export gates remain required for proposed changes and release candidates. |
| P1 | Open | PSP DMA copy semantics: [issue #23](https://github.com/Jstar269/nakagawa-recomp/issues/23) remains an implementation lane. |
| P1 | Open | Portable Allegrex/VFPU float-to-word semantics are tracked in [issue #38](https://github.com/Jstar269/nakagawa-recomp/issues/38). |
| P1 | Open | Unified PSP clocks, waits, and interrupt delivery remain source-owned runtime work. |
| P1 | Open | Direct XB archive/VFS tooling is documented in [`docs/ISSUE196_DIRECT_XB.md`](docs/ISSUE196_DIRECT_XB.md); title inputs remain local-only. |
| P1 | Open | Plain PSP Mutex physical hardware oracle specification and probe suite are documented in [`docs/MUTEX_HARDWARE_ORACLE.md`](docs/MUTEX_HARDWARE_ORACLE.md) ([issue #2](https://github.com/Jstar269/nakagawa-recomp/issues/2)). |
| P1 | Open | The versioned title-manifest/toolkit boundary is described in [`assets/titles/README.md`](assets/titles/README.md); only generic and synthetic manifests are public-scope. Analyzer executable spans and manifest-derived build settings now have one explicit owner ([`docs/TITLE_CODEGEN_PLAN.md`](docs/TITLE_CODEGEN_PLAN.md)). |
| P1 | Blocked | PGF/font and PGD/amctrl surfaces remain excluded pending qualified provenance and distribution review. |

## Evidence boundary

The active public repository contains source-owned runtime/tooling, synthetic
fixtures, and explicit provenance controls. It excludes decrypted modules, generated
retail translation, title assets, saves, captures, private routes, private
oracles, keys, unresolved PGF/PGD implementations, the extended ISO/audio
backends, and historical private work products. A public-scope build therefore
proves only that the fail-closed generic boundary compiles; it does not prove
that a private title boots or plays.

Hosted GitHub Actions is active and required machine checks exist. Public-safe
CI and local tests are evidence only for the paths they execute; they are not a
complete private-title gameplay route, PSP hardware acceptance, visual
acceptance, DCO attestation, or legal approval. Missing private inputs and
external oracles remain unavailable rather than passing.

## Publication controls

The machine-readable policy in [`assets/public_source_profile.json`](assets/public_source_profile.json)
is authoritative. [`PUBLIC_EXPORT.json`](PUBLIC_EXPORT.json), the provenance
ledger, release manifest, SBOMs, and `tools/publish_audit.py` must describe the
same exact bytes. A candidate or export cannot select a relaxed private audit
mode or self-authorize provenance.

Do not treat this dashboard as a release announcement. Update it only when the
public-source boundary or tracked milestone state changes, and keep private
investigation narratives outside the public source tree.
