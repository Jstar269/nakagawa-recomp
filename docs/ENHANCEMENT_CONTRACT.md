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

- **Allowed:** Interpolating host rendering between consecutive guest frames; adjusting presentation swap intervals; multiplying display updates while keeping guest timer tick counts authentic; applying declarative frame-rate rescale transforms (`count_x2`, `step_x0.5`, `decay_k_to_1-sqrt(1-k)`, `probability_per_update_x0.5`, `gate_30hz`, `vblank_wait_2_to_1`, `retention_m_to_sqrt(m)`) to verified symbolic sites during an explicit AOT enhancement epoch.
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
- **Optional root properties:** `min_runtime_version` (semver), `hooks` (array of hook objects with `symbol` and `type` only), and—for declarative frame-rate rescale packages (category `timing_frame_presentation` only)—`game_version` (exact title version string), `executable_sha256` (64-char lowercase hex SHA-256 of the stock guest ELF), `rng_policy` (`mode` and `reason`), and `sites` (non-empty array of symbolic instruction-site transform descriptors).
- **Symbolic site descriptors (`sites`):** Each site entry declares `id` (site identifier), `function` (symbolic function name, never a raw hex address or `FUN_`/`sub_`/`loc_`/`func_` stem), `insn_offset` (non-negative 4-byte-aligned byte offset inside the function), `stock_digest` (64-char lowercase hex SHA-256 of the expected little-endian stock MIPS instruction word or words), optional `word_count` (an integer from 1 to 16, default `1`, the number of words `stock_digest` covers; only `1` has a defined rewrite, so a larger value refuses the package), and `transform` (one of `count_x2`, `step_x0.5`, `decay_k_to_1-sqrt(1-k)`, `retention_m_to_sqrt(m)`, `probability_per_update_x0.5`, `gate_30hz`, `vblank_wait_2_to_1`).
- **No speculative payloads:** Unimplemented category payloads (`assets`, `presentation`, `timing`, `camera`, `save_data`) remain forbidden in the schema until their respective runtime loaders land.

The package loader and AOT codegen hook (`tools/enhancement_package.py` and `tools/codegen.py`) strictly adhere to fail-closed principles:

