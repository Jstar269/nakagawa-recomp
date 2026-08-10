# Project Model and Public Boundaries

Nakagawa Recomp has three related but distinct engineering products. Keeping them separate prevents
HST-specific compatibility work, private reverse-engineering artifacts, and reusable PSP tooling
from becoming indistinguishable in code review or a future public release.

## Product 1 — Hot Shots Tennis recompilation

**Purpose:** translate and run the supported PSP release of *Hot Shots Tennis: Get a Grip* on a
Windows host through independently maintained code generation and runtime compatibility layers.

**Current repository owner:** the HST manager, HST build profile, title-specific module layout,
accepted input routes, and compatibility fixes.

**Private inputs:** retail ISO, decrypted executable/modules, extracted assets, saves/install data,
oracle traces, screenshots/frame dumps, and any locally supplied game-specific key material.

**Public candidate:** general source, tests, synthetic fixtures, interoperability facts, and
provenance records that do not contain or reconstruct copyrighted game expression.

Product 1 is not a game download, emulator distribution, or substitute source release of the game.
A future public repository must not contain the game or a mechanically translated distribution of
its implementation.

## Product 2 — Hot Shots Tennis decompilation/reconstruction

**Purpose:** support a private, evidence-driven source reconstruction effort for understanding and
maintaining the title.

Product 2 may consume private analysis aids such as function maps, decompiler output, local symbols,
and comparison notes, but those materials are not automatically suitable for the Product 1 public
source tree. Facts needed for interoperability—addresses, ABI behavior, structure layouts, NIDs, and
observed state transitions—should be recorded narrowly. Expressive decompiler output or
mechanically translated game implementation must remain outside the public source candidate unless
qualified review establishes a separate lawful distribution basis.

Product 2 should export reviewable interfaces rather than contaminate shared code with opaque
private-derived patches:

- function/address metadata with provenance;
- private decompilation context bundles;
- reproducible local comparison scripts;
- explicit handoffs from a private finding to an independently implemented compatibility change;
- tests demonstrating behavior without embedding retail bytes.

## Product 3 — General PSP recompiler/decompiler toolkit

**Purpose:** extract reusable PSP/Allegrex analysis, translation, runtime, and verification
capabilities from the HST bring-up.

Product 3 must not assume HST's zero base, entry, module addresses, filesystem layout, HLE frontier,
or accepted routes. Reusable pieces should be configuration- or manifest-driven and tested with
source-owned synthetic ELF/PRX/PBP fixtures. PSPSDK/PSPDEV can provide versioned ABI declarations,
build tools, and synthetic workloads, but they are not an exact firmware or hardware oracle.

Candidates for Product 3 include:

- bounded ELF/PRX/PBP parsing and relocation;
- Allegrex/MIPS analysis and C code generation;
- a versioned title manifest format;
- source-owned instruction, ABI, scheduler, GE, and import fixtures;
- a reusable native runtime/HLE boundary;
- differential trace formats and comparison tools;
- decomp.me context/export tooling;
- PSPDEV/PSPSDK declaration synchronization;
- optional PSPLINK or hardware-oracle collection tools that never become runtime requirements.

## Dependency direction

The desired direction is:

```text
General PSP toolkit (Product 3)
        ↑ reusable mechanisms and contracts
HST recompilation (Product 1)
        ↔ narrow private evidence interfaces
HST decompilation/reconstruction (Product 2)
```

Product 3 must not import private HST artifacts. Product 1 may configure Product 3. Product 2 may
produce private evidence used to design Product 1 fixes, but those fixes must be independently
expressed and verified.

## Required provenance label for changes

Every nontrivial change should be classifiable as one or more of:

| Label | Meaning |
| --- | --- |
| `general` | reusable PSP/toolchain behavior supported by public specifications or source-owned tests |
| `title-config` | HST values expressed as data/configuration rather than hard-coded reusable logic |
| `title-compat` | narrowly HST-specific runtime behavior with evidence and a retirement/generalization criterion |
| `private-analysis` | local-only analysis material that must not enter the public candidate |
| `upstream-derived` | adapted from an identified third party with exact revision and license provenance |
| `generated-private` | generated from retail/private inputs and prohibited from publication |

A change should not be described as general merely because it happens to work for HST.

## Public-source candidate rules

A public source candidate should include only material that has passed all applicable gates:

1. no retail executable, ISO, PRX, firmware, key, save, asset, trace, screenshot, frame dump, or
   game-derived hash manifest;
2. no raw decompiler database/output or mechanically translated proprietary implementation;
3. exact source and license lineage for every incorporated third-party component;
4. project-level GPLv3 metadata distinguished from inherited component/file provenance;
5. corresponding source, build scripts, notices, and dependency manifests tied to any distributed
   binary;
6. history/privacy review completed on the actual public repository history;
7. synthetic/public tests sufficient to exercise parsers and reusable mechanisms without private
   fixtures;
8. accurate non-affiliation, no-game-content, no-keys, warranty, and legal-jurisdiction disclaimers;
9. no claim of legal clearance, correctness, security, reproducibility, or compatibility beyond the
   evidence actually completed.

## Legal posture

The project's intended posture is interoperability research and independent compatibility
engineering by users who lawfully possess their own copy. In the United States, [17 U.S.C. § 1201(f)](https://www.copyright.gov/title17/92chap12.html)
contains a limited reverse-engineering interoperability exception, and
[17 U.S.C. § 117](https://www.law.cornell.edu/uscode/text/17/117) addresses certain owner-made
copies/adaptations. These provisions are fact-specific, do not erase other
copyright, anti-trafficking, contract, trademark, or jurisdictional rules, and are not a blanket
approval of this project or any distribution. Repository documents must remain descriptive and
conservative rather than presenting legal conclusions.

The recurring operational rules are therefore:

- do not ship or solicit game content;
- do not ship keys or secret values;
- do not market the project as authorized, official, or endorsed;
- use game/platform names only to identify compatibility;
- do not state that a root open-source license grants rights to third-party content;
- require qualified review before public distribution of uncertain PGF, PGD/amctrl, generated, or
  history-derived material;
- provide a private rights-holder reporting path and act promptly on substantiated concerns.

See `NOTICE.md`, `SECURITY.md`, `docs/KEY_HISTORY_SCRUB.md`, and
`docs/PUBLICATION_READINESS.md` for the maintained operational details.

## Decision rule for repository structure

Until the publication gates are complete, the safest topology remains:

- this historical development repository stays private;
- private Product 2 material stays outside the prospective public source history;
- a fresh sanitized public repository is created from an explicitly staged source candidate;
- any binary release is built from a tagged public-source candidate plus user-supplied private
  inputs, with its exact SBOM/notices/corresponding source archived alongside it.

This model should be revisited only with a documented threat model, provenance review, and migration
plan—not for convenience during an individual implementation task.
