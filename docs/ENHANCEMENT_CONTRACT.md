# Enhancement Contract: Authentic Baseline, Capability Isolation, and Mod Boundaries

> **Status: CURRENT — maintained architectural contract.** Issue reference: #327. Build cache coordination coordinates with #316, and high-refresh presentation coordinates with #328.

**In the works:** no enhancement package can be loaded yet. The runtime and player have no enhancement loader; today this contract, the declaration schema and the package validator exist. Sections 3, 4 and 6 state requirements the loader must meet when it lands.

## 1. Core Principle: Authentic Mode is the Untouched Baseline

Nakagawa Recomp's primary architectural foundation is verifiable, measurable, and bug-compatible PlayStation Portable execution on native 64-bit Windows. The authentic/fidelity execution mode is the project's authoritative ground truth.

To prevent runtime contamination, title-specific regressions, and untracked behavior:

1. **Authentic mode is authoritative:** All canonical correctness suites, hardware-oracle measurements, public acceptance tests, and private fidelity routes execute strictly with all host enhancements disabled.
2. **Untouched baseline:** In authentic mode, the runtime and recompiled code execute with zero enhancement overhead, zero mutation of guest state, and zero host presentation deviations. The instruction trace and hardware-observable behavior remain bit-for-bit identical to standard recompilation.
3. **Opt-in layering:** Host enhancements (such as HD texture replacement, high-refresh pacing, free cameras, or debug overlays) are explicit, isolated capabilities layered strictly above the authentic baseline.
4. **No hidden emulator hacks:** Enhancements are never implemented as ad hoc bypasses, magic memory writes, or hardcoded addresses inside the generic runtime core.

---

## 2. Enhancement Categories and Observation/Mutation Boundaries

Every enhancement package must declare exactly one primary category. Each category defines a rigid observation and mutation contract that determines what guest and host state the enhancement is permitted to inspect or modify. Speculative category payload objects (such as asset replacement tables, presentation parameters, timing multipliers, camera overrides, or save-data configurations) are deliberately not defined in the base declaration schema: each future capability defines its payload specification when that capability lands. The categories below define the observation and mutation boundaries, and the descriptions illustrate future capability examples.

