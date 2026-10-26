# Enhancement Contract: Authentic Baseline, Capability Isolation, and Mod Boundaries

> **Status: CURRENT — maintained architectural contract.** Issue reference: #327. Build cache coordination coordinates with #316, and high-refresh presentation coordinates with #328.

Sections 1–7 define the authentic baseline, category boundaries, fail-closed package validation (`tools/enhancement_package.py`), and AOT cache epoch separation (`tools/nk_core/package_cache.py`, `tools/codegen.py`). Declarative frame-rate rescale packages (60 Hz logic rescaling for 30 Hz titles) are implemented on the AOT codegen and package-builder path; host runtime asset replacement and overlay loaders remain in the works.

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

PSP titles commonly lock physics and animation loops to 30 Hz or 60 Hz VBLANK ticks. Enhancing presentation or rescaling frame-counted game logic to run at a true 60 Hz requires strict separation from real-time clocks (see #328):

- **Allowed:** Interpolating host rendering between consecutive guest frames; adjusting presentation swap intervals; multiplying display updates while keeping guest timer tick counts authentic; applying declarative frame-rate rescale transforms (`count_x2`, `step_x0.5`, `decay_k_to_1-sqrt(1-k)`, `probability_per_update_x0.5`, `gate_30hz`, `vblank_wait_2_to_1`) to verified symbolic sites during an explicit AOT enhancement epoch.
- **Forbidden:** Speeding up the guest real-time clock (`sceKernelGetSystemTime*`, `sceRtc*`, audio hardware sample clocks) to force higher frame rates, which breaks real-time clocks, physics, and audio synchronization. Only frame-counted logic is rescaled; the game's real-time clock stays 100% authentic.
- **RNG Policy for 60 Hz Rescaling:** Frame-rate rescale packages must declare an explicit `rng_policy` (`rescale_probability` or `subtick_30hz`) with its rationale. Under `rescale_probability`, stateless per-update probability thresholds use `probability_per_update_x0.5` ($p_{60} = 0.5\,p_{30}$) so expected events per second remain invariant at 60 Hz, while stateful multi-roll PRNG sequences or coupled physics/particle steps use `gate_30hz` to step on alternate 60 Hz ticks (30 Hz cadence) without altering PRNG state evolution.

---

## 3. Metadata, Flight Recorder, and Evidence Tracking

Captures, traces, and performance logs must never be mistaken for authentic execution evidence. When enhancement loading lands, every enabled enhancement must be stamped deterministically across all diagnostic channels:

1. **Process Exit and Diagnostics:** The native player and CLI must print visible status indicating that enhanced mode is active, naming the loaded package ID and capabilities.
2. **Flight Recorder (`flight_recorder_schema.json`):** Active enhancement package identifiers and their requested capabilities must be stamped into the recorder manifest header, and any event log recorded under enhanced execution must be tagged as non-authentic. The comparator (`tools/flight_diff.py`) prints MATCH only for two comparable, fully retained captures: differing build identities are reported, never compared, while differing schema/runtime/event-class/trigger coverage or any dropped history is refused as INCOMPARABLE (exit 3) instead of matching (#320).
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
- **Optional root properties:** `min_runtime_version` (semver), `hooks` (array of hook objects with `symbol` and `type` only), and—for declarative frame-rate rescale packages (`timing_frame_presentation` or `guest_memory_code_hook`)—`game_version` (exact title version string), `executable_sha256` (64-char lowercase hex SHA-256 of the stock guest ELF), `rng_policy` (`mode` and `reason`), and `sites` (non-empty array of symbolic instruction-site transform descriptors).
- **Symbolic site descriptors (`sites`):** Each site entry declares `id` (site identifier), `function` (symbolic function name, never a raw hex address or `FUN_`/`sub_`/`loc_`/`func_` stem), `insn_offset` (non-negative 4-byte-aligned byte offset inside the function), `stock_digest` (64-char lowercase hex SHA-256 of the expected little-endian stock MIPS instruction word or words), optional `word_count` (`1` or `2`, default `1`), and `transform` (one of `count_x2`, `step_x0.5`, `decay_k_to_1-sqrt(1-k)`, `probability_per_update_x0.5`, `gate_30hz`, `vblank_wait_2_to_1`).
- **No speculative payloads:** Unimplemented category payloads (`assets`, `presentation`, `timing`, `camera`, `save_data`) remain forbidden in the schema until their respective runtime loaders land.

The package loader and AOT codegen hook (`tools/enhancement_package.py` and `tools/codegen.py`) strictly adhere to fail-closed principles:

1. **Unknown Category or Transform:** Any package specifying an unrecognized category string or site transform is immediately rejected.
2. **Incompatible Schema Version:** Packages with `schema_version != 1` are rejected with an explicit version error.
3. **Address Literals:** Any hook symbol, capability name, or site `function` containing address literal patterns (`0x...`, `FUN_...`, `sub_...`, `loc_...`, `func_...`, bare 6–8 digit hex strings, or integer addresses) fails validation.
4. **Stock Instruction Digest Verification:** Before any AOT transform is emitted, `verify_and_resolve_sites()` checks `executable_sha256` against the guest ELF and verifies that the stock instruction word(s) at `function + insn_offset` match `stock_digest`. If any site's stock word differs, or if a symbolic function cannot be resolved, the entire package is refused and the build stays authentic.
5. **Undeclared Properties:** Extra, unknown, or malformed JSON properties cause validation failure (`additionalProperties: false`).
6. **Fallback to Authentic Baseline:** When an enhancement package fails to load or validate, the runtime and package builder do not crash or attempt a "best guess" partial patch. They refuse the package with a clear diagnostic and preserve 100% authentic execution.

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
4. **Phase 4: High-Refresh Frame Pacing and Declarative 60 Hz Logic Rescaling (#328):**
   - Declarative frame-rate rescale package format (`game_version`, `executable_sha256`, `rng_policy`, symbolic `sites` with `stock_digest` and the 6 transform classes).
   - Fail-closed loader and stock-word digest verifier in `tools/enhancement_package.py` and `tools/codegen.py`.
   - Separate AOT instrumentation epoch (`aot-enhancement-package-v1`) in `tools/nk_core/package_cache.py`, `tools/title_codegen_plan.py`, and `tools/nk_cli.py`, plus opt-in experimental 60 Hz setting (`--experimental-60hz`).
   - VBLANK multiplier and display list presentation decoupling.
5. **Phase 5: Symbolic Guest Hook Dispatch and Cache Epochs (#316):**
   - Symbolic hook resolution via private title profiles and instrumented AOT cache keys.

---

## 9. Declarative Frame-Rate Rescale Packages (60 Hz Logic)

### 9.1. Purpose and Authentic-Clock Invariant

Many PSP titles run a 30 Hz game-logic loop by waiting 2 VBLANK intervals per tick (`sceDisplayWaitVblankStart` or a 2-VBLANK counter gate) and tuning frame-counted durations, velocities, exponential decays, and per-tick probabilities for $\Delta t = 1/30\text{ s}$.

A declarative frame-rate rescale package upgrades a 30 Hz title to run its game logic at a true 60 Hz ($\Delta t = 1/60\text{ s}$) when opted in by the user (`--experimental-60hz` or `--enhancement-package <path>`), while obeying two strict invariants:

1. **Authentic mode is untouched:** When the setting is off (the default), generated C code and the `AotPackageManifest` cache digest are byte-for-byte identical to standard compilation. Enhanced builds are stored under a separate package directory (`packages/<disc_id>-enhanced-<pkg_id>`) and keyed under the `aot-enhancement-package-v1` epoch in `codegen_options`, never replacing the authentic package.
2. **No faster guest clock:** The guest real-time clock (`sceKernelGetSystemTime*`, `sceRtc*`, and audio hardware sample rates) remains 100% authentic. Only frame-counted logic sites are rescaled.

### 9.2. Declarative Site Transforms

Each entry in `sites` targets a symbolic function name (`function`) and a 4-byte-aligned byte offset (`insn_offset`) within that function, guarded by a SHA-256 digest (`stock_digest`) of the expected stock 32-bit little-endian MIPS instruction word(s) (`word_count`: `1` or `2`):

| Transform Class | Target Pattern | 60 Hz Effect |
| --- | --- | --- |
| `vblank_wait_2_to_1` | Immediate load `2` or `< 2` VBLANK threshold (`addiu`/`li`/`slti`/`sltiu`) | Changes the 2-VBLANK wait threshold to `1` so the main loop steps once per VBLANK (60 Hz). |
| `gate_30hz` | Branch (`beq`/`bne`/`bnez`/`beqz`), call (`jal`), or step (`addiu`/`addu`/`subu`) inside a 60 Hz callback | Gates execution on `(s->enh_tick & 1u) == 0` so 30 Hz-only state machines or stateful multi-roll subsystems step on alternate 60 Hz ticks without double-advancing. |
| `count_x2` | Frame-count immediate load or comparison (`addiu`/`li`/`ori`/`slti`/`sltiu`) | Doubles the frame count ($N \to 2N$) so real-time duration is preserved at 60 Hz. |
| `step_x0.5` | Per-tick constant step: integer (`addiu`/`li`), float (`lui` or `lui`+`ori` pair), or shift (`sll`/`sra`/`srl`) | Halves the per-tick step ($\Delta x \to 0.5\,\Delta x$) so rate of change per second is preserved at 60 Hz. |
| `decay_k_to_1-sqrt(1-k)` | Per-tick float multiplier / damping factor (`lui` or `lui`+`ori` loading IEEE-754 float $m \in (0,1)$) | Replaces the 30 Hz multiplier $m_{30} = 1 - k$ with $m_{60} = \sqrt{m_{30}}$ (or damping $k$ with $1 - \sqrt{1 - k}$) so two 60 Hz steps equal one 30 Hz decay step ($m_{60}^2 = m_{30}$). |
| `probability_per_update_x0.5` | Per-tick probability threshold immediate (`addiu`/`li`/`ori`/`slti`/`sltiu`/`lui`) | Halves the per-tick threshold ($p_{60} = 0.5\,p_{30}$) so expected triggers per second match 30 Hz. |

### 9.3. Random-Number Generator (RNG) Policy Decision

When a 30 Hz game loop runs at 60 Hz, code that queries the PRNG on every update tick would double its event frequency and advance the PRNG state twice as fast unless handled explicitly. Every frame-rate rescale package must declare `rng_policy`:

- **Selected primary mode (`rescale_probability`):** Stateless per-update probability comparisons (where a single roll is compared against a constant threshold to trigger a sound, blink, or ambient effect) use `probability_per_update_x0.5` so that checks can run smoothly on every 60 Hz frame while keeping the per-second trigger rate invariant. Stateful multi-roll PRNG consumers (such as particle emitters that draw multiple correlated rolls per spawn or physics/AI routines whose state transitions are tightly coupled to 30 Hz step counts) are paired with `gate_30hz` so they execute on a 30 Hz sub-tick (`(s->enh_tick & 1u) == 0`), preserving exact 30 Hz PRNG consumption and trajectory behavior.
- **Alternative mode (`subtick_30hz`):** Packages for titles where all PRNG rolls must remain strictly on a 30 Hz sub-tick declare `mode: "subtick_30hz"` and gate the roll sites with `gate_30hz`.

### 9.4. Building and Running with an Experimental 60 Hz Package

1. **Validate a package manifest (and optional symbol map sidecar):**
   ```powershell
   python tools/enhancement_package.py path/to/pkg-60hz.json
   ```
2. **Build an enhanced title package via `nk_cli.py` (leaves the authentic package untouched):**
   ```powershell
   python tools/nk_cli.py build-package <DISC_ID> --experimental-60hz --enhancement-package path/to/pkg-60hz.json
   ```
   Or via `title_codegen_plan.py`:
   ```powershell
   python tools/title_codegen_plan.py assets/titles/<title>.json --package --game-elf <EBOOT.elf> --enhancement-package path/to/pkg-60hz.json --output-dir <output-dir>
   ```
3. **Launch with the experimental 60 Hz toggle:**
   ```powershell
   python tools/nk_cli.py launch <DISC_ID> --experimental-60hz --enhancement-package path/to/pkg-60hz.json
   ```
   In the native player (`nakagawa_player`), pass `--experimental-60hz` on the command line or toggle `experimental_60hz` in `settings.json` (off by default).
