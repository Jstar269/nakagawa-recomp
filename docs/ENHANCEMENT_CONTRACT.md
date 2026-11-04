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

A critical design requirement of Nakagawa Recomp is the complete absence of title-specific guest addresses and title-keyed behaviour from generic code (`TITLE_PROFILE_ARCHITECTURE.md`).

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
2. **Off at run time unless opted in.** Every rewritten site is emitted twice: the stock statement and its rewrite. The rewrite runs only when the process starts with `SR_EXPERIMENTAL_60HZ=1`; any other value runs the stock statements, so a package build with the toggle off executes the authentic instruction stream. The player sets it per title: "60 FPS game logic" on the title page, shown only for a title with a usable enhancement package, is off by default and saved as the list `enhancement_60hz_discs` in the player's settings file (no other key turns it on). A title with it on launches its enhanced package with `SR_EXPERIMENTAL_60HZ=1`; with it off, the player launches the authentic `packages/<disc_id>` with `SR_EXPERIMENTAL_60HZ=0`, even when an enhanced package exists. Started with `--experimental-60hz`, the player turns it on for every title that has an enhancement package, for that session only; the flag is never written to the settings file.
3. **No faster guest clock.** System time, RTC and audio sample clocks are untouched. Only the sites the package lists change.

### 9.2. Site Transforms

Each `sites` entry names a symbolic `function`, a 4-byte-aligned `insn_offset`, the SHA-256 `stock_digest` of the stock little-endian instruction word, and a `transform`. `plan_site_rewrite()` in `tools/enhancement_package.py` gives each (transform, instruction) pair exactly one rewrite, chosen from the transform and the instruction encoding alone; a pair without one refuses the whole package. A site is one word, or two words (`word_count: 2`) that build a float constant: `lui rt, hi` followed by `ori rt, rt, lo` of the same register. A pair takes one of the scaling transforms below, applied to the whole 32-bit value and rounded to nearest, and both words are rewritten (`plan_float_pair_rewrite()`).

| Transform | Instructions with a defined rewrite | Rewrite |
| --- | --- | --- |
| `vblank_wait_2_to_1` | `addi`/`addiu`/`slti`/`sltiu`/`ori` with immediate 2 | immediate 2 becomes 1 |
| `gate_30hz` | `jal`; accumulating `addiu rt, rt, imm`, `addu`/`subu`/`add`/`sub` with the destination as a source, `add.s`/`sub.s` likewise | the statement runs only on even logic updates (§9.5), so every instance runs on the same alternate updates |
| `count_x2` | positive `addi`/`addiu`/`slti`/`sltiu` immediate; `andi` mask $2^n-1$; `lui` of a normal float; `sll`; integer loads; `lwc1`/`swc1`; `sw` (only with `stock_values`, §9.6); `lv.s` (the loaded VFPU register is scaled); single-precision arithmetic and `cvt.s.w`; a unit countdown `addiu rt, rt, -1` | the produced value is doubled (a countdown is gated) |
| `step_x0.5`, `probability_per_update_x0.5` | as `count_x2` except integer loads, plus accumulating `+1`/`-1`/`addu`/`subu` updates | the produced value is halved (exactly, or by gating a unit or register step); `add.s fd, fd, ft` halves the addend `ft` |
| `decay_k_to_1-sqrt(1-k)` | `lui` of a float $k \in (0, 1)$; `lwc1` or `lv.s` of the factor | $k \to 1 - \sqrt{1-k}$, evaluated as $k / (1 + \sqrt{1-k})$ in double so that a small $k$ does not cancel to 0, and rounded to the upper half; after an `lwc1` or `lv.s` the loaded register is mapped at run time, and a value outside $(0, 1)$ keeps its stock value and is reported once per site (`ENHANCEMENT_FACTOR_KEPT`) |
| `retention_m_to_sqrt(m)` | `lui` of a float $m \in (0, 1)$; `lwc1` or `lv.s` of the factor | $m \to \sqrt{m}$, rounded to the upper half; after an `lwc1` or `lv.s` as for `decay_k_to_1-sqrt(1-k)` |
| `accel_x0.25` | `lui` of a float (or a `lui`/`ori` pair) | the per-update acceleration is divided by 4 (two exponent steps on a lone `lui`) |
| `launch_velocity_x0.5` | a `lui`/`ori` float pair with `acceleration_site` | the launch velocity $v_0 \to v_0/2 + g/8$, where $g$ is the stock value of the named `accel_x0.25` site |
| `offset_x2` | `addi`/`addiu` with a nonzero immediate, also when the destination is a source | an additive offset counted in updates (a lookahead, a delay added to a base) doubles: $n \to 2n$ |
| `window_start_x2` | `addi`/`addiu` with an immediate of $-2$ or less | the start of a window that ends one before a per-update counter: $k \to 2k+1$, so the window spans twice as many updates |
| `argument_x2` | the instruction at a function's entry (`insn_offset` 0), with `arguments` naming the registers (`a0`-`a3`, `t0`-`t3`) | a helper that takes durations in updates from every caller doubles them once per call, after its stock entry instruction, which may not write them; a function that branches back to its entry is refused |

