# Decoupling the Generic Build Orchestrator (`nk_manager`)

This document records the completed decoupling of Nakagawa Recomp's build,
test, and verification manager from legacy title-specific coupling (*Hot Shots
Tennis: Get a Grip!*, UCUS98701). The canonical orchestrator is `nk_manager.ps1`;
the HST-prefixed compatibility entry points were retired under issue #338.

This work was tracked under [issue #196](https://github.com/Jstar269/nakagawa-recomp/issues/196)
as part of the broader Title-#2 readiness roadmap ([issue #98](https://github.com/Jstar269/nakagawa-recomp/issues/98)).
Both issues are now closed; see the linked live records.

---

## 1. Context & Motivation

`nk_manager.ps1` is the canonical orchestration layer for developer workflows in
Nakagawa Recomp. The manager provides **generic recompiler infrastructure**:

- **Build Orchestration**: Invoking Make with toolchain detection, parallel jobs, and profile compilation flags.
- **Run Profiles**: Managing runtime presets (`Standard`, `Performance`, `Benchmark`, `Diagnostics`, `Software`).
- **Visual Oracle**: Deterministic input-recording (`.pad`) replay, vblank deadlines, multi-window frame captures, and diffing.
- **Diagnostic Tooling**: Function trace diffing (`DiffFunc`), symbol resolution (`FindSymbol`), and fuzzing harness execution.
- **Process Lifecycle & Environment**: Window persistence, Vulkan SDK path normalization, MSYS2 toolchain configuration, and log rotation.
- **Title Manifest Planning**: Integration with `tools/title_manager_plan.ps1` and `tools/title_codegen_plan.py` when `-TitleManifest` is passed.

The old `hst_manager.ps1` name was a historical artifact from the single-title
origin of the project. Data-driven title profiles
([`TITLE_PROFILE_ARCHITECTURE.md`](TITLE_PROFILE_ARCHITECTURE.md)) and title-manifest
runtime bindings ([`TITLE_CODEGEN_PLAN.md`](TITLE_CODEGEN_PLAN.md)) now own that
variation, so the compatibility entry points are removed.

---

## 2. Technical Audit: Legacy HST Coupling Inventory

This historical census describes the pre-decoupling `hst_manager.ps1`, before
PR #198: approximately 56 lines out of its then 1,662 lines. The locations and
quotation below refer to that version, not the removed wrapper:

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

### Phase 2: Introduction of `nk_manager.ps1` (Complete)

- Create `nk_manager.ps1` as the canonical build manager.
- Implement title-agnostic parameter handling:
  - If `-TitleManifest` is omitted, default to the safe public synthetic manifest (`assets/titles/synthetic.json`).
  - Derive `GAME_NAME`, build directories, and executable names dynamically from the active plan.
- Migrate the local HST route to that manager with an explicit private manifest.

The temporary forwarding shim used during migration is removed. `nk_manager.ps1`
is the only maintained manager entry point.

### Phase 3: Companion Tooling Decoupling (Complete)

- [x] Add `nk.ps1` as the simple frontend.
- [x] Add `tools/nk_safety.ps1` as the safety owner.
- [x] Add title-neutral `tools/nk_doctor*.py` modules.
- [x] Remove the HST-prefixed frontend, manager, doctor, safety, and run-support compatibility files.

The C-1 runtime diagnostic gate is complete independently: retained HLE diagnostic
reads require the validated HST code-generation profile and `SR_HLE_DIAGNOSTICS`.

### Phase 4: Retirement of Hardcoded HST Literals

- Delete the `0x0029a060` entry literal and `GAME_NAME=hst` fallback.
- Remove hardcoded assumptions about `place_game_here/` layout in generic code paths; delegate input location checks to manifest validation.

### Phase 5: Companion Tooling Rename (Complete)

The canonical names are:

- `hst.ps1` → `nk.ps1`
- `tools/hst_safety.ps1` → `tools/nk_safety.ps1` (updating `Assert-HstWorkspaceRoot` → `Assert-NkWorkspaceRoot`)
- `tools/hst_doctor.py` → `tools/nk_doctor.py`

### Phase 6: Documentation & Test Sweep (Complete)

- Maintained docs, tests, CI routing, and dashboard code use the `nk_*` entry points.
- A regression gate rejects removed HST compatibility paths and generic-tool dependencies.

---

## 5. Acceptance Criteria

1. **Zero Hardcoded Guest Literals**: `nk_manager.ps1` contains 0 hardcoded title guest addresses or disc IDs.
2. **Hermetic Manifest Builds**: Building with `-TitleManifest assets/titles/synthetic.json` produces `build/synthetic/synthetic.exe` without touching or requiring any HST files.
3. **Single maintained entry point:** build, test, and verification workflows use `nk.ps1` or `nk_manager.ps1`; removed HST-prefixed compatibility files are not retained.
4. **Clean Verification**: All automated unit tests and publication audits pass.
