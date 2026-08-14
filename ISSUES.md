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
| P1 | Open | Exotic VFPU NaN/Inf matrix/multiply divergences: [issue #40](https://github.com/Jstar269/nakagawa-recomp/issues/40) is reduced to two mechanisms — the NaN-payload operand order, fixed and silicon-settled (PSP-3000/ARK-5, 20 runs per vector: sNaN quieting 0x7FC00001, order independence, default invalid NaN 0x7FC00000, FTZ subnormal flushing), and the vhdp ±0/NaN accumulation fold, now aligned with the interpreter's fold, whose PSP result bits remain unresolved pending the overlap probe ([`fixtures/vfpu_nan_payload/`](fixtures/vfpu_nan_payload/), [`fixtures/vfpu_overlap_probe/`](fixtures/vfpu_overlap_probe/)). |
| P1 | Open | VFPU source/destination aliasing: the deterministic overlap corpus (`tools/vfpu_overlap_fuzz_gen.py` + `src/rt/vfpu_overlap_selftest.c`) proves codegen/interp read-before-write agreement over every legal alias class of vmmul/vtfm/vmscl/vmmov/vdot/vhdp/vcrs/vscl/vqmul/vcrsp; hardware-contract classes per pspdev/vfpu-docs are ALLOWED/NO_OVERLAP/UNESTABLED, and the UNESTABLED cells (source-source overlap, vmscl scalar-in-destination, vdot/vhdp/vcrs/vscl overlap) await the PSP-side probe in [`fixtures/vfpu_overlap_probe/`](fixtures/vfpu_overlap_probe/). |
| P1 | Open | Unified PSP clocks, waits, and interrupt delivery remain source-owned runtime work. |
| P1 | Open | Direct XB archive/VFS tooling is documented in [`docs/ISSUE196_DIRECT_XB.md`](docs/ISSUE196_DIRECT_XB.md); title inputs remain local-only. |
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