A semi-implicit Euler motion (`v += g`, then `x += v`, once per update) reaches $x_n = n v_0 + g\,n(n+1)/2$. With $g/4$ per 60 Hz update and $v_0/2 + g/8$ at launch it reaches the same $x$ at every second update, which is what `accel_x0.25` and `launch_velocity_x0.5` declare together. The acceleration site must pin its whole float: a two-word site, or `low_insn_offset` and `low_stock_digest` naming the `ori` that completes a lone `lui` (read, never rewritten). A site with `operand: f64_high` is the `lui` of a double's upper word: `count_x2`, `step_x0.5`, `probability_per_update_x0.5` and `accel_x0.25` step its 11-bit exponent exactly, and other transforms refuse it.

The transforms scale the operand the site produces; they do not guess its role. A constant that multiplies or bounds a random roll (`rand() % 100`, `rand * 100.0f < p`) halves the per-update probability when it is doubled, so its site declares `count_x2`, not `probability_per_update_x0.5`. A `swc1` or `sw` site stores a scaled copy of its register and never writes the register, so a store that faults under `--lle-cpu` leaves it unchanged. A `sw` site is `count_x2` only, must declare `stock_values`, and may not store `$zero` or its own base register.

### 9.3. Random-Number Generator (RNG) Policy

Every frame-rate rescale package declares `rng_policy`. Under `rescale_probability`, single-roll per-update checks keep running every update with their probability halved; stateful multi-roll consumers are gated with `gate_30hz` so they keep their 30 Hz roll order. Under `subtick_30hz`, every roll site is gated.

### 9.4. Building and Running

1. Draft a package for a new title: `python tools/enhancement_package.py --draft <selected.elf> --title-id <library title_id> --out <disc_id>-60hz.json` reads the decrypted executable from the title's package cache and recognises its frame-pacing wait: a VBLANK counter that one routine increments by 1 (it reports whether that routine is registered as a VBLANK sub-interrupt handler, also through one wrapper), that the wait compares with `slti`/`sltiu rX, 2` and clears with `sw zero`; or `sceDisplayWaitVblankStartMulti(2)` and its CB form. Each becomes a `vblank_wait_2_to_1` site, with a `<package>.symbols.json` sidecar that names the routines `draft_fn_<address>`. The routines that call a pacing wait are printed as update-boundary candidates. The draft sets `update_boundary` only when there is exactly one, and that routine must still be confirmed to run once per logic update. Other VBLANK waits, for example two plain `sceDisplayWaitVblankStart` calls per frame, are printed for review, because no transform rewrites them. The draft validates but contains none of the title's frame-counted constants: those still come from the title's own code (sections 9.2 and 9.6).
2. Validate a package: `python tools/enhancement_package.py path/to/pkg-60hz.json`.
3. Build a package build: `python tools/nk_cli.py build-package <DISC_ID> --enhancement-package path/to/pkg-60hz.json` (or `--experimental-60hz` with the package at `<user data>/enhancements/<disc_id>-60hz.json`, the disc ID in lowercase, for example `ulus12345-60hz.json`), or pass `ENHANCEMENT_PACKAGE=path/to/pkg-60hz.json` to `make`. Codegen resolves function names from the ELF symbol table or a `<package>.symbols.json` sidecar.
4. Run the built executable with `SR_EXPERIMENTAL_60HZ=1` for 60 Hz logic, or without it for the authentic 30 Hz game. The player selects the package from the title's setting (section 9.1, item 2). A title has an enhancement package when `<user data>/enhancements/<disc_id>-60hz.json` (lowercase disc ID) parses, its `id` matches `^[a-z0-9][a-z0-9._-]{0,63}$` and its `target_title_id` is the title's own; the player reads only those fields and never forms a path from an id that fails the pattern. With the setting on, the player validates and launches `packages/<disc_id>-enhanced-<pkg_id>` with the same manifest, ABI and identity checks as the authentic package, and the package must record that enhancement package id in its codegen options; the authentic folder must record none. Each package has its own status: when the enhanced package is missing or stale, the title's primary action builds it (`build-package --experimental-60hz`) and nothing launches the authentic package in its place. When the setting is on but the enhancement package is gone, the title neither builds nor plays until it is turned off or the package is restored.

