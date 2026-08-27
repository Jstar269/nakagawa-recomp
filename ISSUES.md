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
| P0 | Open | Trusted refresh path for modified source hashes in the public provenance ledger: [issue #132](https://github.com/Jstar269/nakagawa-recomp/issues/132). A content hash may be refreshed in place for a path whose classification and record are unchanged; establishing or re-authenticating an attestation needs the maintainer-controlled trusted evidence this issue tracks. |
| P1 | Open | PSP DMA copy semantics: [issue #23](https://github.com/Jstar269/nakagawa-recomp/issues/23) remains an implementation lane. |
| P1 | Open | Exotic VFPU NaN/Inf matrix and multiply bit-pattern divergences: [issue #40](https://github.com/Jstar269/nakagawa-recomp/issues/40) — matrix/multiply NaN/Inf cells measured and settled on PSP-3000 (ARK-5) silicon (sNaN quieting 0x7FC00001, order independence, default invalid NaN 0x7FC00000, FTZ subnormal flushing); broader overlap/vhdp follow-up remains open. |
| P1 | Open | Unified PSP clocks, waits, and interrupt delivery remain source-owned runtime work. |
| P1 | Open | Direct XB archive/VFS tooling is documented in [`docs/ISSUE196_DIRECT_XB.md`](docs/ISSUE196_DIRECT_XB.md); title inputs remain local-only. |
| P1 | Open | The versioned title-manifest/toolkit boundary is described in [`assets/titles/README.md`](assets/titles/README.md); only generic and synthetic manifests are public-scope. Analyzer executable spans and manifest-derived build settings now have one explicit owner ([`docs/TITLE_CODEGEN_PLAN.md`](docs/TITLE_CODEGEN_PLAN.md)). |
| P1 | Blocked | PGF/font and PGD/amctrl surfaces remain excluded pending qualified provenance and distribution review. |
| P1 | Closed | Acceptance-route determinism: [issue #64](https://github.com/Jstar269/nakagawa-recomp/issues/64). Routes now assert the screen they reach (`WAIT`/`EXPECT` in `SR_PADSCRIPT`, see [`docs/DEBUGGING.md`](docs/DEBUGGING.md)); a run that reaches a different state fails loudly instead of completing. |
| P1 | Open | Compatibility-override surface in generic core: [issue #98](https://github.com/Jstar269/nakagawa-recomp/issues/98). 46 title guest addresses across 59 sites in `src/rt/hle.c`, 16 of them behaviour-altering; the machine-enforced inventory is `tools/compat_overrides.py` and the readiness record is [`docs/PORTING.md`](docs/PORTING.md). |
| P1 | Closed | Executing valid executable AOT misses instead of fabricating success: [issue #116](https://github.com/Jstar269/nakagawa-recomp/issues/116). The production fail-closed interpreter floor and its source-owned cosimulation gate landed. Closing it records that a valid executable miss now executes; it does not claim coverage beyond the implemented interpreter subset. Read [Execution-contract state](#execution-contract-state) before citing this. |
| P1 | Closed | Proving the full production pipeline with a source-owned PSP guest: [issue #110](https://github.com/Jstar269/nakagawa-recomp/issues/110). `production-smoke` and `production-smoke-gap` build a deterministic PSP-shaped fixture through the real two-phase pipeline ([`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)). That gate catches generic pipeline and composition failures without an ISO; it establishes neither commercial-title compatibility nor PSP timing, rendering, or audio correctness. |
| P1 | Open | Guest virtual/vblank rate drift against host real time: [issue #70](https://github.com/Jstar269/nakagawa-recomp/issues/70). Route sampling now keys on elapsed VCOUNT cadence rather than wall time, which removes a measurement artefact; the underlying rate question is unchanged. |
| P1 | Open | Transient model corruption entering the main menu: [issue #69](https://github.com/Jstar269/nakagawa-recomp/issues/69). No public-scope reproduction exists; the public repository holds no private-title route evidence. |
| P1 | Open | Audio stuttering during qualified gameplay: [issue #67](https://github.com/Jstar269/nakagawa-recomp/issues/67). No public-scope reproduction exists. |
| P1 | Open | Post-save prize-ceremony progression: [issue #63](https://github.com/Jstar269/nakagawa-recomp/issues/63). Private-title route behaviour; outside the public-safe acceptance boundary and not reproducible from public inputs. |

## Execution-contract state

This section exists because the AOT/interpreter boundary is the claim most easily
overstated. State it exactly this way:

- The production interpreter is a **fail-closed AOT-gap correctness floor**. Only
  analyzer-owned executable spans carry executable authority; mapped guest RAM and AOT
  registration do not independently grant it.
- Source-owned cosimulation later exposed a missing returning-call continuation contract in
  that floor. It was found by the gate rather than by inspection, and it was repaired by
  giving linked calls an explicit `resume_pc` through `dispatch_call()` instead of inferring
  the resume boundary from a live `$ra` the callee is allowed to rewrite.
- Current evidence covers **both CALL and TAIL** tier crossings, for the **implemented
  interpreter instruction subset only**. That subset is enumerated in
  `src/rt/guest_interp.h` and is checked in both directions against the cells that execute
  it ([`fixtures/cosim/README.md`](fixtures/cosim/README.md)).
- Unsupported forms still **fail closed** rather than being decoded as some unrelated
  arithmetic form. Nested interpreted calls are outside the current floor: the interpreter
  carries one call boundary, not a stack of them.
- The interpreter is **not an all-Allegrex interpreter**, and AOT coverage remains a
  correctness requirement outside the supported floor.

Do not restate this as "arbitrary lawful AOT to interpreter and back is proven". It is not.

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