| Category | Primary Target | Observation Boundary | Mutation Boundary | Strict Prohibitions |
| --- | --- | --- | --- | --- |
| `asset_replacement` | Textures, audio streams, fonts, UI icons | Asset identifiers, file read requests, texture descriptors | Host GPU texture allocations, host audio stream playback buffers | Cannot read or mutate guest registers, guest execution flow, or game logic |
| `presentation_only` | Host overlays, FPS counters, aspect-ratio framing, post-shaders | Host swapchain state, frame timings, compositor layout | Host presentation pass, render target compositing, debug overlay draw lists | Zero mutation of guest memory, guest registers, or PSP display list structures |
| `guest_memory_code_hook` | Intro skipping, symbolic routine interception, gameplay patches | Symbolic function entry/exit, exported milestone events, guest memory | Guest memory values, guest function return registers (`$v0`/`$v1`) at symbolic boundaries | Cannot use raw host or guest numeric address literals; must target symbolic identifiers only |
| `save_data_modification` | Save file converters, save-state slot remapping, progression unlocks | Save serialization calls, savedata header structures | On-disk save payloads during load/save serialization | Cannot modify live guest state during gameplay; strictly restricted to I/O serialization seams |
| `camera_projection_override` | Ultrawide FOV adjustment, projection aspect ratio, free camera | GE transform matrices, view matrices, camera update calls | Host projection matrices submitted to the graphics backend | Cannot modify guest physics or collision boundaries unless paired with a declared guest hook |
| `timing_frame_presentation` | High-refresh display (#328), frame interpolation, VBLANK multiplier | VBLANK interrupts, display buffer swap requests, frame intervals | Host presentation swap cadence, presentation interval throttling | Must preserve guest internal game tick rate; cannot silently accelerate gameplay logic |

### 2.1. Asset Replacement (`asset_replacement`)

Asset replacement operates at the resource loading boundary. When the guest title requests a texture or audio track via standard PSP interfaces, the host loader matches the resource identifier (or cryptographic hash of the original asset) against declared replacement tables.

- **Allowed:** Replacing a 128x128 8-bit paletted texture with a host 4K BC7 texture in the GPU pipeline; routing audio playback through a high-definition FLAC/WAV stream.
- **Forbidden:** Altering memory layouts visible to guest CPU code or modifying game data structures in guest RAM.

### 2.2. Presentation-Only (`presentation_only`)

Presentation enhancements operate strictly after or alongside the host graphics presentation step.

- **Allowed:** Drawing an ImGui-based diagnostic HUD or FPS counter; applying CRT or CAS post-processing shaders to the final swapchain render target; letterboxing 16:9 PSP output into ultrawide display windows.
- **Forbidden:** Mutating GE display list commands, intercepting guest draw calls to alter gameplay geometry, or feeding host input back into the guest controller state.

### 2.3. Guest Memory and Code Hooks (`guest_memory_code_hook`)

Guest code hooks represent the highest-risk category because they directly alter guest architectural state.

- **Allowed:** Intercepting a symbolic function call (e.g., `title_skip_intro_movie`) to set return register `$v0 = 1` and bypass playback; adjusting game progression flags at verified symbolic lifecycle points.
- **Forbidden:** Specifying raw hex address literals (e.g., `0x08804000`); embedding commercial title binaries or patched MIPS machine code inside the package; executing arbitrary unverified host shellcode.

### 2.4. Save-Data Modification (`save_data_modification`)

Save-data enhancements interface exclusively with the virtual filesystem (`ms0:/PSP/SAVEDATA/`) and the PSP utility savedata subsystem (`sceUtilitySavedata`).

- **Allowed:** Remapping save slots to host profile folders; importing external savedata with checksum recomputation; converting regional save formats.
- **Forbidden:** Hooking active memory during combat or gameplay to simulate infinite inventory or health; altering save data during authentic hardware-oracle test executions.

### 2.5. Camera and Projection Override (`camera_projection_override`)

Camera overrides operate at the 3D projection interface, transforming guest camera matrices before graphics submission.

- **Allowed:** Calculating 21:9 or 32:9 projection matrices from guest 16:9 aspect ratios; providing host mouse/gamepad camera offsets to view matrices.
- **Forbidden:** Altering collision meshes, modifying culling logic without declaring a guest hook, or de-syncing game physics from camera motion.

### 2.6. Timing and Frame Presentation (`timing_frame_presentation`)

PSP titles commonly lock physics and animation loops to 30 Hz or 60 Hz VBLANK ticks. Enhancing presentation to high-refresh rates (120 Hz, 144 Hz, 240 Hz) requires careful decoupling (see #328).

- **Allowed:** Interpolating host rendering between consecutive guest frames; adjusting presentation swap intervals; multiplying display updates while keeping guest timer tick counts authentic.
- **Forbidden:** Speeding up the guest real-time clock to force higher frame rates, which breaks animation speed, physics, and audio synchronization.

---

## 3. Metadata, Flight Recorder, and Evidence Tracking

Captures, traces, and performance logs must never be mistaken for authentic execution evidence. When enhancement loading lands, every enabled enhancement must be stamped deterministically across all diagnostic channels:

1. **Process Exit and Diagnostics:** The native player and CLI must print visible status indicating that enhanced mode is active, naming the loaded package ID and capabilities.
2. **Flight Recorder (`flight_recorder_schema.json`):** Active enhancement package identifiers and their requested capabilities must be stamped into the recorder manifest header, and any event log recorded under enhanced execution must be tagged as non-authentic.
3. **Bring-Up Reports (`bringup_report.schema.json`):** Public bring-up and stage evidence reports must reject runs performed with enhancements enabled unless explicitly categorized under a separate non-canonical evidence suite.
4. **Crash Dumps and Logs:** Crash reports must include full enhancement provenance, identifying whether the failure occurred within authentic core execution or inside an enhancement hook boundary.

> **Rule:** No test run or performance capture conducted with enhancements enabled may be cited as evidence of authentic PSP hardware conformance or fidelity acceptance.

---

## 4. AOT and Build Cache Invariants (Coordination with #316)

Issue #316 establishes content-addressed private AOT build caches with explicit compatibility epochs (`AotPackageManifest`). Reusing compiled native C chunks safely requires that cache identity reflect all semantic inputs that affect code generation.

Enhancements are governed by the following cache rules:

- **Presentation, asset, and timing enhancements do not invalidate AOT cache:** Because presentation overlays, texture replacements, and swapchain pacing operate entirely at the host runtime layer, they require zero modifications to generated C code. Authentic AOT packages are shared without recompilation.
- **No silent AOT mutation:** An enhancement package can never silently alter the generated C output of a title.
- **Explicit AOT instrumentation epoch:** If an advanced guest hook requires ahead-of-time code generation hooks or stub wrappers, that hook dependency must be explicitly included in the `AotPackageManifest` cache key. An instrumented AOT package produces a distinct cache digest and cannot collide with or replace the authentic baseline package.

---

## 5. Symbolic Capabilities vs. Raw Addresses

A critical design requirement of Nakagawa Recomp is the complete absence of title-specific guest addresses or proprietary game names from generic code (`TITLE_PROFILE_ARCHITECTURE.md`).

Enhancements must follow the same strict rule:

- **Symbolic hook targets and capability names only:** Enhancement packages declare hook points using symbolic function names (e.g., `symbol: "title_init_display"`, `symbol: "stage_load_complete"`) and request capabilities via symbolic names (e.g., `capabilities: ["hud_overlay"]`).
- **Prohibition of raw address literals:** Enhancement package manifests must not contain raw numerical or hexadecimal addresses in hook symbols or capability names. The validator enforces a uniform rejection rule across all forms: 0x-hex (e.g. `0x08804000`), decompiler-style stems (`sub_`, `loc_`, `func_`), bare 6-8 digit hex strings (including all-letter strings like `deadbeef`), and numeric addresses.
- **Resolution via title profile:** Symbolic hook names are resolved to guest addresses exclusively through the title's private profile and symbol registry. The generic enhancement runtime operates strictly on resolved symbols, keeping all commercial addresses out of package declarations.

---

## 6. Fail-Closed Validation and Incompatible Packages

Enhancement packages are declared as versioned JSON manifests adhering to `assets/enhancement_package.schema.json` and validated by the normative hand-written validator in `tools/enhancement_package.py`.

The declaration schema is kept lean and focused on core metadata and capability requests:

- **Required root properties:** `schema_version` (must be 1), `id` (package identifier), `display_name`, `version` (semver), `category` (one of the 6 recognized categories), `target_title_id` (title ID or `*`), and `capabilities` (non-empty array of symbolic capability names).
- **Optional root properties:** `min_runtime_version` (semver) and `hooks` (array of hook objects with `symbol` and `type` only).
- **No speculative payloads:** Payload objects for specific categories are defined when those capabilities land in the runtime, not speculatively in the base schema.

The package loader strictly adheres to fail-closed principles:

1. **Unknown Category:** Any package specifying an unrecognized category string is immediately rejected.
2. **Incompatible Schema Version:** Packages with `schema_version != 1` are rejected with an explicit version error.
3. **Address Literals:** Any hook symbol or capability name containing address literal patterns fails validation.
4. **Undeclared Properties:** Extra, unknown, or malformed JSON properties cause validation failure (`additionalProperties: false`).
5. **Fallback to Authentic Baseline:** When an enhancement package fails to load or validate, the runtime does not crash or attempt a "best guess" execution. It reports a clear, human-readable error to the user and continues execution in 100% authentic mode.

---

## 7. Project API vs. Third-Party Script Separation

The enhancement architecture enforces a clean boundary between the compiled host runtime and external mod packages:

- **Project-Authored Enhancement API:** Nakagawa provides a structured, type-safe C interface (`src/rt/`) for presentation hooks, texture substitution callbacks, and frame pacing. This API is versioned, reviewed, and compiled as part of the native player.
- **Declarative Mod Packages:** Third-party enhancements are distributed as declarative JSON packages containing metadata, replacement asset files, and symbolic hook points.
- **No Untrusted Scripting Core:** The runtime does not embed a general-purpose scripting engine (such as Lua, Python, or JavaScript) in the core execution path. Untrusted external scripts cannot run arbitrary host code or compromise memory safety.

---

## 8. Implementation Roadmap and First-Proof Fixture

Implementation of the enhancement subsystem is phased to maintain fidelity at every milestone:

1. **Phase 1: Contract, Schema, and Package Validator (Current Task — Issue #327):**
   - Architectural contract defined in `docs/ENHANCEMENT_CONTRACT.md`.
   - Machine-readable package schema in `assets/enhancement_package.schema.json`.
   - Standalone package validation tooling in `tools/enhancement_package.py`.
   - Comprehensive unit test suite in `tools/test_enhancement_package.py`.
2. **Phase 2: First-Proof Synthetic Presentation Fixture (Next Step):**
   - Implement a minimal, source-owned synthetic presentation overlay fixture in the native player.
   - Prove that authentic execution remains 100% byte- and trace-equivalent when the overlay is disabled.
   - Verify that enabling the overlay stamps diagnostics into evidence metadata without mutating guest memory.
   - Verify fail-closed package rejection in the native loader.
3. **Phase 3: Asset Replacement Engine:**
   - Texture dumping and replacement interface at the host GPU backend.
   - Streaming replacement audio handler.
4. **Phase 4: High-Refresh Frame Pacing (#328):**
   - VBLANK multiplier and display list presentation decoupling.
5. **Phase 5: Symbolic Guest Hook Dispatch and Cache Epochs (#316):**
   - Symbolic hook resolution via private title profiles and instrumented AOT cache keys.
