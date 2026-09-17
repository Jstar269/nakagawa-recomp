# Decoupling the Generic Build Orchestrator (`nk_manager`)

This document outlines the architectural plan for decoupling Nakagawa Recomp's
primary build, test, and verification manager (`hst_manager.ps1`) from legacy
title-specific coupling (*Hot Shots Tennis: Get a Grip!*, UCUS98701) and transitioning
to the project's canonical orchestrator: `nk_manager.ps1`.

This work was tracked under [issue #196](https://github.com/Jstar269/nakagawa-recomp/issues/196)
as part of the broader Title-#2 readiness roadmap ([issue #98](https://github.com/Jstar269/nakagawa-recomp/issues/98)).
Both issues are now closed; see the linked live records.

---

## 1. Context & Motivation

At 1,445 lines (`wc -l`), `nk_manager.ps1` is the canonical orchestration layer for
developer workflows in Nakagawa Recomp; `hst_manager.ps1` is now a deprecated
forwarding wrapper (165 lines by `wc -l`). The manager provides **generic recompiler infrastructure**:

- **Build Orchestration**: Invoking Make with toolchain detection, parallel jobs, and profile compilation flags.
- **Run Profiles**: Managing runtime presets (`Standard`, `Performance`, `Benchmark`, `Diagnostics`, `Software`).
- **Visual Oracle**: Deterministic input-recording (`.pad`) replay, vblank deadlines, multi-window frame captures, and diffing.
- **Diagnostic Tooling**: Function trace diffing (`DiffFunc`), symbol resolution (`FindSymbol`), and fuzzing harness execution.
- **Process Lifecycle & Environment**: Window persistence, Vulkan SDK path normalization, MSYS2 toolchain configuration, and log rotation.
- **Title Manifest Planning**: Integration with `tools/title_manager_plan.ps1` and `tools/title_codegen_plan.py` when `-TitleManifest` is passed.

The name `hst_manager.ps1` is a historical artifact from the single-title origin of the
project. Now that data-driven title profiles ([`TITLE_PROFILE_ARCHITECTURE.md`](TITLE_PROFILE_ARCHITECTURE.md))
and title-manifest runtime bindings ([`TITLE_CODEGEN_PLAN.md`](TITLE_CODEGEN_PLAN.md)) exist,
the manager must reflect the same multi-title genericity.

---

## 2. Technical Audit: Legacy HST Coupling Inventory

This historical census describes the pre-decoupling `hst_manager.ps1`, before
PR #198: approximately 56 lines out of its then 1,662 lines. The locations and
quotation below refer to that version, not the current forwarding wrapper:

| Coupled Surface | Location in `hst_manager.ps1` | Description / Issue |
| --- | --- | --- |
| `GAME_NAME=hst` | Line 268 | Hardcoded fallback game name when `-TitleManifest` is not specified |
| `build/hst` | Lines 233, 400, 1176 | Hardcoded build directory path |
| `hst.exe` / `hst_recomp_*.c` | Lines 543–545, 786, 965, 1093, 1103, 1118, 1202 | Hardcoded executable target name and generated C chunk filenames |
| `0x0029a060` | Lines 247–248, 257 | Hardcoded legacy HST entry point literal |
| `place_game_here/` layout | Lines 216, 223, 229, 231, 321, 324, 327–331, 478, 498, 500, 1005, 1018, 1217, 1221, 1389 | Assumes HST-specific retail directory and module structure |
| `HST_EXTRA_SPANS` | Line 306 | HST-specific environment variable alias (generic is `TITLE_EXTRA_SPANS`) |

The script itself explicitly identifies this debt:

> *"That literal is the last hardcoded title guest address in this manager and is title coupling in generic tooling"* — `hst_manager.ps1:247-248`

---

## 3. Manifest-Driven Replacement

The generic title infrastructure underneath already supersedes every hardcoded item:

1. **Title Name & Executable**: Derived from `manifest.id` (projected as `GAME_NAME`), placing outputs in `build/<id>/<id>.exe` and generating `<id>_recomp_*.c`.
2. **Guest Entry Point**: Derived from `manifest.runtime_bindings.fallback_entry` (projected as `run_entry` by `title_codegen_plan.py`).
3. **Extra Executable Spans**: Derived from `manifest.executable.extra_spans` (projected as `TITLE_EXTRA_SPANS`).
4. **Input Layout**: Manifests declare required PRX modules, disc structures, and data containers.

When `-TitleManifest` is supplied, `title_manager_plan.ps1` executes and overrides all
legacy values with validated, canonical plan projections.

---

## 4. Migration & Transition Roadmap

### Phase 1: Tracking & Design Specification (Complete)

- Document the decoupling strategy and register [issue #196](https://github.com/Jstar269/nakagawa-recomp/issues/196).
- Define the transition interface and backward-compatibility rules.

### Phase 2: Introduction of `nk_manager.ps1` & Forwarding Shim (Complete)

- Create `nk_manager.ps1` as the canonical build manager.
- Implement title-agnostic parameter handling:
  - If `-TitleManifest` is omitted, either require it or default to a safe public synthetic manifest (`assets/titles/synthetic.json`).
  - Derive `GAME_NAME`, build directories, and executable names dynamically from the active plan.
- Retain `hst_manager.ps1` as a thin backward-compatibility wrapper that issues a warning and invokes `nk_manager.ps1` with the HST manifest.

Merged in PR #198. `nk_manager.ps1` is the canonical entry point and
`hst_manager.ps1` remains the compatibility wrapper for the local HST input route.

### Phase 3: Companion Tooling Decoupling (Implementation complete; provenance validation separate)

- [x] Add `nk.ps1` and preserve `hst.ps1` as a deprecated forwarding wrapper.
- [x] Add `tools/nk_safety.ps1` and preserve `tools/hst_safety.ps1` as a deprecated wrapper.
- [x] Add title-neutral `tools/nk_doctor*.py` modules and preserve the `hst_doctor*.py` names as deprecated wrappers.
- [ ] Update the public source profile and exact reviewed-blob admission for every new implementation-bearing path.

The C-1 runtime diagnostic gate is complete independently: retained HLE diagnostic
reads require the validated HST code-generation profile and `SR_HLE_DIAGNOSTICS`.
The companion scripts and wrappers now exist in the current tree. Their presence
establishes implementation status, not external provenance attestation; the
publication checklist item still requires trusted evidence.

### Phase 4: Retirement of Hardcoded HST Literals

- Delete the `0x0029a060` entry literal and `GAME_NAME=hst` fallback.
- Remove hardcoded assumptions about `place_game_here/` layout in generic code paths; delegate input location checks to manifest validation.

### Phase 5: Companion Tooling Rename (Implemented under Phase 3)

The renames below landed with Phase 3's companion-tooling work; the `hst_*`
names remain as deprecated wrappers. This list is kept as the rename map.

- `hst.ps1` → `nk.ps1`
- `tools/hst_safety.ps1` → `tools/nk_safety.ps1` (updating `Assert-HstWorkspaceRoot` → `Assert-NkWorkspaceRoot`)
- `tools/hst_doctor.py` → `tools/nk_doctor.py`

### Phase 6: Documentation & Test Sweep

- Update references in `docs/DEBUGGING.md`, `docs/PORTING.md`, `docs/TITLE_CODEGEN_PLAN.md`, and developer guides.
- Update test cases in `tools/test_*.py` that invoke `hst_manager.ps1`.

---

## 5. Acceptance Criteria

1. **Zero Hardcoded Guest Literals**: `nk_manager.ps1` contains 0 hardcoded title guest addresses or disc IDs.
2. **Hermetic Manifest Builds**: Building with `-TitleManifest assets/titles/synthetic.json` produces `build/synthetic/synthetic.exe` without touching or requiring any HST files.
3. **Backward Compatibility**: Calling `hst_manager.ps1` continues to function during the deprecation period by delegating to `nk_manager.ps1`.
4. **Clean Verification**: All automated unit tests and publication audits pass.
