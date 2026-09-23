# Title Profile and Launch Session Architecture

> **Status: CURRENT.** This page describes the public title-manifest and launch contracts visible in source. Private title manifests, game-derived bindings, and acceptance evidence stay outside the public repository.

## 1. Goal and current evidence

The architecture goal is to keep title identity and title-specific inputs out of generic UI and runtime defaults. Source-owned title data is validated before it reaches the registry, build planner, or launch path.

The current source proves a narrower claim than complete multi-title acceptance: Python and native launch resolution bind a session to a validated title identity, and the test suite covers fail-closed identity/path selection and parity on synthetic fixtures. A real second-title bring-up remains open in [issue #285](https://github.com/Jstar269/nakagawa-recomp/issues/285). The project does not claim that every title can be supported without presentation or integration work.

```text
public title manifest
        │
        ▼
tools/title_manifest.py ──► validated catalog ──► registry / build planner
                                                       │
selected source ──► preparation ──► local manifest ──┤
                                                       ▼
                                      validated launch session
                                                       │
                                                       ▼
                                      child runtime + launch inputs
```

The title catalog, per-user preparation output, and child-process environment are separate contracts. A value may pass from one to another only through the corresponding validator or launch-session builder.

## 2. Public title catalog contract

The authoritative public shape is [`assets/title_manifest.schema.json`](../assets/title_manifest.schema.json), enforced and normalized by [`tools/title_manifest.py`](../tools/title_manifest.py). The source-owned [`display-smoke.json` fixture](../assets/titles/display-smoke.json) is an actual synthetic manifest checked by the manifest tests; it is a better example of the current format than an independent schema sketch in this document. The format rejects unknown fields and separates retail disc identity from the shared synthetic/homebrew shape.

The schema covers manifest identity and kind, retail disc identity, executable layout, module inventory, filesystem roots, game/HLE/code-generation identifiers, feature requirements, compatibility-manifest and verification references, and notes. Optional `runtime_contract` and `profile_zero` blocks have additional machine-checked constraints; `profile_zero` is limited to synthetic manifests. Their exact required fields belong to the schema and validator, not to a second prose-defined schema.

### Temporary compatibility configuration

The current schema also permits an optional, typed `runtime_bindings` block for specific compatibility seams. These bindings are not structural facts about a disc and do not establish generic PSP semantics. They are bounded configuration consumed by the runtime; `required_runtime_bindings` names the binding families the selected profile depends on so a missing required family fails validation. These mechanisms remain compatibility debt and are expected to retire as the underlying generic behavior becomes correct. [Issue #363](https://github.com/Jstar269/nakagawa-recomp/issues/363) tracks that work; [`TITLE_CODEGEN_PLAN.md`](TITLE_CODEGEN_PLAN.md) documents validation and runtime consumption, and [`PORTING.md`](PORTING.md) inventories the remaining title coupling.

The target remains a generic core that does not require title-specific patches. The current contract records the temporary bindings honestly rather than describing the target state as already achieved. Public schema fields do not authorize private paths, retail bytes, captures, or derived evidence.

## 3. Catalog identity and generic launch resolution

The registry maps validated manifest identity to title metadata. Preparation and launch use that mapping rather than a default title or a title-shaped path guess. The Python launcher in `tools/nk_core/launcher.py` and native launcher in `src/core/nk_launch.c` resolve the selected runtime and image from validated identity and fail closed when identity or a matching artifact is missing.

The launcher contract and its synthetic cross-title regressions are tracked by [issue #366](https://github.com/Jstar269/nakagawa-recomp/issues/366). This source-level result does not replace the second-title production route in #285, nor does it prove private retail acceptance.

## 4. Local prepared-game metadata

Preparation writes a local `manifest.json` beside the staged game. This is operational metadata for one prepared copy, not a public catalog manifest. The current preparation code records the schema and engine versions, validated title and disc identities, display metadata, the source ISO path and size, creation time, and the runtime/archive/save settings derived from the selected profile. Because it can contain a user-supplied source path, keep it in local application state and do not copy it into public source or evidence.

The preparation manifest's `engine_version` is currently emitted from `PREP_ENGINE_VERSION` in `tools/nk_core/prep_engine.py`. Its relationship to the release and package version authorities is still being reconciled under [issue #333](https://github.com/Jstar269/nakagawa-recomp/issues/333); this page therefore does not assign it a version value.

## 5. Launch-session transport

Both launchers turn validated session data into runtime inputs for a child process. The native path uses the typed `NkLaunchSession` in `src/core/nk_launch.h`; the Python path validates the local manifest and resolves its title against the registry. The launchers derive runtime values such as `PSP_ISO`, `SR_DATAROOT`, frame-rate and graphics settings, and diagnostic flags from that session and its configuration, then pass them through the child environment.

Environment variables are the process transport; they are not the source of title identity. The Python launcher starts with a copy of the host environment and sets its session-owned runtime keys from validated state. The native launcher constructs the child environment from the prepared session and runtime settings. The runtime's data-root and ISO inputs therefore remain explicit launch inputs even though the final OS boundary uses environment entries.

For the native player’s current implementation boundary and process lifecycle, see [`NATIVE_PLAYER_ARCHITECTURE.md`](NATIVE_PLAYER_ARCHITECTURE.md) and [`RUNTIME_PACKAGING_ARCHITECTURE.md`](RUNTIME_PACKAGING_ARCHITECTURE.md). Those pages describe a prepared-runtime launch path; they do not claim arbitrary-ISO package provisioning is complete.

## 6. Evidence and remaining boundaries

- Manifest structure is enforced by the shared schema/validator and public fixture tests.
- Python/native identity and path resolution have source-owned parity and fail-closed regression coverage under #366.
- A real second-title bring-up remains pending under #285.
- Temporary profile-owned compatibility bindings remain visible debt under #363.
- Release and preparation version authority remains unresolved under #333.
- Private title inputs and private acceptance remain separate from public schema and synthetic-fixture results.
