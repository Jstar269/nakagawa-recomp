# Long-range roadmap: three products, one semantic core

**Status: strategic direction, not a task list.** The live task list is
[`ISSUES.md`](../ISSUES.md) (current milestone) and the canonical GitHub issues.
This document records *where the project is going* and *why*, so that day-to-day
decisions can favor designs that generalize. It does not authorize new work on
its own; nothing here overrides the current milestone.

> [!IMPORTANT]
> **The only active project today is HST recompilation.** Everything below Phase 1
> is forward-looking. Do not start decompiler or general-recompiler work in place
> of the current milestone. #29 (the `drive_court` Exhibition-return route) closed
> 2026-07-25 as not reproducible; the live milestone is now
> [#33](https://github.com/Jstar269/nakagawa-recomp/issues/33) performance, which is
> unblocked and **partially measured** (renderer lead and exact-shadow applicability
> established), alongside the rendering investigations
> [#142](https://github.com/Jstar269/nakagawa-recomp/issues/142) and
> [#143](https://github.com/Jstar269/nakagawa-recomp/issues/143), whose implementation fixes are merged
> and whose issues were closed on 2026-08-04. The value of this roadmap is that it
> changes *how* current work is designed, not *what* is worked on next.

## The three products

The project is deliberately scoped as three separate products, in order. Only the
first is being built.

1. **HST recompilation (current, and the only active work).** Statically translate
   *Hot Shots Tennis: Get a Grip* to C and run it faithfully. Boots to title, menus,
   the 3D lobby, and a live match, and returns from a match to the club cleanly
   (#29, closed 2026-07-25 on two isolated replays). The former #143 formatter defect and #142
   display-latch defect are fixed on `main`; both issues were closed on 2026-08-04 after the recorded
   exact-main evidence.

2. **HST decompilation (future).** Recover maintainable, human-understandable
   source and data structures from what the recompiler has learned about the game.
3. **A general PSP recompiler/decompiler toolkit (future).** Accept other PSP
   executables without adding title-specific code to the generic pipeline.

These share one asset: an accurate, machine-faithful model of PSP program behavior.
Every correctness improvement made for HST today — thread, callback, GE, VFPU,
module-loading, I/O, and interrupt semantics — becomes the behavioral specification
that later validates reconstructed source. Current emulator work is not throwaway.

## Three thresholds, kept distinct

Do not collapse these into one "is it done" question. They are different bars with
different gates, and reaching one does not imply the others. Publication gating is
detailed in [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md).

| Threshold | Question | Gate |
| --- | --- | --- |
| **Generic source is publishable** | Is the *tooling + runtime* legal and clean to make public? | Legal/provenance + history-scrub, **not** PSP correctness. An experimental project can be public with documented bugs. |
| **Runtime is PSP-faithful** | Does the runtime reproduce PSP behavior? | The correctness campaign below; measured against oracles, not route progress. |
| **A game-specific binary is distributable** | Can a built executable containing game-derived code/data be shipped? | A higher, separate legal bar; recompiled game chunks and game-derived images are **not** initially publishable. |

## Repository topology

The three products are three *scopes and legal postures*, not three repositories.
Splitting them physically too early costs more than it buys (duplicated
runtime/IR/analysis, cross-repo atomic changes, 3x CI on billing-capped Actions,
and splitting code whose tier boundaries are not yet clean). The durable shape is
**two tracks**:

| Track | Repo | Visibility | Holds |
| --- | --- | --- | --- |
| **Toolkit** (Products 1 → 3) | this repo | public-track | generic pipeline, runtime Tier 0/1, SRIR + analysis DB + decompiler backend, reference interpreter, synthetic/homebrew corpus, analysis tooling (e.g. `tools/decompme_export.py`). HST is a **Tier-2 profile** (config), not a repo. |
| **Game data** (Product 2) | a new *private* repo | never public without a separate legal decision | the HST project profile, recovered symbols/annotations/types, decompiled source, private oracle traces, decomp.me matches. |

Product 3 (the general recompiler) is **not a fork** — it is this toolkit repo
*reframed* once HST is fully a Tier-2 profile. The interface between the two tracks
is **symbols/annotations** (the N64Recomp ↔ Zelda64RecompSyms pattern; see the
symbol-based patch/annotation direction below).

**When to actually split** (do none of it preemptively):

- **Now:** one repo. The Tier-2 quarantine (project manifest) is on the critical
  path anyway and makes any later split nearly free. Get modularity from
  **packages inside this repo**, not from repo boundaries.
- **When decompilation starts producing game-derived source:** create the private
  game-data repo — Product 2 output is copyrightable game expression (see
  [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md)).
- **When a second PSP title exercises the generic core (Generality L2):** extract
  the toolkit as its own package/repo — you finally have two consumers proving the
  boundary is right. Extracting a library before it has 2+ consumers just churns
  the API.

## Guiding principle: title-independence at the boundaries

The single most important architectural direction is to make today's recompiler
**title-independent at its edges without changing its behavior**, and to stop
letting "analysis" and "C generation" be the same operation.

### The tier model — every change should name its tier

| Tier | Owns | Title-specific? |
| --- | --- | --- |
| **Tier 0 — architecture** | Allegrex decode, PSP memory model, ELF/PRX formats, the semantic IR | Never |
| **Tier 1 — PSP platform** | kernel/HLE, GE, audio, display, filesystem, modules — PSP *semantics*, kept separate from the host/OS backends (SDL3, Vulkan, Media Foundation) that implement them | Never |
| **Tier 2 — title profile** | HST module list, private-input paths, load addresses, verified executable spans, compatibility patches, route configuration | **All** title-specific material lives here |

If a proposed change cannot answer "which tier owns this?", the design is probably
wrong. This is the generalization of the existing "no band-aids" rule in
[`AGENTS.md`](../AGENTS.md) and the compatibility-debt discipline of
[#20](https://github.com/Jstar269/nakagawa-recomp/issues/20).

### Current HST coupling to migrate (the concrete Tier-2 candidates)

These already exist and are already inventoried/tested — they are the first things
a project-manifest/Tier-2-profile mechanism should absorb, not new debt to create:

- `tools/analyze.py` — `DEFAULT_HST_EXTRA_SPANS = "0x00303194,0x00306e24"` (an HST
  executable range outside the normal section table). Already env-overridable via
  `HST_EXTRA_SPANS` and guarded as HST-specific.
- `tools/codegen.py` / `tools/host_stubs.py` — `HST_SIMPLE_STUBS` and
  `NULL_BASE_WORD_LOADS`. Already inventoried in `tools/compat_overrides.py` and
  enforced by `tools/test_compat_manifest.py` under
  [#20](https://github.com/Jstar269/nakagawa-recomp/issues/20).
- `Makefile` / `hst_manager.ps1` — HST module bases, extra-PRX paths, and manager
  defaults (see [`PORTING.md`](PORTING.md)).

The target is a per-project **manifest** (module list, load policy, executable
spans, compatibility entries) that the generic pipeline reads, so Tier 0/1 code
contains no retail address or `HST_*` condition. This does not legitimize the
stubs — it *quarantines* them so the generic recompiler knows nothing about HST.

## Architecture invariants (adopt as code is moved, not retroactively)

1. **Title-independent core.** No Tier-0/Tier-1 code carries a retail address or a title condition.
2. **Every analysis conclusion has provenance.** Store *why* something is a function/pointer/table, with confidence — not just that it is.
3. **Original addresses are never lost.** Keep file offset, module-relative address, original PSP VA, relocated address, and flattened runtime address distinct; never treat them as interchangeable.
4. **Exact semantics before simplification.** The low-level IR encodes machine behavior faithfully before any pass makes it prettier.
5. **Patches are data.** Every title-specific alteration is identifiable, explained, and removable — no hidden `if pc == 0x...:` in generic code.
6. **Analysis is deterministic.** Same bytes + same project config + same tool version ⇒ identical functions, blocks, IR, and output.
7. **Annotations cannot silently change execution.** Human names/types are separate from execution semantics.
8. **Private title material stays external.** The generic pipeline may consume proprietary inputs locally; they never become fixtures or public generated artifacts.

## Target architecture: separate the semantic model from its consumers

Today's pipeline (see [`ARCHITECTURE.md`](ARCHITECTURE.md)) is
`ELF/PRX → prxload → analyze → codegen → C`. Analysis and C emission are fused, and
the flat guest image is the primary representation. That is ideal for *executing*
HST and must not be rewritten wholesale — but it discards information a decompiler
later needs (module identity, relocation provenance, symbolic origin, callable vs.
interior entry).

The long-range target keeps the working recompiler and grows a canonical
representation beneath it:

```text
PSP ELF/PRX/modules
      │
      ▼
 Program model            modules / segments / relocations / imports / exports
      │                   (a persistent ProgramImage; the flat image becomes ONE
      ▼                    materialized backend view, not the primary form)
 Allegrex decoder         bytes → DecodedInstruction (no CpuState, no C strings)
      │
      ▼
 Exact semantic IR  ── "SRIR" ──  faithful PSP machine behavior: delay slots,
      │                            32-bit wrap, HI/LO, FPU/VFPU prefixes, guest
      │                            addresses, syscall boundaries, memory-space id
      ├────────────┬───────────────┐
      ▼            ▼               ▼
 Recompiler    Analysis /      Interpreter /
 backend       SSA / CFG       verifier (independent oracle)
      │            │
      ▼            ▼
 literal C    high-level IR ──► Decompiler backends (semantic C / matching C)
```

Key moves, each a separate effort, each behavior-preserving:

- **A ProgramImage** that remembers module/segment/relocation/import/export
  structure; the flat image is produced by `materialize_flat_image()` for today's
  runtime rather than being the canonical form.
- **Module-relative identities** (`func:main:00015f98`, not the flat address) that
  survive rebasing, alternate module bases, and cross-version comparison.
- **A standalone Allegrex decoder** so `bytes → DecodedInstruction` is independent
  of code generation and CPU state.
- **SRIR**, a custom low-level IR that preserves PSP concepts ordinary compiler IR
  erases. Start with integer ALU + memory + control flow; add FPU, then VFPU
  (reusing the existing VFPU verification investment). LLVM is a *later optional
  backend*, never the definition of PSP semantics.

The recompiler C backend then becomes `SRIR → C`, intentionally producing ugly but
faithful C.

## Migration discipline (non-negotiable)

Because thousands of HST functions already execute far enough to boot and play, that
behavior *is evidence*. Every structural migration therefore obeys:

- **Old and new paths coexist until outputs are provably equivalent** (behind a
  temporary flag); only then is the old path removed.
- **No structural PR also changes guest-visible behavior, fixes unrelated bugs, or
  optimizes.** "Move code into IR," "fix instruction semantics," and "optimize
  generated C" are three separate PRs.
- **Equivalence is proven three ways:** random-state instruction differential tests
  (recompiled C vs SRIR interpreter vs the C++ reference interpreter in `src/ref/`),
  bounded per-function differential tests, and the private HST route/dispatch/
  framebuffer parity check. The independent reference interpreter
  ([#36](https://github.com/Jstar269/nakagawa-recomp/issues/36)) is preserved, not
  replaced by SRIR execution.
- **`CpuState` stays frozen during the IR migration.** SRIR initially lowers to
  exactly today's `r[32]`/`hi`/`lo`/`f[]`/`v[128]`/`vfpuCtrl[16]` layout. Changing
  the IR and the CPU ABI at once would destroy regression isolation. `CpuState` is a
  shared ABI (`src/rt/recomp.h`, `src/ref/cpu.h`) — change both together, never mid-migration.

## Correctness foundation (the campaign that makes the runtime PSP-faithful)

Most open HLE issues are symptoms of missing shared abstractions, not isolated
stubs. Semaphores, mutexes, event flags, message pipes, and volatile memory all
need the same primitive: a typed UID with lifecycle state, owner, ordered wait
queue, timeout/deadline, callback-aware interruption, delete/cancel wake reason, and
validated error mapping. Build that once, then the individual issues collapse.

Recommended dependency order (each item maps to its canonical GitHub issue; the full
backlog and per-issue acceptance criteria live there, not here):

```text
memory/boundary safety (#15)
      → executable production-HLE test harness (#76)
      → typed kernel objects + wait transactions
      → thread lifecycle/scheduler (#92, #26, #61)
      → callbacks + synchronization (#1, #2, #13, #93, #74, #79, #88, #64)
      → I/O + savedata + utility (#72, #19, #55, #14, #68, #63, #90, #91)
      → multimedia (#69 → #38 → #70/#75 → #32 → #31)
      → GE/display correctness (#44 → #83/#89 → #23/#24 → #57 → #29)
      → deterministic route (#29)
      → performance (#33; #29 gate satisfied; accepted renderer measurements are recorded in GitHub issue #33 and docs/PERFORMANCE.md)
```

Two architecture-correctness items sit apart from game compatibility and should be
addressed before extensive optimization: separating table occupancy from guest
address zero ([#45](https://github.com/Jstar269/nakagawa-recomp/issues/45)) and
splitting callable entry points from interior/continuation PCs
([#51](https://github.com/Jstar269/nakagawa-recomp/issues/51)). Both are also
prerequisites for a trustworthy function model in the decompiler.

## Decompiler direction (Product 2)

Not started; recorded so current analysis work stays compatible with it.

- **Evidence-based function model.** Replace "function 0x1234 exists" with a
  `FunctionCandidate` carrying entry evidence (export, direct call, prologue,
  function pointer, symbol, annotation) vs. non-function evidence (jump-table
  target, interior branch, shared epilogue, import stub) and a confidence level.
  Never discard the evidence. Directly extends [#51](https://github.com/Jstar269/nakagawa-recomp/issues/51).
- **First-class CFG**, then **SSA layered *above* exact SRIR** (never as the
  execution form) for constant/copy propagation, value ranges, and variable
  reconstruction — so decompiler simplifications can never change execution.
- **PSP ABI, `$gp`, and stack-frame recovery** — infer argument/return/saved
  registers per function rather than assuming the standard ABI; treat `$gp` as a
  first-class global-recovery clue, not a generic constant; reconstruct stack
  frames and name locals/args by offset.
- **Data & type recovery** — typed data objects (globals, strings, pointer/jump/
  vtable tables), structures inferred from field-usage across callers, and a
  monotonic type lattice that refines (`unknown → pointer → Player *`) and records
  conflicts rather than oscillating. Bootstrap types from HLE/import prototypes.
- **Semantic vs. matching decompilation are different products** — a readable
  backend (behavior-equivalent) and a matching backend (rebuilds the original
  binary with the original toolchain) should not be forced through one path. Start
  recording compiler fingerprints (`.comment`, idioms, layout) now.
- **PSP API database & library fingerprinting** — a versioned NID→prototype/type
  database as the single source of API truth (feeds HLE dispatch *and* decompiler
  typing, eliminating duplicated/incorrect NID tables), plus fingerprinting of
  legally redistributable PSPSDK/newlib/homebrew code so known library functions are
  recognized instead of re-analyzed.
- **Ghidra as an independent cross-check, not a dependency** — export/import
  symbols, boundaries, and types both ways (see [`GHIDRA.md`](GHIDRA.md)); an
  Allegrex SLEIGH comparison is a second independent analysis, never a required
  runtime component.
- **Persistent, versioned analysis database** — modules, functions, blocks, CFG,
  data objects, symbols, types, and annotations stored across runs, with
  **generated facts kept strictly separate from human annotations** so a reanalysis
  never erases human work, and every artifact stamped with schema/tool/input
  versions. Once symbols are stable, patches and hooks target **symbols, not raw
  addresses**, so they survive rebasing and cross-version work.
- **`src/rt/hle.c` modularization proceeds only behind executable production-HLE
  coverage ([#76](https://github.com/Jstar269/nakagawa-recomp/issues/76)).** The first
  registered-NID specimen covers `sceKernelExitThread`; that is enough to begin narrowly
  extracting tested ThreadMan behavior, not to authorize a broad file move. Each subsystem
  needs its own production-boundary suite first. The callback extraction in
  [#116](https://github.com/Jstar269/nakagawa-recomp/issues/116) follows the same discipline.

## Measuring generality and decompilation (honestly)

"General" and "% decompiled" are meaningless undefined. Track them as explicit,
multi-dimensional ladders, and build a **public multi-compiler PSP homebrew corpus**
(known source ⇒ ground truth) to measure function-boundary precision/recall, CFG
correctness, call-target recovery, and decompiler readability *before* attempting a
second retail title.

- **Generality levels:** L0 HST only · L1 public homebrew corpus runs · L2 a second
  retail title needs only a manifest + HLE additions (zero analyzer/codegen changes)
  · L3 a third, subsystem-diverse title · L4 no title-specific behavior outside
  profiles · L5 arbitrary supported decrypted ELF/PRX in, with explicit unsupported
  diagnostics rather than silent corruption.
- **Decompilation milestones:** D0 machine map · D1 function map · D2 semantic
  (correct low-level structured C) · D3 symbols/types · D4 buildable · D5 behavioral
  equivalence · D6/D7 matching campaign.
- **Progress is reported per dimension** (bytes classified, boundaries confirmed,
  CFG resolved, functions semantically decompiled, named, typed, matching) — never a
  single dishonest percentage.

## What NOT to do

- **No big-bang rewrite** in another language; no simultaneous replacement of Make,
  Python, runtime, and analyzer.
- **No optimization before the IR architecture exists** — perf changes mask semantic
  migration failures. (Perf hypotheses live in [`TODO.md`](../TODO.md). #29 no longer
  blocks them, but a hypothesis is not a measurement — take the two #33 Benchmark
  baselines before acting on any of them.)
- **Do not auto-generalize HST patches into generic rules** — that is exactly how
  overfitting happens. Each stays a Tier-2 profile entry with evidence and a
  retirement criterion.
- **LLVM is not the source of truth** for PSP semantics; **Ghidra is not a required
  dependency**.
- **Publishing the generic tooling does not publish HST decompiled source** — that
  is a separate legal decision (see [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md)),
  because reconstructed game source can itself contain copyrightable expression.

## The next strategic milestone (when Product 1 permits)

Not the decompiler. The next architecture campaign is to make today's recompiler
title-independent at its boundaries **without changing behavior**: introduce the
project manifest, move every known HST-only address/path/module/stub into the
Tier-2 profile, add stable module-relative identities and the persistent
ProgramImage, keep the current flat-image/C pipeline on top of it — and only then
begin the decoder → SRIR migration. This remains direction, not work, until Product 1's
open performance work (#33) and the broader correctness, portability, and publication gates are
resolved.

## Related

- [`ISSUES.md`](../ISSUES.md) — current milestone and the canonical issue links (the live backlog).
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — the pipeline/runtime/ABI as it exists today.
- [`PORTING.md`](PORTING.md) — practical steps for one other title (the manual precursor to a manifest).
- [`PUBLICATION_READINESS.md`](PUBLICATION_READINESS.md) — the publication/legal thresholds and checklists.
- [`GHIDRA.md`](GHIDRA.md) — the independent cross-check tooling.
- [`TODO.md`](../TODO.md) — the performance hypothesis backlog (unblocked; #33 accepted renderer measurements are recorded in GitHub issue #33 and docs/PERFORMANCE.md).