### 9.5. Update Boundary: Gating Counts Logic Updates

A gated statement (a `gate_30hz` site, and the unit steps and countdowns whose rewrite is a gate) runs when the number of logic updates begun so far is even. The package says where an update begins with `update_boundary`: a symbolic `function`, a 4-byte-aligned `insn_offset` and the `stock_digest` of one instruction that runs exactly once per logic update, usually the first instruction of the title's per-frame body. While `SR_EXPERIMENTAL_60HZ=1`, that instruction first advances the counter `nk_enhancement_updates` and then runs its stock statement unchanged; with the toggle off nothing is counted.

1. **Required when anything gates.** The validator refuses a package with a `gate_30hz` site and no `update_boundary`, and codegen refuses one whose planned rewrites include any gate without it.
2. **Verified like a site.** The boundary's function resolves and its stock word is checked against `stock_digest` the same way, fail-closed. It may not be a control transfer or a rewritten site, and a package whose boundary is never emitted is refused like an unemitted site.
3. **Correct under frame drops.** The count advances once per update however many display VBLANKs that update takes, so gated statements keep alternating when an update overruns a VBLANK or a stock two-VBLANK wait still runs. `tools/test_enhancement_package.py` compiles the generated gate and runs updates of one to three VBLANKs to check it.
4. **Slowdown is reported.** An update that takes longer than one VBLANK of host time is followed by the title's own wait for the next VBLANK, so the logic runs below 60 updates per second while every per-update step stays halved: the game plays slower than the original. The generated start-up hands `nk_enhancement_updates` to the runtime (`sr_enhancement_register_updates()`), which compares it once a second. Logic that runs but stays under 57 updates per second for three seconds in a row is logged as `ENHANCEMENT_LOGIC_SLOWED` and shown in the game window as a red `60 FPS logic: <n>/s - SLOWED` line, with or without the F3 HUD. A second with no update at all (a load or a pause) clears the count.
5. **Why it is declared, not inferred.** A `vblank_wait_2_to_1` site is the compare inside a title's wait loop, which can run several times in one update, so it does not mark where an update begins. The package names the boundary explicitly.

This package format is experimental and opt-in.

### 9.6. Data Sites and Data Files

Some frame-counted values are data, not instructions: tables in the executable's initialised data, and parameter files the title reads from its disc. A package rescales them once, when they are loaded, and only while `SR_EXPERIMENTAL_60HZ=1`. The scaling transforms of §9.2 apply (`count_x2`, `step_x0.5`, `probability_per_update_x0.5`, `decay_k_to_1-sqrt(1-k)`, `retention_m_to_sqrt(m)`); an integer value with no exact result, a float outside the range its transform needs, or an overflow refuses the package.

