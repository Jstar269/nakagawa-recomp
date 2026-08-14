# Documentation

This index covers the active sanitized public source repository and its
public-safe candidate/export boundary. Source code, tests and the
Makefile remain authoritative for implementation behavior; curated GitHub Issues
are authoritative for actionable work. Private operational, title-run, legal
review and historical-archive documents are intentionally outside the public
source tree.

## Start here

| Need | Read |
| --- | --- |
| Project scope and setup | [`README.md`](../README.md), [`SETUP.md`](SETUP.md) |
| Architecture and build model | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| Current issue map | [`ISSUES.md`](../ISSUES.md) |
| Public publication gates | [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md), [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) |
| Provenance and notices | [`NOTICE.md`](../NOTICE.md), [`../assets/public_provenance_ledger.json`](../assets/public_provenance_ledger.json), [`provenance/INDEPENDENCE_MODEL.md`](provenance/INDEPENDENCE_MODEL.md) |
| HST/public-boundary census | [`provenance/HST_PUBLIC_CENSUS.md`](provenance/HST_PUBLIC_CENSUS.md) |
| Security and contribution policy | [`SECURITY.md`](../SECURITY.md), [`CONTRIBUTING.md`](../CONTRIBUTING.md), [`DCO_POLICY.md`](DCO_POLICY.md) |

## Maintained engineering guides

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — runtime, code generation, renderer and
  two-phase build structure.
- [`SETUP.md`](SETUP.md) — supported toolchain and documented external inputs.
- [`CI.md`](CI.md) — path-gated hosted checks and their evidence limits.
- [`DEBUGGING.md`](DEBUGGING.md) — diagnostics and safe local troubleshooting.
- [`PORTING.md`](PORTING.md) — generic title-manifest/code-generation boundaries.
- [`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md) — portability plan.
- [`STATIC_VERIFY.md`](STATIC_VERIFY.md) — oracle-free verification and blocked
  external-input gates.
- [`HARDWARE_ORACLE.md`](HARDWARE_ORACLE.md) — bounded proposal and limits;
  source-owned probes are not hardware acceptance without measured provenance.
- [`AI_USAGE.md`](AI_USAGE.md) — factual AI-assistance and review boundaries.

## Provenance and publication

- [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) defines the exact-tree,
  policy, provenance, history, SBOM, build, documentation and governance gates.
- [`PUBLIC_SOURCE_PROFILE.md`](PUBLIC_SOURCE_PROFILE.md) explains the explicit
  include/exclude policy and fail-closed candidate construction.
- [`../assets/public_provenance_ledger.json`](../assets/public_provenance_ledger.json)
  is the path-hashed public provenance ledger; unresolved records are not clearance.
- [`provenance/HST_PUBLIC_CENSUS.md`](provenance/HST_PUBLIC_CENSUS.md) classifies
  title-specific versus generic/synthetic surfaces.
- [`provenance/MODIFIED_FILE_NOTICES.md`](provenance/MODIFIED_FILE_NOTICES.md)
  describes the retained upstream notice contract.

The active public repository and any publication candidate must not include
private inputs, game-derived bytes, captures, saves, keys, oracle material,
decrypted modules, generated retail code, private repository metadata, or
counsel/incident work product. Unknown paths fail closed in the machine policy.
