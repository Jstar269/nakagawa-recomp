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

**Public source surface:** general source, tests, synthetic fixtures, interoperability facts, and
provenance records that do not contain or reconstruct copyrighted game expression.

Product 1 is not a game download, emulator distribution, or substitute source release of the game.
The active public repository and any future release must not contain the game or
a mechanically translated distribution of its implementation.

## Product 2 — Hot Shots Tennis decompilation/reconstruction

**Purpose:** a complete (100%) decompilation of the title: source code for every function that
compiles back to machine code identical to the retail executable, with each function marked
matched only after a byte-for-byte comparison (see [DECOMPME_INTEGRATION.md](DECOMPME_INTEGRATION.md)).
It is evidence-driven and exists to understand and maintain the title. It lives in its own
repository, [Jstar269/hst-decomp](https://github.com/Jstar269/hst-decomp), public since 2026-10-17 by maintainer
decision: that repository's publish filter keeps out Sony SDK reconstructions, game data, keys and compilers, and
its matching sources carry their own licensing there. Its sources are never copied or translated into this
repository; only the interoperability facts and independently implemented fixes described below cross over.

Product 2 may consume private analysis aids such as function maps, decompiler output, local symbols,
and comparison notes, but those materials are not automatically suitable for the Product 1 public
source tree. Facts needed for interoperability—addresses, ABI behavior, structure layouts, NIDs, and
observed state transitions—should be recorded narrowly. Expressive decompiler output or
mechanically translated game implementation must remain outside the public source tree unless
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

## Direction

Nakagawa Recomp's long-term goal is to evolve into a definitive, multi-title PSP recompilation and preservation platform built from original code wherever possible.

While initial development began by adapting upstream open-source toolkits, the architectural direction systematically replaces convenience shortcuts and high-level approximations with maximal sensible low-level emulation (LLE), precise hardware-measured semantics, and original implementations. Core initiatives include:

- Driving title-specific HLE overrides toward zero in generic runtime code
- Prioritizing guest execution fidelity over host reimplementations
- Expanding data-driven multi-title manifest planning
- Delivering and hardening the standalone native player interface

See [`LLE_FIDELITY_ARCHITECTURE.md`](LLE_FIDELITY_ARCHITECTURE.md) for the fidelity doctrine and [`HLE_AND_WORKAROUND_INVENTORY.md`](HLE_AND_WORKAROUND_INVENTORY.md) for the HLE budget that drives title-specific overrides to zero.

## Roadmap (next 12 months)

The project intends to:

- publish the first Windows release, v0.0.1, with signed build provenance
  ([#278](https://github.com/Jstar269/nakagawa-recomp/issues/278));
- broaden title support through generic fixes rather than per-title patches
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)). PAC-MAN Championship Edition already reaches
  its menu and plays a stage with no title-specific branch; the next milestone takes one such title through a
  documented gameplay scenario that saves, closes and resumes in the native player, with no title-specific branch
  or HLE override ([#1126](https://github.com/Jstar269/nakagawa-recomp/issues/1126));
- extend the [title qualification route](TITLE_TESTING.md) past boot, so menus, gameplay, saving, loading, movies,
  rendering and recovery are each reported, and a stage that was not proven reads `NOT_RUN`, `BLOCKED` or `SKIP`.
  Retail titles stay local-only: a run on a user's own game is recorded outside the public tree, and only a
  public-safe summary is published;
- deliver one-click play from a PSP ISO the user owns, without the developer toolchain
  ([#308](https://github.com/Jstar269/nakagawa-recomp/issues/308)), within the lawful-input and fail-closed rules;
- make the native player sharp at any window size, including fullscreen: draw the player UI at the window's real
  pixel size, and present game frames with integer or sharp-bilinear scaling instead of plain bilinear filtering
  at fractional sizes, keeping native 480×272 rendering as the verified default
  ([#1129](https://github.com/Jstar269/nakagawa-recomp/issues/1129));
- move more of the runtime from high-level emulation to low-level emulation, measured against a physical PSP;
- bring the player to Linux, macOS and Android ([#306](https://github.com/Jstar269/nakagawa-recomp/issues/306),
  [#329](https://github.com/Jstar269/nakagawa-recomp/issues/329),
  [#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).

It does not intend to bundle games, firmware, keys or fonts, to download game data, or to help users obtain
games they do not own. Live priorities are the open [GitHub issues](https://github.com/Jstar269/nakagawa-recomp/issues).

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
| `private-analysis` | local-only analysis material that must not enter the public source repository |
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

## What the project ships and does not ship

The repository ships source code, documentation and the notices listed in
[`NOTICE.md`](../NOTICE.md). It does not ship game executables, game assets,
firmware files, decryption keys or private traces. The user supplies the game
files and any key file that the decryption boundary reads. This section is not
legal advice. Check the rules where you live.

The project's operational rules are:

- do not ship or solicit game content;
- do not ship keys or secret values;
- do not market the project as authorized, official, or endorsed;
- use game/platform names only to identify compatibility;
- do not state that a root open-source license grants rights to third-party content;
- require qualified review before public distribution of uncertain PGF, PGD/amctrl, generated, or
  history-derived material;
- provide a private rights-holder reporting path and act promptly on substantiated concerns.

See `NOTICE.md`, `SECURITY.md`, `docs/PUBLIC_SOURCE_PROFILE.md`, and
`docs/PUBLICATION_READINESS.md` for the maintained operational details.

## Repository structure

The active topology is:

- `Jstar269/nakagawa-recomp` is the active sanitized public source repository;
- its public history deliberately begins with the sanitized restoration lineage,
  without the former development history as ordinary ancestry;
- private Product 2 material and private evidence stay outside the public source
  history;
- any binary release is built from a tagged public-source candidate plus user-supplied private
  inputs, with its exact SBOM/notices/corresponding source archived alongside it.

Do not reconnect private/pre-sanitization ancestry or otherwise change this
topology without a documented threat model, provenance review, migration plan,
and explicit maintainer authorization.