- **`data_sites`** name a symbolic data label (`symbol`, resolved like a site's function), a byte `offset` aligned to the element `type` (`u8`, `s8`, `u16`, `s16`, `u32`, `s32` or `f32`), an element `count`, the SHA-256 `stock_digest` of those bytes and a `transform`. Codegen reads the bytes from an allocated, non-executable `PROGBITS` section (never code or zero-fill), checks the digest and computes the rescaled bytes. The generated start-up (`nk_enhancement_apply_data()`, run from `sr_register_all()` after the executable is loaded and before it runs) compares the loaded bytes with the stock bytes and writes the rescaled ones; any difference stops the run with `ENHANCEMENT_DATA_MISMATCH`.
- **`data_files`** name a disc `path` (plain names separated by `/`, compared without case), the file's `sha256`, a `format` (`csv`), the zero-based `header_row` that names the columns, the `first_data_row`, and `cells`: a `column` by its header name, optional zero-based data `rows` (all data rows when left out) and a `transform`. A cell is a plain decimal number. An integer cell stays an integer and must rescale exactly; a decimal cell is read as a float, rescaled, and written as the shortest decimal that reads back to the same float. Build-package passes the title's extracted files (`--enhancement-data-root`); codegen finds the file by path and digest, rewrites it, and embeds the result. At run time `sr_enhancement_file_register()` holds it, and opening that path for reading reads the stock bytes first: matching bytes are served rewritten, with the rewritten size; any other bytes at a registered path stop the run with `ENHANCEMENT_DATA_MISMATCH`. Files on disc are never modified.

Both are covered by the package digest, so a change to either is a different enhanced build.

#### Load-time store sites

Some titles parse a table from disc at load time and store each field into RAM with its own instruction. Rescaling that store makes every later reader see the rescaled value, once per load, and it stays correct when the title loads again, because each load scales the value being stored. The store is the anchor: declare it as a `swc1` site (`count_x2`, `step_x0.5` or `probability_per_update_x0.5`) or as a `sw` site (`count_x2`), and give it `stock_values`.

- **`stock_values`** is a non-empty list of numbers, the values the loader is expected to store at that instruction. The package refuses a duplicate (after conversion to `f32` for a `swc1` site), a value that is not finite or whose scaled result is not finite, and, for a `sw` site, a value that is not an integer or whose double is outside the `s32` range. A `sw` site without `stock_values` is refused, because exactness and range are proven from the declared values. It is allowed only on `swc1` and `sw` sites.
- **Run time, toggle on.** The generated statement compares the 32-bit pattern of the stored register with the stock values. A value outside the list prints `ENHANCEMENT_DATA_MISMATCH: site <id> at 0x<pc> stores <value> (0x<bits>), not one of the package's stock values; the run stops here` to stderr and exits with status 1. A listed value is stored scaled and counts as one application: the first 16 applications of a site print `ENHANCEMENT_DATA_APPLIED: site <id> at 0x<pc>: <stock> -> <scaled>`, then one line `ENHANCEMENT_DATA_APPLIED: site <id>: later applications are not logged` follows. With the toggle off the statement is the stock one, with no check and no output.
- **A store site that never runs.** With the toggle on, when the process ends, each site with `stock_values` that stored no value prints `ENHANCEMENT_DATA_SITE_NOT_REACHED: site <id> at 0x<pc> stored no value in this run; whatever it should have rescaled stayed stock`: the title filled the field on another path, and its readers saw stock values. A run stopped by `ENHANCEMENT_DATA_MISMATCH` prints only the mismatch. The line is printed at an ordinary exit and on the runtime's `_Exit` paths (the headless end of run, a route failure, a capture exit, an unsupported import, a guest fault), through `sr_at_process_end()` in `src/rt/perf.h`. It is a report and leaves the exit status as it is: those statuses already mean something (a route result, a capture code), and the site may legitimately not have run yet when the run ends early. A tool that accepts a 60 Hz run checks for the line.
- **Choosing the store.** The site must be the last store of the value: after any clamp or range check the title applies to the field, or a scaled value the title clamps afterwards is lost without a line. If a later store cannot be ruled out, keep the scaled values inside the clamp.
- **A scaled value that is also a stock value.** The check compares the stored value with the stock list, so it cannot tell a stock value from an already scaled value that equals another stock value (stock values 30 and 60 with `count_x2`: a re-stored 60 that was a scaled 30 is scaled again to 120). The package is not refused, because one table can hold both v and 2v, but codegen prints `warning: enhancement site <id>: stock value <v> scales to <w>, which is also a stock value; ...` once per such value. The package author checks that no path stores an already scaled value again through that instruction.
- **`requires`** is a non-empty list of site ids, each another site of the package that declares `stock_values`. The site runs its rewrite only after every required site has applied at least once in the run; until then it runs its stock statement, and the first time that happens it prints `ENHANCEMENT_SITE_GATED: site <id> at 0x<pc> runs stock: <required id> has not applied` once. It works for every rewrite kind. The package refuses an unknown id, the site's own id, a duplicate, an id that names a data site or data file, and a required site that itself declares `requires`.
- **"In the run" means the process.** Application counts and the gate are kept for the life of the process, including after an in-process `LoadExec`: a site that applied before it still counts as applied.

### 9.7. Side-by-Side Timing Check

A package is right when the enhanced game does in each second what the authentic game does in that second. The check compares two runs of the same input route, sampled with `SR_SAMPLE_SPEC` ([sampling guest state per vblank](DEBUGGING.md#sampling-guest-state-per-vblank)): one of the authentic build, one of the enhanced build with `SR_EXPERIMENTAL_60HZ=1`. The spec must sample the title's logic-update counter; it may also sample a word that names the game phase (a mode or screen) and the values whose changes are the events to compare.

```text
python tools/enhancement_package.py --compare-rates stock/samples.csv enhanced/samples.csv \
    --phase mode --event count:points --event change:ai_choice --event lcg:seed:<A>:<C>=2 --json report.json
```

1. **Game time, not host time.** Each run's game time is its logic updates divided by its nominal rate (`--stock-hz 30`, `--enhanced-hz 60`). An enhanced run that falls below 60 updates per second still plays every update in half the game time, so its event rates stay comparable on a loaded host. The wall-clock rate is reported per phase as well, and a phase under 57 updates per second is marked `SLOWED` (the threshold of §9.5, item 4).
2. **Events.** `count:COL` sums the increases of a counter; `change:COL` counts the samples whose value differs from the one before; `lcg:COL:A:C[:BITS]` counts the steps a linear congruential generator $x' = (Ax + C) \bmod 2^{\text{BITS}}$ took between two samples, searching at most `--lcg-limit` steps (4096) and reporting a pair it cannot join as unresolved. `=RATIO` gives the expected enhanced/stock rate per game second, 1 unless given: under `rescale_probability` (§9.3) a generator rolled once per update advances twice as often, so its expected ratio is 2, while the events its rolls decide keep the ratio 1.
3. **A verdict from the counts.** For each event, the enhanced share of the two runs' events is binomial for a given true ratio; the 99 % Wilson interval of that share gives an interval for the ratio. `PASS` means the expected ratio lies inside it, `FAIL` that it does not. With fewer than 20 events in the two runs together the event is `NOT_JUDGED`: a rare event is not a pass. The interval assumes independent events; scripted events that come at fixed times vary less than that, so for them it is wider than it needs to be and can pass a small difference it could have caught.
4. **Per phase.** With `--phase`, each phase is judged as well as the whole run, and a `FAIL` in any phase fails the run: a phase whose rate is wrong must not be hidden by the totals. A phase with too few events to judge adds them to the whole run and is listed as not judged. A phase that runs logic in one run only means the runs did not follow the same route, and makes the result `INCOMPLETE`; a phase that runs no logic (a loading screen) has no game time and may appear in one run only.
5. **The captures.** The sampler can write one vblank twice: a row that repeats the previous row exactly is skipped and counted. Anything else that would lose time refuses the file (exit 1): a vblank that is not an integer, does not increase, or repeats with different values; a row without the updates counter; a cell that is not an integer in a counted column. An empty event cell (a value the spec could not read, such as a pointer chain before its object exists) leaves that step out of the event's count and out of its game time, and the number of such steps is reported. The shorter run must cover at least `--min-coverage` (0.9) of the longer run's game time, or the result is `INCOMPLETE`: a truncated capture is not a pass.
6. **Exit status.** 1 when an event fails in the whole run or in any phase, an LCG step is unresolved, or an input is refused (with a `rates: FAIL:` line naming it); 2 (`INCOMPLETE`) when an event is not judged over the whole run, no event is given, a phase is `SLOWED` or in one run only, or the coverage is too low; 0 otherwise. Each phase's events are also printed for diagnosis, and `--json` writes the measurements and the report.

Sample specs and routes name guest addresses, so they stay private with the package; the check itself is generic.

## 10. Play Modes: Authentic, Enhanced and Ace

The player offers three play modes per game (#2055):

- **Authentic** is the standard: the game as released, recreating the PSP as closely as the project can. It is the
  default for a new library.
- **Enhanced** turns on the project-authored improvements that work for any game: texture filtering, crisp system
  text, the speed readout, the game badge and smooth motion today. Each one is a *part* that can be turned off for
  the library or for a single game.
- **Ace** is Enhanced plus the improvements made for that game alone. Today that is the title's 60 FPS game logic
  package (section 9). The player offers Ace only for a title with a usable package; its own parts can be turned off
  for that game too.

The parts are project-authored code in the player and the runtime (section 7), not a package: no loader, no AOT change,
no guest hook. Ace's per-title package is the one exception, and section 9 governs it. The runtime keeps the
development name: `src/rt/ace.h` serves both Enhanced and Ace runs, and the in-game UI is `src/rt/gpu_sdl3vk/ace_ui.h`.
Code identifiers that say `ace` mean "Enhanced or Ace"; every name a player reads says Enhanced or Ace.

### 10.1. The Launch Line

The launcher always exports `SR_ACE`, so an inherited value cannot opt a launch in: `off` for Authentic, or
`v1;tier=enhanced;af=16;text=on;readout=off;notice=on;wide=off;smooth=x2`. `tier` (`enhanced` or `ace`) only names the
run, in the game badge, the pause menu and the stamps; a line without it is an Enhanced run. Every other key is
required, in any order, each once; a value for a feature this build does not carry (`wide=fill`, `smooth=match`)
refuses the line. So does anything else: the run is Authentic and one `ACE_MODE: refused (<reason>)` line says why.
Unset is off. With Enhanced or Ace on, the launcher also exports `SR_ACE_REPORT`, the per-run report under
`<user data>/ace/`. Ace's 60 FPS game logic is not on this line: it selects the title's enhanced package variant
(section 9.4).

### 10.2. Authentic Is Untouched

With `SR_ACE=off` (the default: players opt in) every Ace branch sits behind one switch that stays false: the Vulkan
device is created with no features, every sampler and the texture descriptor pool are as before, the presenter
registers no UI pass, the observation counters do not run, and nothing is printed. `gpu-coherence-selftest` checks
the samplers and the pool; `display-smoke` keeps its image hash.

### 10.3. What Ace Changes, and What It Never Changes

- **Never:** guest logic, the instruction trace, the generated code and the package key (`SR_ACE` is not a codegen
  option), the guest timing model (VBLANK schedule, GE timing, input sampling, media clocks).
- **Host work:** the detector and the readout count flips, draws and host waits; their cost runs on the scheduler
  thread like any renderer cost.
- **Pixels:** texture filtering (anisotropic, for transformed draws with linear minification) changes framebuffer
  pixels, and every present commits the frame to guest VRAM, so a game that copies its screen (a save picture) keeps
  the filtered picture: the same class as a render scale above 1x, and the launcher says so. The launch notice, the
  speed readout and the crisp keyboard draw only on the swapchain image, never on the frame a capture reads, and so
  do Smooth motion's in-between frames (section 10.6).

### 10.4. Stamps and Evidence

An Enhanced or Ace run prints `ACE_MODE: on (tier=..., ...)` at start and `ACE_MODE: exit (...)` at an ordinary exit,
and its flight recorder bundle carries `runtime.enhancements` (`assets/flight_recorder_schema.json`); an Authentic
bundle's bytes are unchanged. `tools/flight_diff.py` refuses to compare any bundle that carries the block,
`tools/boot_gate.py` refuses an Enhanced or Ace run's log, the acceptance checkpoint refuses both
(`reason=ace_mode`), and the launcher never starts either for it.

### 10.5. Per-Title Choice and Precedence

The library's default is Authentic or Enhanced. Ace is never a library default: it depends on a package only some titles
have, and a player turns it on title by title where they want it. Each title follows the library or chooses one of the
three modes for itself (its page's Play mode button, or Settings > Enhanced opened from its page). What a title launches
with resolves in this order:

1. **The mode.** The title's own choice, else the library's default. Authentic turns everything off, its 60 FPS game
   logic included, and the line is `off`.
2. **Each Enhanced part, for Enhanced and Ace alike.** The title's own value where it has one, else the library's. So
   Ace includes Enhanced, with the title's own choices first.
3. **Ace's parts.** The title's 60 FPS game logic is on unless the title turned it off. A title whose package has gone
   is blocked, not quietly played without it, until the package is back or the part or Ace is turned off.

A title that launches its 60 FPS game logic gets `smooth=off` on its line, whatever the settings say: logic and
interpolation are refused together, not stacked (#1272).

A part is one row of the player's part table (`src/player/player_state.c`): its key, its values (each part has an off
value, 0) and the words a row shows. The settings file, the launch line and both settings pages walk that table, so a
new part is one more row and no new switch. Ultrawide (a host-only wide mirror of the GE's draws) is designed and
tracked in #1272; it is not part of this build, and the line always carries `wide=off`.

### 10.6. Smooth Motion

With `smooth=x2`, a title the 30 Hz detector holds steady at 30 presents at the VBLANK rate. The code is
`src/rt/gpu_sdl3vk/ace_smooth.c`, its shaders and the pure pacing rule in `src/rt/ace.c`.

- **How a frame is made.** Block-matched motion between two private copies of consecutive presented frames, in a
  compute pass on the present's command buffer. It reads nothing of the GE, so it does not need #318, and it treats
  a GPU target and a CPU frame (a movie) alike.
- **When.** The flip of frame B shows the in-between of A and B; the next VBLANK shows B. Nothing is shown before the
  guest made it, so a frame is never predicted. The cost is one VBLANK of added delay (about 17 ms), reported in the
  run's report as `added_delay_ms`. A flip that arrives first replaces the held frame.
- **Only at 30.** Every other detector class, a pause and a static picture show each frame as it comes, as without
  the feature. The detector enters 30 Hz after a second of agreement and leaves after half a second of disagreement.
- **UI.** A pixel that did not change between the two frames is copied, so a HUD stays exact while the scene moves
  behind it.
- **Cuts.** When a third of the blocks fit nothing or disagree with their neighbours, the two frames are different
  pictures: the in-between is the old frame, and the new one follows on its VBLANK.
- **What it never touches.** The in-between and the held frame are private images blitted to the swapchain. The
  flip's own present still commits the real frame to guest VRAM and gives any capture the real frame. The VBLANK's
  present latches no input and serves no capture.
- **Stamps.** The report adds `presents` (real, between, held, superseded) and `present_interval_ms`. Section 10.4
  applies: the run is refused as evidence.

### 10.7. Settings

`settings.json` keeps the library's choice in `"enhanced"` and a sparse `"title_play"` map, every value by name:

```json
"enhanced": {"default": "enhanced", "af": "16", "text": "on", "readout": "off", "notice": "on", "smooth": "off"},
"title_play": {"ULUS10000": {"mode": "ace", "af": "8"}, "ULES00000": {"mode": "library", "logic_60": "off"}}
```

A title's `mode` is `library`, `authentic`, `enhanced` or `ace`; a part it does not name follows the library; its
`"logic_60": "off"` turns Ace's 60 FPS game logic off. An entry that follows the library everywhere is not written.
Reading never makes a choice more permissive: in an `"enhanced"` block, a part it does not name or a word this build
does not know is off, and a default other than `enhanced` is Authentic; a title's unknown mode follows the library,
and an unknown part value is off. A file with no `"enhanced"` block starts from the fresh defaults: Authentic, with
16x filtering, crisp text and the game badge set for when the player turns Enhanced on.

Development builds after v0.0.1 saved `"ace"`, `"ace_titles"` and `"enhancement_60hz_discs"`. The player reads them only
when `"enhanced"` is absent, maps them as below, and writes the new keys from then on:

- `"ace"`: `"enabled": true` is an Enhanced library default, `false` Authentic; its feature values become the library's
  parts (a value those builds did not know keeps the fresh default, as they did).
- `"ace_titles"`: mode `ace` is Enhanced, `authentic` Authentic, anything else follows the library; the title's
  feature values become its parts.
- `"enhancement_60hz_discs"`: each disc becomes plain Ace: every Enhanced part as the library has it (the documented
  defaults unless the old `"ace"` block changed them), any value of its own from `"ace_titles"` kept, plus its 60 FPS
  game logic. Its parts can be turned off one by one afterwards. A disc chosen Authentic stays Authentic: its 60 FPS
  game logic was not in effect while Authentic was chosen.

`tests/native/test_player_state.c` (`test_play_mode_settings`, `test_play_mode_settings_mapping`) checks the names,
the precedence, the off switches at both levels and this mapping.