1. **Unknown Category or Transform:** Any package specifying an unrecognized category string or site transform is immediately rejected.
2. **Incompatible Schema Version:** Packages with `schema_version != 1` are rejected with an explicit version error.
3. **Address Literals:** Any hook symbol, capability name, or site `function` containing address literal patterns (`0x...`, `FUN_...`, `sub_...`, `loc_...`, `func_...`, bare 6–8 digit hex strings, or integer addresses) fails validation.
4. **Stock Instruction Digest Verification:** Before any AOT transform is emitted, `verify_and_resolve_sites()` checks `executable_sha256` against the guest ELF and verifies that the stock instruction word(s) at `function + insn_offset` match `stock_digest`. If any site's stock word differs, or if a symbolic function cannot be resolved, the entire package is refused and the build stays authentic. Codegen also refuses the package, before it writes any output, when a verified site is not emitted with its rewrite (for example, its function became an untranslatable stub).
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
   - Declarative frame-rate rescale package format (`game_version`, `executable_sha256`, `rng_policy`, symbolic `sites` with `stock_digest` and the section 9.2 transforms).
   - Fail-closed loader and stock-word digest verifier in `tools/enhancement_package.py` and `tools/codegen.py`.
   - Separate AOT instrumentation epoch (`aot-enhancement-package-v1`) in `tools/nk_core/package_cache.py`, `tools/title_codegen_plan.py`, and `tools/nk_cli.py`, plus the run-time opt-in (`SR_EXPERIMENTAL_60HZ`, the player's `experimental_60hz` setting).
   - VBLANK multiplier and display list presentation decoupling.
5. **Phase 5: Symbolic Guest Hook Dispatch and Cache Epochs (#316):**
   - Symbolic hook resolution via private title profiles and instrumented AOT cache keys.

---

## 9. Declarative Frame-Rate Rescale Packages (60 Hz Logic)

### 9.1. Purpose and Invariants

Many PSP titles run a 30 Hz game-logic loop by waiting two VBLANK intervals per update and tune frame-counted durations, steps, decays and per-update probabilities for $\Delta t = 1/30\text{ s}$. A frame-rate rescale package lists the instruction sites that make the loop wait two VBLANKs and the sites whose constants assume $1/30\text{ s}$, so the same logic can run once per VBLANK.

1. **Authentic mode is untouched.** Without `--enhancement-package` (the default), `tools/codegen.py` output and the codegen options in the AOT cache key are byte-identical to a build without this feature. A package build records `enhancement_package_epoch` (`aot-enhancement-package-v1`), the package id and digest in its codegen options, and `nk_cli.py build-package` writes it to its own directory (`packages/<disc_id>-enhanced-<pkg_id>`), never over the authentic package. `build-package` applies a package only when its command names one; it refuses to build while the environment names one on its own (`ENHANCEMENT_PACKAGE`, also through `MAKEFLAGS`, or `--enhancement-package` in `CODEGEN_USER_ARGS`), because the Makefile would apply it.
2. **Off at run time unless opted in.** Every rewritten site is emitted twice: the stock statement and its rewrite. The rewrite runs only when the process starts with `SR_EXPERIMENTAL_60HZ=1`; any other value runs the stock statements, so a package build with the toggle off executes the authentic instruction stream. The player exports that variable at launch when its saved `experimental_60hz` setting is on (off by default) or when it was started with `--experimental-60hz`. The flag applies to that session only and is never written to the settings file.
3. **No faster guest clock.** System time, RTC and audio sample clocks are untouched. Only the sites the package lists change.

### 9.2. Site Transforms

Each `sites` entry names a symbolic `function`, a 4-byte-aligned `insn_offset`, the SHA-256 `stock_digest` of the stock little-endian instruction word, and a `transform`. `plan_site_rewrite()` in `tools/enhancement_package.py` gives each (transform, instruction) pair exactly one rewrite, chosen from the transform and the instruction encoding alone; a pair without one refuses the whole package. A site is one word, or two words (`word_count: 2`) that build a float constant: `lui rt, hi` followed by `ori rt, rt, lo` of the same register. A pair takes one of the scaling transforms below, applied to the whole 32-bit value and rounded to nearest, and both words are rewritten (`plan_float_pair_rewrite()`).

| Transform | Instructions with a defined rewrite | Rewrite |
| --- | --- | --- |
| `vblank_wait_2_to_1` | `addi`/`addiu`/`slti`/`sltiu`/`ori` with immediate 2 | immediate 2 becomes 1 |
| `gate_30hz` | `jal`; accumulating `addiu rt, rt, imm`, `addu`/`subu`/`add`/`sub` with the destination as a source, `add.s`/`sub.s` likewise | the statement runs only on even logic updates (§9.5), so every instance runs on the same alternate updates |
| `count_x2` | positive `addi`/`addiu`/`slti`/`sltiu` immediate; `andi` mask $2^n-1$; `lui` of a normal float; `sll`; integer loads; `lwc1`/`swc1`; single-precision arithmetic and `cvt.s.w`; a unit countdown `addiu rt, rt, -1` | the produced value is doubled (a countdown is gated) |
| `step_x0.5`, `probability_per_update_x0.5` | as `count_x2` except integer loads, plus accumulating `+1`/`-1`/`addu`/`subu` updates | the produced value is halved (exactly, or by gating a unit or register step); `add.s fd, fd, ft` halves the addend `ft` |
| `decay_k_to_1-sqrt(1-k)` | `lui` of a float $k \in (0, 1)$ | $k \to 1 - \sqrt{1-k}$, rounded to the upper half |
| `retention_m_to_sqrt(m)` | `lui` of a float $m \in (0, 1)$ | $m \to \sqrt{m}$, rounded to the upper half |

The transforms scale the operand the site produces; they do not guess its role. A constant that multiplies or bounds a random roll (`rand() % 100`, `rand * 100.0f < p`) halves the per-update probability when it is doubled, so its site declares `count_x2`, not `probability_per_update_x0.5`. A `swc1` site stores a scaled copy of its register and never writes the register, so a store that faults under `--lle-cpu` leaves it unchanged.

### 9.3. Random-Number Generator (RNG) Policy

Every frame-rate rescale package declares `rng_policy`. Under `rescale_probability`, single-roll per-update checks keep running every update with their probability halved; stateful multi-roll consumers are gated with `gate_30hz` so they keep their 30 Hz roll order. Under `subtick_30hz`, every roll site is gated.

### 9.4. Building and Running

1. Validate a package: `python tools/enhancement_package.py path/to/pkg-60hz.json`.
2. Build a package build: `python tools/nk_cli.py build-package <DISC_ID> --enhancement-package path/to/pkg-60hz.json` (or `--experimental-60hz` with the package at `<user data>/enhancements/<disc_id>-60hz.json`, the disc ID in lowercase, for example `ulus12345-60hz.json`), or pass `ENHANCEMENT_PACKAGE=path/to/pkg-60hz.json` to `make`. Codegen resolves function names from the ELF symbol table or a `<package>.symbols.json` sidecar.
3. Run the built executable with `SR_EXPERIMENTAL_60HZ=1` for 60 Hz logic, or without it for the authentic 30 Hz game. The player does not yet select `packages/<disc_id>-enhanced-<pkg_id>` when launching a library entry.

### 9.5. Update Boundary: Gating Counts Logic Updates

A gated statement (a `gate_30hz` site, and the unit steps and countdowns whose rewrite is a gate) runs when the number of logic updates begun so far is even. The package says where an update begins with `update_boundary`: a symbolic `function`, a 4-byte-aligned `insn_offset` and the `stock_digest` of one instruction that runs exactly once per logic update, usually the first instruction of the title's per-frame body. While `SR_EXPERIMENTAL_60HZ=1`, that instruction first advances the counter `nk_enhancement_updates` and then runs its stock statement unchanged; with the toggle off nothing is counted.

1. **Required when anything gates.** The validator refuses a package with a `gate_30hz` site and no `update_boundary`, and codegen refuses one whose planned rewrites include any gate without it.
2. **Verified like a site.** The boundary's function resolves and its stock word is checked against `stock_digest` the same way, fail-closed. It may not be a control transfer or a rewritten site, and a package whose boundary is never emitted is refused like an unemitted site.
3. **Correct under frame drops.** The count advances once per update however many display VBLANKs that update takes, so gated statements keep alternating when an update overruns a VBLANK or a stock two-VBLANK wait still runs. `tools/test_enhancement_package.py` compiles the generated gate and runs updates of one to three VBLANKs to check it.
4. **Why it is declared, not inferred.** A `vblank_wait_2_to_1` site is the compare inside a title's wait loop, which can run several times in one update, so it does not mark where an update begins. The package names the boundary explicitly.

This package format is experimental and opt-in.

### 9.6. Data Sites and Data Files

Some frame-counted values are data, not instructions: tables in the executable's initialised data, and parameter files the title reads from its disc. A package rescales them once, when they are loaded, and only while `SR_EXPERIMENTAL_60HZ=1`. The scaling transforms of §9.2 apply (`count_x2`, `step_x0.5`, `probability_per_update_x0.5`, `decay_k_to_1-sqrt(1-k)`, `retention_m_to_sqrt(m)`); an integer value with no exact result, a float outside the range its transform needs, or an overflow refuses the package.

- **`data_sites`** name a symbolic data label (`symbol`, resolved like a site's function), a byte `offset` aligned to the element `type` (`u8`, `s8`, `u16`, `s16`, `u32`, `s32` or `f32`), an element `count`, the SHA-256 `stock_digest` of those bytes and a `transform`. Codegen reads the bytes from an allocated, non-executable `PROGBITS` section (never code or zero-fill), checks the digest and computes the rescaled bytes. The generated start-up (`nk_enhancement_apply_data()`, run from `sr_register_all()` after the executable is loaded and before it runs) compares the loaded bytes with the stock bytes and writes the rescaled ones; any difference stops the run with `ENHANCEMENT_DATA_MISMATCH`.
- **`data_files`** name a disc `path` (plain names separated by `/`, compared without case), the file's `sha256`, a `format` (`csv`), the zero-based `header_row` that names the columns, the `first_data_row`, and `cells`: a `column` by its header name, optional zero-based data `rows` (all data rows when left out) and a `transform`. A cell is a plain decimal number. An integer cell stays an integer and must rescale exactly; a decimal cell is read as a float, rescaled, and written as the shortest decimal that reads back to the same float. Build-package passes the title's extracted files (`--enhancement-data-root`); codegen finds the file by path and digest, rewrites it, and embeds the result. At run time `sr_enhancement_file_register()` holds it, and opening that path for reading reads the stock bytes first: matching bytes are served rewritten, with the rewritten size; any other bytes at a registered path stop the run with `ENHANCEMENT_DATA_MISMATCH`. Files on disc are never modified.

Both are covered by the package digest, so a change to either is a different enhanced build.
