# Enhancement Contract: Authentic Baseline, Capability Isolation, and Mod Boundaries

> **Status: CURRENT — maintained architectural contract.** Issue reference: #327. Build cache coordination coordinates with #316, and high-refresh presentation coordinates with #328.

Sections 1–7 define the authentic baseline, category boundaries, fail-closed package validation (`tools/enhancement_package.py`), and AOT cache epoch separation (`tools/nk_core/package_cache.py`, `tools/codegen.py`). Declarative frame-rate rescale packages (60 Hz logic rescaling for 30 Hz titles) are implemented on the AOT codegen and package-builder path; host runtime asset replacement and overlay loaders remain in the works. Section 10 covers the project-authored library-wide mode, and section 11 the class of its items that change what the guest can observe (#2055).

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
- **Upscaling:** `SR_PRESENT_UPSCALE=off|fsr1` (default `off`) enlarges the shown picture to the
  window with AMD FidelityFX Super Resolution 1 (EASU, then RCAS) in place of the bilinear or
  nearest filter. It reads the shown image and writes private images only; captures still hold
  the guest's frame (`gpu-capture-selftest`, run by hosted CI on lavapipe, presents through the
  real pass and checks both). A run recording, which holds the picture the window shows, is
  enlarged by its own FSR 1 pass at the recording's size and names the scaling it used in `record.json`
  (the same selftest compares it with the pass's output, byte for byte), and guest VRAM and every
  guest flight event match with it off (`display-smoke-fsr1`, which needs a Vulkan device and a
  display, so it is a local gate). In the player it is the Enhanced part "Sharper enlarging"
  (`"upscale"`, off by default) on Settings > Enhanced and each title's page. It is not part of
  Authentic: the launcher exports `SR_PRESENT_UPSCALE=off` for an authentic session whatever the
  setting or the inherited environment (section 10.1), and a run with it on says
  `ACE_MODE: present upscale fsr1`, emits a `phase=present_upscale` boot event and carries
  `"present_upscale": "fsr1"` in its flight bundle's `runtime.enhancements`, so neither
  `flight_diff.py` nor `boot_gate.py` takes it as authentic evidence. `picture_gate.py` and
  `library_sweep.py` run the runtime themselves, so they set the key `off` (a picture-gate route
  may ask for it) and record the value they used. GPU cost at
  4K: 0.40 ms per present for 1920x1088 to 3840x2160 on an RTX 3080 (`fsr1-selftest`).
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

- **Allowed:** Interpolating host rendering between consecutive guest frames; adjusting presentation swap intervals; multiplying display updates while keeping guest timer tick counts authentic; applying the declarative frame-rate rescale transforms of section 9.2 to verified symbolic sites during an explicit AOT enhancement epoch.
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
2. **Off at run time unless opted in.** Every rewritten site is emitted twice: the stock statement and its rewrite. The rewrite runs only when the process starts with `SR_EXPERIMENTAL_60HZ=1`; any other value runs the stock statements, so a package build with the toggle off executes the authentic instruction stream. The player sets it per title: "60 FPS game logic" on the title page, shown only for a title with a usable enhancement package, is off by default and saved as the list `enhancement_60hz_discs` in the player's settings file (no other key turns it on). A title with it on launches its enhanced package with `SR_EXPERIMENTAL_60HZ=1`; with it off, the player launches the authentic `packages/<disc_id>` with `SR_EXPERIMENTAL_60HZ=0`, even when an enhanced package exists. Started with `--experimental-60hz`, the player turns it on for every title that has an enhancement package and does not resolve to Authentic (section 10.5), for that session only; the flag is never written to the settings file.
3. **No faster guest clock.** System time, RTC and audio sample clocks are untouched. Only the sites the package lists change.

### 9.2. Site Transforms

Each `sites` entry names a symbolic `function`, a 4-byte-aligned `insn_offset`, the SHA-256 `stock_digest` of the stock little-endian instruction word, and a `transform`. `plan_site_rewrite()` in `tools/enhancement_package.py` gives each (transform, instruction) pair exactly one rewrite, chosen from the transform and the instruction encoding alone; a pair without one refuses the whole package. A site is one word, or two words (`word_count: 2`) that build a float constant: `lui rt, hi` followed by `ori rt, rt, lo` of the same register. A pair takes one of the scaling transforms below, applied to the whole 32-bit value and rounded to nearest, and both words are rewritten (`plan_float_pair_rewrite()`).

| Transform | Instructions with a defined rewrite | Rewrite |
| --- | --- | --- |
| `vblank_wait_2_to_1` | `addi`/`addiu`/`slti`/`sltiu`/`ori` with immediate 2 | immediate 2 becomes 1 |
| `gate_30hz` | `jal`; accumulating `addiu rt, rt, imm`, `addu`/`subu`/`add`/`sub` with the destination as a source, `add.s`/`sub.s` likewise | the statement runs only on even logic updates (§9.5), so every instance runs on the same alternate updates |
| `gate_30hz_hold` | `jal` | the call runs only on even logic updates, as `gate_30hz`; on a skipped update `f0` takes the value of `f12`. It is for a helper that returns its first float argument advanced by one update (an ease toward a target): a skipped update holds the value, so each 30 Hz value shows for two updates. The resolver refuses the site unless the callee, read from its entry to its `jr $ra` and that delay slot, makes no call, jump or trap, writes no memory, `$ra`, `$sp` or FPU control register, branches only to words up to that `jr $ra`, reads `f12` before it writes it, and writes `f0`: skipping it then drops nothing but its result. The check reads the body in address order, not along each path, and does not look at VFPU registers. The site's evidence shows the rest: that the result is stored back as the value the call advances, and that nothing reads the helper's other outputs |
| `count_x2` | positive `addi`/`addiu`/`slti`/`sltiu` immediate; `andi` mask $2^n-1$; `lui` of a normal float; `sll`; integer loads; `lwc1`/`swc1`; `sw` (only with `stock_values`, §9.6); `lv.s` (the loaded VFPU register is scaled); single-precision arithmetic and `cvt.s.w`; a unit countdown `addiu rt, rt, -1` | the produced value is doubled (a countdown is gated) |
| `count_x2_even` | `slti`/`sltiu rt, rs, n` with $0 < n \le$ `0x3FFF` and `rt` neither `$zero` nor `rs` | a bound on a counter of logic updates since an event: true while `rs` $< 2n$ and `rs` is even, so the $n$ events of the stock window still happen, one every second update, at the stock instants (`count_x2` would make $2n$ of them, twice as dense). Under `slti` a negative `rs` stays true, as in stock, so an inactive value such as $-1$ keeps its meaning; under `sltiu` it is a large unsigned value, false in both forms. The resolver refuses the site unless `rs` is loaded from memory (`lb`, `lbu`, `lh`, `lhu` or `lw`) within the 16 instructions before the compare, with no branch, jump or call in between, and not in the delay slot of a jump, a call or a branch-likely. The check reads backward in address order and does not see a branch from elsewhere into those instructions. The site's evidence shows the rest: that the stored count advances by one every update |
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

A gated statement (a `gate_30hz` or `gate_30hz_hold` site, and the unit steps and countdowns whose rewrite is a gate) runs when the number of logic updates begun so far is even. The package says where an update begins with `update_boundary`: a symbolic `function`, a 4-byte-aligned `insn_offset` and the `stock_digest` of one instruction that runs exactly once per logic update, usually the first instruction of the title's per-frame body. While `SR_EXPERIMENTAL_60HZ=1`, that instruction first advances the counter `nk_enhancement_updates` and then runs its stock statement unchanged; with the toggle off nothing is counted.

1. **Required when anything gates.** The validator refuses a package with a `gate_30hz` or `gate_30hz_hold` site and no `update_boundary`, and codegen refuses one whose planned rewrites include any gate without it.
2. **Verified like a site.** The boundary's function resolves and its stock word is checked against `stock_digest` the same way, fail-closed. It may not be a control transfer or a rewritten site, and a package whose boundary is never emitted is refused like an unemitted site.
3. **Correct under frame drops.** The count advances once per update however many display VBLANKs that update takes, so gated statements keep alternating when an update overruns a VBLANK or a stock two-VBLANK wait still runs. `tools/test_enhancement_package.py` compiles the generated gate and runs updates of one to three VBLANKs to check it.
4. **Slowdown is reported.** An update that takes longer than one VBLANK of host time is followed by the title's own wait for the next VBLANK, so the logic runs below 60 updates per second while every per-update step stays halved: the game plays slower than the original. The generated start-up hands `nk_enhancement_updates` to the runtime (`sr_enhancement_register_updates()`), which compares it once a second. Logic that runs but stays under 57 updates per second for three seconds in a row is logged as `ENHANCEMENT_LOGIC_SLOWED` and shown in the game window as a red `60 FPS logic: <n>/s - SLOWED` line, with or without the F3 HUD. A second with no update at all (a load or a pause) clears the count.
5. **Why it is declared, not inferred.** A `vblank_wait_2_to_1` site is the compare inside a title's wait loop, which can run several times in one update, so it does not mark where an update begins. The package names the boundary explicitly.

This package format is experimental and opt-in.

### 9.6. Data Sites and Data Files

Some frame-counted values are data, not instructions: tables in the executable's initialised data, and parameter files the title reads from its disc. A package rescales them once, when they are loaded, and only while `SR_EXPERIMENTAL_60HZ=1`. The scaling transforms of §9.2 apply (`count_x2`, `step_x0.5`, `probability_per_update_x0.5`, `decay_k_to_1-sqrt(1-k)`, `retention_m_to_sqrt(m)`); an integer value with no exact result, a float outside the range its transform needs, or an overflow refuses the package.

- **`data_sites`** name a symbolic data label (`symbol`, resolved like a site's function), a byte `offset` aligned to the element `type` (`u8`, `s8`, `u16`, `s16`, `u32`, `s32` or `f32`), an element `count`, the SHA-256 `stock_digest` of those bytes and a `transform`. Codegen reads the bytes from an allocated, non-executable `PROGBITS` section (never code or zero-fill), checks the digest and computes the rescaled bytes. The generated start-up (`nk_enhancement_apply_data()`, run from `sr_register_all()` after the executable is loaded and before it runs) compares the loaded bytes with the stock bytes and writes the rescaled ones; any difference stops the run with `ENHANCEMENT_DATA_MISMATCH`.
- **`data_files`** name a disc `path` (plain names separated by `/`, compared without case), the file's `sha256`, a `format` (`csv`), the zero-based `header_row` that names the columns, the `first_data_row`, and `cells`: a `column` by its header name, optional zero-based data `rows` (all data rows when left out) and a `transform`. A cell is a plain decimal number. An integer cell stays an integer and must rescale exactly; a decimal cell is read as a float, rescaled, and written as the shortest decimal that reads back to the same float. Build-package passes the title's extracted files (`--enhancement-data-root`); codegen finds the file by path and digest, rewrites it, and embeds the result. At run time `sr_enhancement_file_register()` holds it, and opening that path for reading reads the stock bytes first: matching bytes are served rewritten, with the rewritten size; any other bytes at a registered path stop the run with `ENHANCEMENT_DATA_MISMATCH`. Files on disc are never modified. Only an open by path is served: a title that reads its data through raw UMD sectors or its own archive reader never opens these paths, and their rewrites do not apply. With the other end-of-process reports, however the run ends (their order relative to `COMPAT_PATCH_REPORT` is not fixed), `ENHANCEMENT_DATA_REPORT: served=K/N` counts the registered files that were opened and served, and `ENHANCEMENT_DATA_NOT_SERVED` names each one that never was.

Both are covered by the package digest, so a change to either is a different enhanced build.

#### Load-time store sites

Some titles parse a table from disc at load time and store each field into RAM with its own instruction. Rescaling that store makes every later reader see the rescaled value, once per load, and it stays correct when the title loads again, because each load scales the value being stored. The store is the anchor: declare it as a `swc1` site (`count_x2`, `step_x0.5` or `probability_per_update_x0.5`) or as a `sw` site (`count_x2`), and give it `stock_values`.

- **`stock_values`** is a non-empty list of numbers, the values the loader is expected to store at that instruction. The package refuses a duplicate (after conversion to `f32` for a `swc1` site), a value that is not finite or whose scaled result is not finite, and, for a `sw` site, a value that is not an integer or whose double is outside the `s32` range. A `sw` site without `stock_values` is refused, because exactness and range are proven from the declared values. It is allowed only on `swc1` and `sw` sites.
- **Run time, toggle on.** The generated statement compares the 32-bit pattern of the stored register with the stock values. A value outside the list prints `ENHANCEMENT_DATA_MISMATCH: site <id> at 0x<pc> stores <value> (0x<bits>), not one of the package's stock values; the run stops here` to stderr and exits with status 1. A listed value is stored scaled and counts as one application: the first 16 applications of a site print `ENHANCEMENT_DATA_APPLIED: site <id> at 0x<pc>: <stock> -> <scaled>`, then one line `ENHANCEMENT_DATA_APPLIED: site <id>: later applications are not logged` follows. With the toggle off the statement is the stock one, with no check and no output.
- **A store site that never runs.** With the toggle on, when the process ends, each site with `stock_values` that stored no value prints `ENHANCEMENT_DATA_SITE_NOT_REACHED: site <id> at 0x<pc> stored no value in this run; whatever it should have rescaled stayed stock`: the title filled the field on another path, and its readers saw stock values. A run stopped by `ENHANCEMENT_DATA_MISMATCH` prints only the mismatch. The line is printed at an ordinary exit and on the runtime's `_Exit` paths (the headless end of run, a route failure, a capture exit, an unsupported import, a guest fault), through `sr_at_process_end()` in `src/rt/perf.h`. It is a report and leaves the exit status as it is: those statuses already mean something (a route result, a capture code), and the site may legitimately not have run yet when the run ends early. A tool that accepts a 60 Hz run checks for the line.
- **Choosing the store.** The site must be the last store of the value: after any clamp or range check the title applies to the field, or a scaled value the title clamps afterwards is lost without a line. If a later store cannot be ruled out, keep the scaled values inside the clamp.
- **A scaled value that is also a stock value.** The check compares the stored value with the stock list, so it cannot tell a stock value from an already scaled value that equals another stock value (stock values 30 and 60 with `count_x2`: a re-stored 60 that was a scaled 30 is scaled again to 120). The package is not refused, because one table can hold both v and 2v, but codegen prints `warning: enhancement site <id>: stock value <v> scales to <w>, which is also a stock value; ...` once per such value. The package author checks that no path stores an already scaled value again through that instruction.
- **`requires`** is a non-empty list of site ids, each another site of the package that declares `stock_values`. The site runs its rewrite only after every required site has applied at least once in the run; until then it runs its stock statement, and the first time that happens it prints `ENHANCEMENT_SITE_GATED: site <id> at 0x<pc> runs stock: <required id> has not applied` once. It works for every rewrite kind; on a gated call (`gate_30hz` or `gate_30hz_hold` on a `jal`) the stock statement is the call made on every update. The package refuses an unknown id, the site's own id, a duplicate, an id that names a data site or data file, and a required site that itself declares `requires`.
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
  text, the speed readout, the game badge, smooth motion, clean pitched sound and sharper enlarging (the last two off
  by default) today. Each one is a *part* that can be turned off for the library or for a single game.
- **Ace** is Enhanced plus the improvements made for that game alone. Today that is the title's 60 FPS game logic
  package (section 9). The player offers Ace only for a title with a usable package; its own parts can be turned off
  for that game too.

The parts are project-authored code in the player and the runtime (section 7), not a package: no loader, no AOT change,
no guest hook. Ace's per-title package is the one exception, and section 9 governs it. The runtime keeps the
development name: `src/rt/ace.h` serves both Enhanced and Ace runs, and the in-game UI is `src/rt/gpu_sdl3vk/ace_ui.h`.
Code identifiers that say `ace` mean "Enhanced or Ace"; every name a player reads says Enhanced or Ace.

### 10.1. The Launch Line

The launcher always exports `SR_ACE`, so an inherited value cannot opt a launch in: `off` for Authentic, or
`v1;tier=enhanced;af=16;text=on;readout=off;notice=on;wide=off;smooth=x2;sas=off`. `tier` (`enhanced` or `ace`) only names the
run, in the game badge, the pause menu and the stamps; a line without it is an Enhanced run. Every other key is
required, in any order, each once; a value for a feature this build does not carry (`wide=fill`, `smooth=match`)
refuses the line. So does anything else: the run is Authentic and one `ACE_MODE: refused (<reason>)` line says why.
Unset is off. With Enhanced or Ace on, the launcher also exports `SR_ACE_REPORT`, the per-run report under
`<user data>/ace/`. Ace's 60 FPS game logic is not on this line: it selects the title's enhanced package variant
(section 9.4). Nor is sharper enlarging (`"upscale"`, #2122): the launcher exports it as `SR_PRESENT_UPSCALE`, `fsr1`
for an Enhanced or Ace run with the part on and `off` otherwise, and always `off` for Authentic (section 2.2).

### 10.2. Authentic Is Untouched

With `SR_ACE=off` (the default: players opt in) every Ace branch sits behind one switch that stays false: the Vulkan
device is created with no features, every sampler and the texture descriptor pool are as before, the presenter
registers no UI pass, and the observation counters do not run. Nothing is printed, except one `TEX_SCALE: off`
line when the environment asks for texture scaling or the smart 2D filter, which names the request and does not obey
it (section 10.10), and one `TWIN: off` or `TWIN: refused` line when `SR_GPU_TWIN_COLOR` is set, which changes
nothing else (section 10.11). `gpu-coherence-selftest` checks the samplers and the pool; `display-smoke` keeps its
image hash.

### 10.3. What Ace Changes, and What It Never Changes

- **Never, for a host-only feature:** guest logic, the instruction trace, the generated code and the package key
  (`SR_ACE` is not a codegen option), the guest timing model (VBLANK schedule, GE timing, input sampling, media
  clocks). Every feature in this section is host-only except texture filtering (the "Pixels" bullet), which section
  11.1 names as already guest-visible; the other items are host-only unless section 11 admits them.
- **Host work:** the detector and the readout count flips, draws and host waits; their cost runs on the scheduler
  thread like any renderer cost.
- **Pixels:** texture filtering (anisotropic, for transformed draws with linear minification) changes framebuffer
  pixels, and every present commits the frame to guest VRAM, so a game that copies its screen (a save picture) keeps
  the filtered picture: the same class as a render scale above 1x, and the launcher says so. The launch notice, the
  speed readout and the crisp keyboard draw only on the swapchain image, never on the frame a capture reads, and so
  do Smooth motion's in-between frames (section 10.6).
- **Sound:** `sas=sinc` writes band-limited sample values into the guest's SAS output buffer, so it is a
  guest-visible item. Its entry is section 11.3.1.

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

A title's 60 FPS game logic and smooth motion may be on together (owner decision of 2026-11-04, replacing the earlier
rule that refused the pair, #1272): the package keeps the game's logic at 60 Hz, and interpolation presents
in-between frames above that rate. The two are separate switches. Smooth motion keeps its own key (`smooth`) and its
own library and title settings, is off by default, and stays off by default until the "nothing worse" evidence of
section 11.2, item 4 exists for the 60 FPS package together with the 60 Hz interpolator. The 60 FPS
package's own switch does not turn it on or off.

What stays refused is a second interpolator: one interpolation mode runs at a time, so `smooth=match` and `smooth=x2`
are never both on, and a launch line that names a mode this build does not carry is refused whole (section 10.1).

This build does not yet interpolate above 30 Hz. Smooth motion acts only on a title the detector holds at 30 (section
10.6); a title running 60 FPS game logic is presented frame by frame as it comes, as without the feature. The
interpolator for a 60 Hz source is its own item, with its own key, entry and evidence: a host-only feature under
section 10.6 unless it changes something section 11.1 lists, in which case it is a section 11 item. Until it
lands, the player still turns smooth motion off for a launch of the enhanced 60 FPS build, and the title page says
so. That is a limit of this build, not a rule of the contract; the item that lands the interpolator removes it and
updates the tests that pin it.

The session flag `--experimental-60hz` makes every title with a usable package that resolves to Enhanced or Ace run as
Ace for that session, with its 60 FPS game logic and `tier=ace`; a title that resolves to Authentic stays Authentic.

A part is one row of the player's part table (`src/player/player_state.c`): its key, its values (each part has an off
value, 0) and the words a row shows. The settings file, the launch line and both settings pages walk that table, so a
new part is one more row and no new switch. Ultrawide, the widescreen Fill mode (#1881), is not offered in this
build: `SR_ACE` still refuses `wide=fill`, and the line always carries `wide=off`. Its renderer layer is in place and off
by default (section 10.8).

### 10.6. Smooth Motion

With `smooth=x2`, a title the 30 Hz detector holds steady at 30 presents at the VBLANK rate. The code is
`src/rt/gpu_sdl3vk/ace_smooth.c`, its shaders and the pure pacing rule in `src/rt/ace.c`.

- **How a frame is made.** Block-matched motion between two private copies of consecutive presented frames, in a
  compute pass on the present's command buffer. It does not need #318, and it treats a GPU target and a CPU frame (a
  movie) alike.
- **What it reads of the GE.** For a GPU target, two host-only inputs (`src/rt/gpu_sdl3vk/ace_hud.c`):
  - **A HUD mask.** The boxes of the frame's through-mode (2D) draws onto that target, as one bit per guest pixel.
    A box that covers a quarter of the screen or more, or that samples a render target, is a backdrop or a post
    effect and is left out. A frame with more than 1024 boxes, or boxes covering more than two screens, has no mask.
    Masked pixels never steer the motion search. Beside the HUD, where the scene's own vector puts most of a block
    under it, the block keeps the cost the coarser search level measured for that vector. A vector whose end lands
    on the HUD in one frame takes only the other frame's side, judged by its block's cost, so the HUD is never
    dragged into the scene. A changed pixel on the HUD keeps the old frame's side.
  - **Depth.** A private R16 copy of the target's depth, taken only when a batch wrote that depth image since it
    was last handed on. A clear that clears depth counts as a write. Freshness belongs to the depth image, not to
    the colour target: when two display buffers share one depth buffer and a frame writes no depth, the depth still
    holds the other buffer's frame and is withheld. A frame without fresh depth has none, so its pair matches on
    colour alone. A vector whose two ends land at different depths fits worse: it joins two surfaces, not one that
    moved. The GE's depth image is only read and keeps its layout.

  Both are used only when both frames of a pair have them. Each present hands its inputs to the image it presents
  and to no other, and the show pass takes them once: a CPU frame (a movie) never gets a GPU frame's mask or depth,
  and inputs no show pass took are dropped and counted. `SR_ACE_SMOOTH_HUD=0` and `SR_ACE_SMOOTH_DEPTH=0` turn each
  off. There is no per-title switch yet. With both off, the in-betweens are the same bytes as without these inputs.
  The boxes are noted in the GPU GE's draw hooks after the draw is recorded, and the depth copy is recorded in the
  present's command buffer. So neither changes what the guest, the authentic renderer or a capture sees.

  Nothing is dropped silently. The report's `smooth_aux` block counts the pairs that used each input (`pairs`,
  `pairs_mask`, `pairs_depth`) and the depth copies refused (`depth_refused`). It counts the boxes kept, merged or
  left out, by reason (`boxes`: `kept`, `merged`, `large`, `target`, `outside`, `full`). It also counts frames:
  those with a mask, those that overflowed it, and those with depth, with unwritten depth or with no depth buffer.
  The last count is inputs no show pass took (`frames`: `mask`, `overflow`, `depth`, `depth_unwritten`,
  `depth_none`, `unshown`). The run's exit prints the same counts on one `ACE_MODE: smooth inputs` line. The first
  frame to overflow the mask prints a warning.
- **When.** The flip of frame B shows the in-between of A and B; the next VBLANK shows B. Nothing is shown before the
  guest made it, so a frame is never predicted. The cost is one VBLANK of added delay (about 17 ms), reported in the
  run's report as `added_delay_ms`. A flip that arrives first replaces the held frame.
- **Only at 30.** Every other detector class, a pause and a static picture show each frame as it comes, as without
  the feature. A title that runs 60 FPS game logic is such a class in this build (section 10.5). The detector enters
  30 Hz after a second of agreement and leaves after half a second of disagreement.
- **UI.** A pixel that did not change between the two frames is copied, so a HUD stays exact while the scene moves
  behind it.
- **Cuts.** When a third of the blocks fit nothing or disagree with their neighbours, the two frames are different
  pictures: the in-between is the old frame, and the new one follows on its VBLANK.
- **What it never touches.** The in-between and the held frame are private images blitted to the swapchain. The
  flip's own present still commits the real frame to guest VRAM and gives any capture the real frame. The VBLANK's
  present latches no input and serves no capture.
- **Stamps.** The report adds `presents` (real, between, held, superseded) and `present_interval_ms`. Section 10.4
  applies: the run is refused as evidence.

### 10.7. Band-Limited SAS Resampling

`sas=sinc` (#2054) resamples SAS VAG voices through a band-limited filter on the host. The mixed values land in guest
memory, so it is a guest-visible item, and its entry is section 11.3.1.

### 10.8. Widescreen Fill: the Renderer Layer

Fill shows more of the 3D scene to the left and right of the PSP's 480x272 picture, at the game's own vertical field of
view and pixel scale, and keeps 2D at its native shape in the centre. The GE has no x or y clip planes, so the
transformed positions of geometry past the screen edges already exist; the guest's scissor is what drops them. Fill
draws them again onto host-only images instead of widening any guest matrix. The code is in `src/rt/ge.c` (which
primitives reach the wings, and the software rasterizer's wings), `src/rt/gpu_sdl3vk/ge_gpu.c` (the Vulkan wings) and
`ge_wide_*` in `src/rt/ge_shared.h`.

- **The picture itself never changes.** Guest VRAM, depth, readbacks, captures, the snapshot path and GE time are those
  of Fill off. Each display framebuffer has a host-only wing canvas holding both wings, which no guest-memory path
  reaches, and a wing pass charges no GE time and counts in no guest or frame statistic. One route differs: the opt-in
  GPU T&L route (`SR_GPU_TNL=1`, off by default) declines while Fill is on, because the wings need ge.c's screen-space
  triangles, so with that switch set the picture is drawn by CPU T&L as it is without the switch.
- **What reaches the wings.** A transform-mode triangle or sprite that leaves the screen while it draws into a display
  framebuffer under a scissor as wide as the screen, shifted by whole pixels so it rasterizes as it would on a wider
  screen: a triangle after the 28.4 snap, a sprite after flooring its corners, so a corner at x = -10.5 lands in column
  D - 11. Of the through-mode (2D) draws, only clear-mode sprites that reach a screen edge (`sceGuClear`'s strips) and
  untextured whole-screen sprites (fades) are stretched across the wings; every other 2D draw stays in the centre. CPU
  writes (movies), block transfers, lines and points do not reach the wings. On Vulkan a primitive whose state the
  backend draws in software is dropped from the wings, counted, and named once on stderr (`gegpu: Fill dropped a wing
  primitive`).
- **Never a stale wing.** The first wing draw after a canvas was shown (`ge_wide_shown`) clears it, so a canvas holds
  the current frame's wings only, whatever the game clears with. `ge_wide_drawn` is true only when a wing primitive was
  really drawn since then; the presenter shows black wings otherwise.
- **Any window aspect wider than the PSP's.** The wing width comes from the window's shape at the PSP's 272 lines,
  rounded down to a multiple of 4: 21:9 (about 635x272) has 76-pixel wings and 32:9 (about 967x272) has 240-pixel
  wings. The PSP's 480x272 is 30:17, about 16:9; a 16:9 window widens it by under 4 pixels, so 16:9 has none. One canvas
  holds both wings in a 512-column image, the GE's widest framebuffer, so the Vulkan canvas shares every target's size,
  viewport and readback. That caps a wing at 256 pixels, a picture of 992x272 (about 32.8:9); a wider window (three
  screens) gets the widest Fill and black beyond it.
- **Off is the default.** With no wing width set, no canvas exists and no wing is drawn or submitted. What Off still
  costs is a test per primitive in `src/rt/ge.c` (`s_wide_d`), plus tests of an always-NULL pointer in the software
  rasterizer's pixel accessors (`s_wing`) and the Vulkan backend's batch path (`s_wing_dst`).
- **Tests.** `gpu-coherence-selftest` (case `wide-fill`, also at render scales 2 and 4) and
  `ge-cpu-profile-validate-selftest` run one list at 16:9, 21:9 and 32:9. Shapes with sub-pixel corners in and across
  each edge (flat and gouraud triangles, transform-mode sprites) are drawn a second time wholly on screen, and each wing
  pixel must equal, exactly and at every scale, the pixel of the same renderer's frame that a wider screen would show
  there. At render scale 1 the software and Vulkan wings must also agree with each other: byte for byte, except on a
  gouraud shape's rows, where each channel may differ by one step. The tests also check that all of guest VRAM is
  byte-identical with Fill on and off, that the GE clock is unchanged, that a narrower scissor reaches no wing, that a
  shown canvas holds only the next frame's wings, that a frame whose every wing primitive the Vulkan backend drops
  leaves the canvas undrawn and black, and that the wing hooks receive the guest's vertices snapped and shifted.

**Next layers.** Presentation composes the left wing, the picture and the right wing into one image the window's
shape, shows black wings for a frame that drew none, and turns on `wide=fill`. Then **layer 2**, per title: a game
that skips objects it considers off screen never submits them, so they appear at the edges of the wings. A declared
symbolic `guest_memory_code_hook` (section 2.3) that widens that title's culling extents, resolved through its title
profile and never by a raw address in a package (section 5), fixes that title, and the title's HUD stays inside the
PSP's picture at every aspect. That hook changes what the guest computes, so it is a section 11 item. The setting's
help text says so.

### 10.9. Settings

`settings.json` keeps the library's choice in `"enhanced"` and a sparse `"title_play"` map, every value by name:

```json
"enhanced": {"default": "enhanced", "af": "16", "text": "on", "readout": "off", "notice": "on", "smooth": "off",
            "sas": "off", "upscale": "off"},
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
the precedence, the off switches at both levels and this mapping; `test_play_mode_upscale_settings` checks sharper
enlarging (`"upscale"`: `fsr1` or `off`) and the upscaler it sets for the launch.

### 10.10. Texture Scaling and the Smart 2D Filter

Two features in the Vulkan GE renderer (#1273). Both are guest-visible (section 11.1) and are not admitted under
section 11.2: section 11.3.3 is their entry and says what the guest can observe, and this section says how they draw.
The code is `src/rt/gpu_sdl3vk/tex_scale.c` and its hook in `ge_gpu.c`'s texture cache; the scaler is xBRZ 1.9
(`third_party/xbrz/`, GPL-3.0).

- **Settings.** `SR_TEX_SCALE=off|2|3|4|5|6` (the xBRZ factor), `SR_SMART_2D=off|on` and `SR_TEX_SCALE_BUDGET_MB`
  (1 to 8192, default 512: the GPU memory the scaled copies may hold), read when the renderer starts. Both features
  take effect only in Ace Mode: in an authentic run they stay off whatever the environment says, and a request
  prints one `TEX_SCALE: off (texture scaling and the smart 2D filter need Ace Mode)` line. A value outside the list
  prints `TEX_SCALE: refused (<reason>)` and leaves both off. There is no launch-line key, library setting or
  per-title setting yet: they come in the launch-line pull request that follows the Ace settings work (#2074), and
  only that pull request admits the features (section 11.3.3).
- **What is scaled.** A texture the texture cache decodes, with one level (no mipmaps), after its palette lookup.
  The factor drops for a texture whose scaled side would pass 2048 texels, and a texture that would pass 2048 at 2x
  is not scaled. The image is padded by two texels, repeated or clamped by the texture's wrap mode, so a repeating
  texture tiles without a seam.
- **Never scaled.** A render target sampled as a texture: bound directly, it never enters the cache; read back
  through the cache, it is decoded unscaled; and a texture whose VRAM range meets any live render target's is not
  scaled.
- **Never stale.** A scaled copy records the cache entry's upload and the content hash it was made from, and is
  bound only while both still match. A texture the guest rewrites, or a CLUT texture drawn with another palette (a
  different cache key), is drawn unscaled until its own copy is ready, and never with the old one.
- **Never waited on.** Two worker threads scale; the renderer only queues the decoded texels and stores finished
  copies at a later lookup. The jobs, waiting, running or finished, are at most 64 and hold at most 128 MiB of host
  memory together (one job is always accepted, so the largest copy, 16 MiB, can be made). Storing a copy is an upload
  on the renderer thread, so at most 8 MiB of copies are stored per present, the first copy of a present always;
  the rest wait in the job table for the next present. Until its copy is stored, or with the job table full, a
  texture is drawn unscaled. Nothing is hidden or skipped.
- **Budget.** The copies hold at most `SR_TEX_SCALE_BUDGET_MB` of GPU memory. A copy that would not fit first frees
  copies that no draw has bound for 60 presents (one to two seconds), the longest idle first, until a quarter of the
  budget is spare besides the new copy; that drains the GE once, as a cache eviction does. A copy bound in those 60
  presents is never freed, so when the copies in use fill the budget, the new texture is drawn unscaled. It is
  offered again on a later draw, at most once per present and only when it could fit, and so is a texture whose
  copy was freed.
- **Sampling.** A copy is sampled on its own texel lattice: the fragment shader's PSP sample-point rule (nearest at
  the texel centre, linear on the 1/16 grid) runs at the copy's resolution, from the factor the draw carries in its
  flags.
- **One texel per pixel.** A draw that maps one texel to one pixel (the smart 2D test below, whatever the filter)
  binds the unscaled texture even when a copy is ready. xBRZ adds no detail at that size, and because it scales a
  whole atlas (a font page), a copy carries a neighbouring cell into a cell's edge: the marks LittleBigPlanet's text
  showed. Magnified and 3D draws keep the copy.
- **Smart 2D.** A textured draw that maps one texel to one pixel on each axis (either direction), axis-aligned,
  without perspective and with its edge on a texel boundary, samples nearest: a HUD, a font or a sprite stays crisp
  at a render scale above 1x. The map may differ from 1:1 by 1/64 texel over the draw's whole span, and the
  coordinate every pixel centre samples must lie on a texel centre or at most 1/32 texel above it, never more than
  1/65536 below (the test's own float rounding). The PSP's linear rule floors to a 1/16 step, so a centre even
  slightly below takes part of the texel before, while above it up to the next step reads the texel alone; the
  shader's rounding to 1/4096 is left whole for the host's interpolation error. A triangle must carry the same 1/w
  at all three vertices, bit for bit: the shader divides by the interpolated 1/w at each pixel, so any difference
  bends the map between the vertices. At 1x every draw the test accepts reads the same whole texel with either
  filter, so the frame is unchanged; `tex-scale-selftest` checks that against the shader's two rules, with its
  per-pixel division, over offsets and drifts on both sides of the lattice, and refuses the draws just below it.
  Every other draw, a 3D one included, keeps the game's filter.
- **Guest memory.** Neither feature writes guest memory, the CLUT or a texture source; texture readbacks and
  render-to-texture results are unchanged. What a present commits to guest VRAM does change, which makes both
  features guest-visible: section 11.3.3 is their entry, and this section covers only their presentation.
- **Stamps.** `TEX_SCALE: on (xbrz=<factor>, smart2d=on|off, budget_mb=<n>)` at start; at exit,
  `TEX_SCALE: draws (scaled_binds=, onetoone_unscaled=, scaled_uploads=, stale=, smart2d=, reclaimed=,
  budget_short=, upload_capped=)` and
  `TEX_SCALE: exit (scaled=, queue_full=, discarded=, budget_peak_mb=, worker_ms=)`.
  A run with either feature on also prints, at start, the line section 11.3.3 describes.
  Section 10.4 applies: the run is an Ace run.
- **A title it breaks.** A title whose fonts or pixel art suffer gets its off switch in its own package, never as
  per-title code in a generic file.
- **Hot Shots Tennis and LittleBigPlanet keep smart 2D off.** Both draw text from an anti-aliased glyph page, one
  texel to one pixel. Nearest sampling turns each glyph texel into a hard block at a render scale above 1x, so the
  filter makes their text worse, not crisper. The evidence is the before-and-after runs at render scale 4 recorded in
  #2092. `SR_SMART_2D` is off by default for every title, so nothing in the runtime names them. Their per-title
  setting comes with the launch-line pull request that follows #2074 (section 11.3.3).
- **Tests.** `tex-scale-selftest` checks the C port against goldens from the upstream release, the borders, the job
  queue, the settings (an authentic start with both variables set keeps both off) and the classifier, with refused
  draws at the edge of its tolerance. `tex-scale-gate-mutant` builds the selftest without the Ace gate and passes only
  when the settings cell fails for that reason. `gpu-texscale-selftest` runs the production path on a Vulkan device
  at render scales 1, 4 and 2, the budget's reclamation included; `gpu-texscale-mutant` builds a cache that binds a
  copy without its content check (state, serial and hash) and passes only when the rewrite cell fails for that
  reason.

### 10.11. Full 32-Bit Colour (the Host Twin)

The game's 16-bit render targets are presented in full 32-bit colour without dither (#2050), in the Vulkan GE renderer.
It is a presentation-only change (section 2.2): it changes only the image the host shows, never what the guest
draws, reads or waits for. The code is the host twin in `src/rt/gpu_sdl3vk/ge_gpu.c` (its header and the comment
at `s_twin`).

- **How.** Every render target and depth buffer gets a host-only twin at the user's render scale. Every write to the
  authentic image reaches its twin, in order: each render op is drawn a second time into the twin without dither or
  the 16-bit colour snap, and uploads, CPU-write patches, depth uploads, block transfers and destination snapshots
  are repeated on it. Twin passes sample twins, so a render-to-texture chain stays full colour. The presenter shows
  a twin only while it holds every write its image took; otherwise it shows the authentic image and re-seeds the twin.
- **Guest memory.** The authentic chain keeps the user's render scale, and nothing reads a twin back. Guest VRAM,
  every readback, every block transfer into RAM and the frame each present commits to guest VRAM are the twin-off
  run's at the same scale, byte for byte (`gpu-coherence-selftest` twin-scene, at scales 1, 2 and 4). Present-source
  captures follow the shown image, so they hold the twin's picture; `SR_GPU_TWIN_CAPTURE=authentic` shows and
  captures the authentic image instead.
- **What the guest can observe (section 11.1).** Each of the four items is unchanged:
  - VRAM bytes and other guest memory: unchanged, byte for byte (twin-scene, above).
  - Readbacks: unchanged. GE and display readbacks, block transfers into RAM and the committed frame read the
    authentic image, as with the twin off (twin-scene).
  - Timing the guest can measure: unchanged. The guest timing model (VBLANK schedule, GE timing, media clocks) does
    not follow host GPU work (section 10.3). The second draw of each render op, and the drain when a twin allocation
    fails, are host waits.
  - I/O speed: unchanged. The twin performs no guest I/O.
- **Default.** Off. It stays off by default until section 11.2 item 4's "nothing worse" runs pass: picture-gate or
  route runs on *Hot Shots Tennis* and *LittleBigPlanet*, and a library sweep, each with the twin on against the same
  run with it off. When the Ace launch line carries its key (`twin`, below), the key follows section 11.2 item 2.
- **Setting.** `SR_GPU_TWIN_COLOR=1` (full colour) or `native` (the twin with the authentic colour, which the identity
  self-tests use), read when the renderer starts. Unset, empty, `0` and `off` are off. Any other value prints
  `TWIN: refused (...)` and leaves it off. It takes effect only in Ace Mode: in an authentic run it stays off whatever
  the environment says, and a request prints one `TWIN: off (the host twin needs Ace Mode)` line. Until the Ace
  launch line carries its key (`twin`, beside `tex`, `s2d` and `glyphs`, with the per-title switches of section
  10.5, after #2074), the variable is the only way to turn it on, and it is off by default.
- **Cost.** Each render op is drawn twice at the render scale, and the twins use as much GPU memory as the targets.
- **Fail-closed.** A GPU without nearest blits of RGBA8 and D16 prints `TWIN: unavailable (...)` and runs without it.
  If any twin allocation fails during the run, one `TWIN: off for the rest of the run (...)` line is printed, every
  twin is freed, and the run continues exactly as a twin-off run at the user's scale (`twin-alloc-fail`).
- **Stamps.** `TWIN: on (colour=full|native, scale=<n>x)` at start, and `TWIN: exit (ops=, copies=, reseeds=,
  authentic_presents=, alloc_failures=)` at exit. Section 10.4 applies: the run is an Ace run, refused as evidence.

### 10.12. Texture Replacement

A user-supplied texture pack replaces guest textures in the Vulkan GE renderer (#1880), the way PPSSPP's texture
packs do. It is guest-visible (section 11.1) and not admitted under section 11.2: section 11.3.4 is its entry, and
this section says how it draws. The code is `src/rt/texrepl.c` (the pack reader and the hashes: no GPU, no guest
memory, no network) and its hook in `ge_gpu.c`'s scaled-copy path (`tex_xs_offer`, section 10.10).

- **Settings.** `SR_TEX_REPLACE=<folder>` names the pack and `SR_TEX_REPLACE_BUDGET_MB` (1 to 8192, default 256) bounds
  the host memory its decoded images may hold, both read when the renderer starts. It takes effect only in Ace Mode:
  in an authentic run the pack is not read whatever the environment says, and a request prints one
  `TEX_REPLACE: off (texture replacement needs Ace Mode)` line. A folder that cannot be read, or whose
  `textures.ini` is malformed (a line that is no comment, section or `key = value`, no `hash` type, an unknown one),
  prints `TEX_REPLACE: refused (<reason>)` and loads nothing: a half-read pack would replace some textures and not
  others in a way nobody asked for. There is no launch-line key, library setting or per-title setting yet; they come
  with the launch-line pull request that follows #2074, and only that pull request admits the feature
  (section 11.3.4).
- **The pack.** The project ships none and never downloads one: it is a folder the user made or took from a
  community, kept locally. A `textures.ini` is optional. `[options]` takes `hash = quick | xxh32 | xxh64` (required
  when the file exists; a folder with no ini reads as `quick`), `ignoreAddress`, `reduceHash`,
  `skipLastDXT1Blocks128x64` and `skipLastDXT1Blocks128x128`; `[hashes]` maps `<16 hex digits><8 hex digits>` to a
  file, and an empty value marks a texture to leave alone; `[hashranges]` and `[reducehashranges]` are read as
  PPSSPP reads them. A PNG named `<16 hex digits><8 hex digits>.png` in the folder's root needs no ini line.
  Everything else (the `[filtering]`, `[indexed]` and `[games]` sections, `saveIndexed`, `ignoreMipmap`, mip-level
  files, DDS, KTX2 and ZIM files, and any key or section this reader does not know) is reported, one `TEX_REPLACE:
  note:` line each, and not applied.
- **Identity.** A texture is identified as in PPSSPP: its address (30 bits) and size code in the high half of the
  key, its quick (or xxh) hash of the guest bytes in the low half, and for a palette format the palette's XXH32
  hash (seed 0xC0108888) folded into the key together with the CLUTFORMAT command word, opcode byte 0xC5
  included. The bytes hashed are PPSSPP's: the last CLUT load's size plus the format's index start position,
  capped at the largest load seen. A run in which the game has loaded no palette yet hashes the 16 or 256
  entries the texture can index, which a replayed capture is the only case of. Lookup tries, in PPSSPP's order, the exact key, the palette alone, the key with
  no data hash, the data hash with no address, and the address alone.
- **Which textures.** One level (no mip chain), not a render target, whose guest bytes the renderer can read
  directly. The image must be a whole factor from 2 to 6 of the texture, the same on both axes, and no side may
  pass 2048 texels; any other image is counted (`bad_factor`) and the texture is drawn as the game made it. Only PNG
  is read, and only level 0.
- **How it draws.** The image is stored as the entry's scaled copy: an image `factor` times the guest texture,
  sampled on its own texel lattice (section 10.10, "Sampling"). It is finished work, so no worker is involved, and
  it is normally stored in the draw that finds it. Storing goes through the collect step scaled copies use, which
  takes the oldest finished job first and stops at the per-present upload cap (section 10.10), so with xBRZ also on,
  or with several large images first drawn in one scene, a later image waits for a later present; until it is
  stored the texture is drawn as the game made it, never stale. A replacement is tried first; xBRZ scales a texture the
  pack leaves alone. A draw that maps one texel to one pixel keeps the replacement: the rule that binds the
  unscaled texture for xBRZ copies never applies to a pack's image, which the pack's author drew.
- **Never stale.** The copy records the cache entry's upload and the content hash it was made from, as scaled copies
  do. A texture the guest rewrites is looked up again: its new content is replaced by the image the pack lists for
  it, or scaled or drawn as made, and the old image is never drawn. Replaced content that returns is replaced again.
- **Budget.** Decoded images stay in host memory up to `SR_TEX_REPLACE_BUDGET_MB`, the least recently used dropped
  first and decoded again when next needed; an image the budget cannot hold is counted (`over_budget`) and its
  texture drawn as made. The copies share the GPU budget of `SR_TEX_SCALE_BUDGET_MB` (512 MiB by default) and its
  reclamation (section 10.10).
- **Guest memory and files.** Texture bytes are read through the same host view the cache's content hash reads;
  nothing is written to guest memory, the CLUT or a texture source. Files are read only from inside the pack folder
  (a name or `[hashes]` value that leaves it is refused); nothing is uploaded and no network connection is made.
- **Stamps.** `TEX_REPLACE: on (hash=, entries=, notes=, budget_mb=)` at start, followed by the line section 11.3.4
  describes and one `TEX_REPLACE: note:` line for each thing read and not applied (a file that cannot be read or decoded is
  noted at its first use, when it is found, not only counted), and at exit `TEX_REPLACE: exit
  (lookups=, hits=, misses=, left_alone=, queued=, bound_draws=, loaded=, load_failed=, over_budget=, evicted=,
  bad_factor=, cache_peak_mb=, decode_ms=)`. Section 10.4 applies: the run is an Ace run.
- **Not yet.** Decoding a PNG runs on the renderer thread the first time an image is needed (a worker thread, as
  scaling has, and DDS or KTX2 files come after); the tallest row a game draws is not tracked, so a 512-high texture
  is hashed whole; same-size replacement (factor 1), mip levels, `[filtering]` and `[indexed]`, a dump switch, ZIP
  packs and a player toggle or per-title pack folder are not done.
- **Tests.** `texrepl-selftest` checks the hashes against values taken from PPSSPP's own code and published xxHash
  vectors, the ini and file-name rules, the lookup order, palette identity (against PPSSPP's formula worked out
  separately, with a pack file named the way PPSSPP names it), malformed packs, path escapes and the
  budget, on synthetic packs. `gpu-texscale-selftest`'s `replace` cell runs the production path on a Vulkan device: a
  listed texture drawn from the pack's image from the draw that finds it, even with the 1:1 rule on, with
  `SR_TEX_REPLACE` alone and with xBRZ also on; a rewrite to unlisted
  content never drawn from the old image; the content returning replaced again; an image of no whole factor counted
  and not used; and an authentic start reading no pack.

## 11. Guest-Visible Items

Sections 10.3, 10.6, 10.8 and 10.11 describe host-only features: what the guest reads, in its registers, memory and timing, is the
same with them on or off. The owner's decision of 2026-11-04 (#2055) is that Enhanced does not have to stay fully
Authentic: a change that improves a game without making anything worse is allowed, even when the guest can observe
it. This section is the class for those changes. It adds to the classes above and removes none of them.

"Enhanced" here is the library-wide tier and "Ace" the per-title tier of #2055. In builds before that rename the
library-wide tier is the Ace Mode of section 10, and the identifiers keep `ace`. Enhanced applies to every game and
may offer switches to turn parts off; Ace is Enhanced plus extras for one title, overrides the library's Enhanced
choice when selected, and may also offer switches.

### 11.1. What Makes an Item Guest-Visible

An item is guest-visible when, with it on, any of the following differs from the same run with it off:

- the bytes of guest VRAM or any other guest memory the item writes;
- what a readback returns: a GE or display readback, a save picture, a screenshot the title takes of its own screen;
- timing the guest can measure: when a VBLANK, a DMA, a GE list or a media clock completes relative to the guest;
- the speed of I/O the guest performs: disc, Memory Stick or savedata reads and writes.

The item's contract entry (section 11.2) names each of these that applies. An item that changes none of them is
host-only and follows sections 10.2 to 10.6, 10.8 and 10.11, not this section. One existing feature already sits on this line:
texture filtering changes the frame that every present commits to guest VRAM, so a game that copies its screen keeps
the filtered picture (the "Pixels" bullet of section 10.3). It keeps that entry, and is not held to items 2 to 5 of
section 11.2 retroactively; a change to it is. A new item is not admitted by analogy with it. Render scale, below,
is the other existing feature on this line, with its own rule for changes.

A render scale above 1x (the player's Internal Render Resolution, not an Enhanced or Ace item) sits on the same line.
Its off switch is 1x, where the picture and every readback are the PSP's values. A change to how the renderer draws
above 1x is part of this render-scale item, not a new item or a new class, when it keeps 1x byte-identical and above
1x moves the picture toward the 1x picture. That is a claim about the picture, not about every pixel: the change's
pull request traces each device pixel that ends farther from its own 1x value and says why it does. Items 2 to 5 of
section 11.2 do not apply to such a change; its pull request still carries a test that fails without it and the
comparison runs that show it. A readback keeps the
top-left device pixel of each guest pixel, so the guest reads what the device drew there:

- Where the device image holds the PSP's own value, the readback returns it. That covers a target uploaded from
  guest memory, and a sprite sampled nearest at one texel per pixel in either direction, whatever its origin. Every
  device pixel of such a sprite shows the PSP's texel (`sprite-scaled` in `gpu-coherence-selftest`).
- Every other draw samples at each device pixel's own position. The readback returns the top-left device sample,
  which sits 0.5 - 0.5 / scale of a pixel before the PSP's pixel centre. At 4x that is 0.375 of a pixel: for a 1:1
  linear sprite or triangle the value moves by up to 0.375 of the step between neighbouring texels, and a minified
  nearest sprite can return a different texel. The exception is the left and top edge pixels of a clamped primitive
  (next bullet): there the top-left device sample lies in the clamped band, so the readback returns the PSP's value.
- At the edges of an axis-aligned through-mode primitive (a sprite, or a strip or quad drawn as screen-aligned
  rectangles, flipped or not), no device pixel samples outside the texture range the primitive's 1x pixel centres
  cover. Those centres follow the rasteriser's top-left rule. A device pixel in the outer part of an edge pixel shows
  that pixel's own sample, as the PSP does, instead of reaching past it (with repeat wrapping, into the texture's
  opposite edge), and is filtered with the primitive's own footprint. This change is part of the render-scale item
  above: 1x stays byte-identical, and above 1x it removes the edge bleed, moving the picture toward the 1x picture
  (`uv-edge-clamp` in `gpu-coherence-selftest`). A pixel that ends farther from its own 1x value comes from
  interpolation on the axis the clamp leaves free, or from content beneath the primitive; #2104 traces each one on
  its captures.
  What it does not do:
  - Rotated and sheared primitives, a strip half without both a horizontal and a vertical leg (a trapezoid), and
    transform-mode (3D) geometry are not clamped.
  - Where two clamped primitives meet with continuous texture coordinates, each keeps its edge pixel's sample over
    its outer band, so the seam shows the 1x step instead of a smooth upscaled blend across it.
  - A sprite that samples a render target at device resolution (a screen copy) is clamped the same way, so the copy's
    one-pixel border shows the guest-pixel sample instead of the upscaled detail.

At 1x the readback is the PSP's value.

### 11.2. What an Item Must Have

1. **Authentic stays byte-identical.** With the library choice or the title's choice at Authentic, a build behaves
   as section 10.2 describes: the item sits behind the one switch that stays false, the generated code, the package
   key, the guest's memory, its instruction trace and its timing are what they are without the item. The test that
   pins the item also runs with Authentic and compares against the item-less result byte for byte.
2. **Its own key and off switch.** The item has a key on the launch line (section 10.1), a library setting and a
   per-title setting, and each can turn the item off alone, without turning off the rest of Enhanced or Ace. A
   missing or unknown value reads as off, never as more permissive. A launch line that carries a key this build does
   not know is refused, as section 10.1 says.
3. **A contract entry that says what the guest can observe.** Each item is listed in this document, in its own
   subsection, with: its key; what it changes; for each of VRAM bytes, readbacks, timing and I/O speed, either
   "unchanged" or exactly what differs and by how much; the tier it belongs to (Enhanced for every game, or Ace for
   one title); and its default. An Ace-tier item is on only for a title whose Ace is selected, and Ace keeps every
   Enhanced off switch. An item without an entry is not admitted.
4. **"Nothing worse" is shown before it is on by default.** Until the evidence below exists, the item is off by
   default and a player may turn it on. To be on by default in Enhanced or Ace it needs all of:
   - a test that pins the new behaviour, in this repository, that fails without the item and passes with it;
   - private route or picture-gate runs (`tools/picture_gate.py` or the title's private route) on *Hot Shots Tennis*
     and on *LittleBigPlanet*, each compared with the same build and route with the item off, with no new failure;
   - for an item that applies to every game, a library sweep (`tools/library_sweep.py`) with the item on against the
     same sweep with it off, with no title that passed turning into a failure.

   The comparison runs are non-authentic evidence (section 3): they show the item made nothing worse and are not
   cited for PSP conformance. The pull request lists each run with its result and says which could not be run.
5. **A title where it breaks gets a per-title off switch in its package.** The switch is data in that title's own
   package (section 5), which the generic code reads as "this item is off here". Generic files
   contain no title ID, no address and no branch for it. When a regression is found for a title, the item stays
   admitted and the title's switch goes in with the fix; the item is not made title-aware.
6. **The evidence tools keep their answers.** An Enhanced or Ace run is stamped, labelled and refused as section 10.4
   says: `ACE_MODE: on (...)` carries the item's key and value, the flight recorder bundle lists it in
   `runtime.enhancements`, `tools/flight_diff.py` and `tools/boot_gate.py` refuse the run and the acceptance
   checkpoint refuses it. A guest-visible item never relaxes one of those checks, and none is changed to let an item
   through.

### 11.3. Where Items Are Listed

No guest-visible item is admitted by this section alone. Each lands in its own pull request with its entry added here
(`11.3.<n>`), following the order of section 11.2. This section is the template that entry is checked against.

#### 11.3.1. Band-Limited SAS Resampling (`sas=sinc`)

- **Key.** `sas=off` or `sas=sinc` on the launch line (section 10.1), a required key (#2054). A line without it, or
  with any other value, is refused, and the run is Authentic.
- **Tier and default.** Enhanced: it applies to every game, in Enhanced and Ace runs alike (section 10.5). The
  default is off. It is the part "Clean pitched sound" (`"sas"`, `sinc` or `off`) on the library's Settings >
  Enhanced page and on each game's own page (section 10.9), so the library and each title can turn it on or off
  alone (section 11.2, item 2), and the launch line carries `sas=sinc` or `sas=off`. It stays off by default until
  the "nothing worse" runs below exist.
- **The authentic path.** Without the item the mixer does not interpolate (a zero-order hold). Each output sample
  plays the decoded sample at the voice's whole position. A 12-bit fraction then advances by the pitch (0x1000 is one
  source sample per output sample). Above 0x1000 the voice skips samples with no low-pass filter, so it aliases.
  Below 0x1000 it repeats them, so it carries images of its source band. A step that runs past the end of a 28-sample
  VAG block lands on the next block's first sample, dropping the overshoot. This rule has been in the mixer since the
  initial public export. It is not a physical-PSP measurement of the SAS core.
- **What changes.** A VAG voice's output samples are a Kaiser-windowed sinc (24 zero crossings, beta 7) evaluated at
  the voice's exact position, with a DC gain of exactly 1. Above a pitch of 0x1000 the cutoff drops by the pitch
  ratio, so the source is low-passed below the output's Nyquist frequency before it is decimated. The code is in
  `src/rt/hle_audio.c`, next to the authentic mixer, and `src/rt/sas_resample.h`. PCM and noise voices, which ignore
  the pitch today, are mixed as before.
- **Guest memory.** VRAM bytes are unchanged. The sample values the SAS core writes into the guest's output buffer
  change: by the measurements below, the energy aliased above the output's Nyquist frequency falls by 25 to 30 dB,
  and the images above the source's Nyquist frequency by 78 to 81 dB. A title that reads that buffer back (a level
  meter, its own effect chain) sees those values. Every other guest-readable SAS result is unchanged: the voice's
  decode position, fraction, envelope, end of sample, end flags and the grain run the authentic statements in the
  authentic order.
- **Readbacks.** Unchanged. The item touches no GE or display readback, save picture or screenshot.
- **Timing.** Unchanged. The grain, the SAS calls' return values, the end flags and the envelope heights are the
  authentic ones in every grain, and data that ends mid-grain ends on the same sample. The filter costs host time
  inside the SAS call, like any host cost.
- **I/O speed.** Unchanged. The item performs no disc, Memory Stick or savedata access.
- **Reading ahead.** Samples ahead of the position come from a copy of the voice that decodes ahead of it, reading
  the guest's VAG data in memory. The voice itself never decodes ahead. The copy decodes at most one sample past the
  farthest sample the filter can read (one spare). Past the last block the authentic decoder has read by the end of
  the same grain, it decodes at most 1 VAG block (16 bytes) at a pitch of 0x1000 or below, rising to 4 blocks
  (64 bytes) at 0x4000. A title that streams VAG data into its buffer just in time may not have written those blocks
  yet. The copy then mixes the stale bytes into the output samples at the end of the grain whose filter window
  reaches them. Only the mixed values are affected. The copy is discarded, so voice state never changes.
- **Authentic stays byte-identical (section 11.2, item 1).** `hle-thread-selftest` plays a two-voice authentic run
  (a loop, a key-off, an end of data) that must match a checksum taken from the mixer before #2054. Main's mixer
  after the envelope change of #1296 gives the same checksum. A run switched on and then off gives it again, and a
  mutant that only rounds the authentic mix differently is caught by that checksum alone.
- **"Nothing worse" (section 11.2, item 4).** `hle-thread-selftest` plays synthetic VAG data through the guest
  calls, off and on, and fails without the item.
  - **Upward pitches.** A sweep lying wholly above the output's Nyquist frequency once pitched, so all of its
    output is alias. At pitch 0x1800 the alias energy falls by 25.5 dB (floor 22), at 0x2A00 by 28.1 dB (floor 25)
    and at 0x4000 by 29.8 dB (floor 26).
  - **Downward pitches.** An in-band sweep, measuring the share of energy above the source's Nyquist frequency at
    that pitch, where only images land. It falls by 78.4 dB at 0x0B00 (floor 75) and 80.7 dB at 0x0800 (floor 77).
  - **Negative control.** A model of the authentic stepping replays the same stream. It matches the mixer within
    0.5 dB. On that model, linear interpolation and a 4-tap cubic reach at most 2.9 dB upward and 20.9 dB downward,
    so they fail every floor.
  - **Other cells.** A looped voice mixes the same samples as its data unrolled. A constant signal comes out
    unchanged at three pitches. Two voices mix to the sum of each alone. A key-on mid-play restarts the voice like a
    fresh one.
  - **The read bound.** A selftest-only counter checks the lookahead's read count against its bound. Stale data
    just past that bound changes nothing, and data inside it reaches only the mixed values.
  - **Mutants.** Each was built from a scratch copy, run, and failed for its intended reason: linear interpolation
    or a 4-tap cubic in place of the sinc (the floors and the model match), a key-on that keeps the filter history
    (the key-on cell), history not kept across grains (the constant-signal cells and the image floors), the
    rounding change above (the checksum), and a lookahead one block longer (the read-bound counter).
  - **Not run.** No private route or picture-gate run on *Hot Shots Tennis* or *LittleBigPlanet* and no library
    sweep, with the item on against off. Until they exist the item stays off by default.
- **Per-title off switch (section 11.2, item 5).** None. No title is known to regress with the item on.
- **Evidence tools (section 11.2, item 6).** `ACE_MODE: on (...)` and the flight recorder's `runtime.enhancements`
  carry `sas=` with the rest of the launch line, and section 10.4's refusals apply to the run unchanged.

#### 11.3.2. The 60 FPS logic transforms `count_x2_even` and `gate_30hz_hold`

Every section 9 transform rewrites guest code, so what the guest computes differs with it on. These two are the
first section 9 transforms admitted under this section; a later section 9 transform is admitted the same way, with
its own entry.

- **Key.** The title's "60 FPS game logic" setting of section 9.1, item 2: the player's
  `enhancement_60hz_discs` list (or `--experimental-60hz` for the session), which launches the title's enhanced
  package with `SR_EXPERIMENTAL_60HZ=1`. A transform acts only at the sites the title's package names with it, so
  each title's package is that title's switch for each transform.
- **Off switch.** The same setting, per title: turning a title's 60 FPS game logic off launches its authentic
  package with `SR_EXPERIMENTAL_60HZ=0`, and any value other than `1` runs every stock statement. This switch is
  coarser than section 11.2, item 2 asks: it turns off every section 9 transform of the title's package together,
  not these two alone, and there is no launch-line key or library setting for them alone. A package leaves either
  transform out for its title by not naming it (the site keeps its stock instruction, or names `count_x2` or
  `gate_30hz` instead); that is the per-title data of section 11.2, item 5.
- **What it changes.** `count_x2_even` doubles a `slti`/`sltiu` bound on a counter of logic updates and reads false
  on a non-negative odd count, so the window's events happen at the stock instants. `gate_30hz_hold` skips a call to
  an ease helper on odd logic updates and gives `f0` the value of `f12`, so each 30 Hz value shows for two updates.
- **What the guest can observe.**
  - Guest registers and memory: at a `count_x2_even` site, the compare's `rt` is 0 on an odd count below twice the
    bound, where `count_x2` would give 1. At a skipped `gate_30hz_hold` call, `f0` is the held value and the
    callee's other result registers keep their earlier values; no memory differs, because the resolver refuses a
    callee that writes memory. Everything the logic computes from these values, and so what it draws, follows them,
    as for every site of a 60 Hz package.
  - VRAM bytes: no transform writes VRAM; the frames drawn follow the logic, as above.
  - Readbacks: unchanged; neither transform reads or writes a GE or display readback.
  - Timing the guest can measure: no VBLANK, DMA, GE list or media clock changes. An odd update does less work, by
    the skipped call.
  - I/O speed: unchanged.
- **Tier and default.** Ace: on only for a title whose package names the sites and whose 60 FPS game logic is
  selected. Off by default, as 60 FPS game logic is.
- **Authentic stays byte-identical.** Without `--enhancement-package`, codegen output and the cache key's codegen
  options are those of a build without this feature (section 9.1, item 1). With the package built in and the toggle
  off, every site runs its stock statement. `test_count_x2_even_and_gate_30hz_hold_match_30hz_when_run` in
  `tools/test_enhancement_package.py` builds a fixture with the package and without it, checks that each site's
  toggle-off statement is the Authentic statement byte for byte, runs the same harness on both builds, and checks
  that the toggle-off output equals the Authentic output byte for byte.
- **Sampled checking.** With `SR_EXPERIMENTAL_60HZ=1`, the in-play verifier
  ([sampled in-play checking](DEBUGGING.md#sampled-in-play-checking)) compares the enhanced code with the stock
  reference interpreter, so a sampled call whose body holds any enhancement site, these two included, reports a
  mismatch there; there is no enhancement skip class. Run it with the toggle off, or read such a mismatch as
  expected. A skipped `gate_30hz` or `gate_30hz_hold` call never reaches its return boundary, so it starts no check.
- **"Nothing worse".** The transforms replace `count_x2` and `gate_30hz` where those were wrong, and their tests pin
  that: an event window keeps its stock count and instants, and a held ease keeps its 30 Hz values. The private
  comparison runs of section 11.2, item 4 are recorded in the pull request that turns a title's 60 FPS game logic
  on by default, not here; until they exist it stays off by default, as it is today.

#### 11.3.3. Texture Scaling and the Smart 2D Filter (Not Yet Admitted)

The two features of section 10.10 (#1273). Their presentation is described there; this entry is what the guest can
observe.

- **Status: guest-visible, not admitted under section 11.2.** Each present commits the frame it shows to guest VRAM,
  so a frame drawn with a scaled copy, or with a 1:1 draw sampled nearest by smart 2D, is the frame the guest keeps.
  The features stay outside section 11.2 until the requirements below exist. Until then they are reachable only
  through environment variables, for development.
- **Key.** None yet. `SR_TEX_SCALE` and `SR_SMART_2D` are environment variables, not keys on the launch line (section
  10.1), and have no library or per-title setting. Those come in the launch-line pull request that follows the Ace
  settings work (#2074): it adds `tex`, `s2d` and `glyphs` (#2110) to `SR_ACE` in one change, with the library and
  per-title switches. This entry is completed in that pull request, and only then are the features admitted.
- **Never silent.** A run with either feature on prints, after its `TEX_SCALE: on` line, one line naming the features
  as guest-visible, on through the environment and not admitted under section 11.2. The `tex-scale-selftest`
  settings cell pins it.
- **What it changes:** the frame a present commits, wherever a draw used a scaled copy or smart 2D at a render scale
  above 1x.
- **VRAM bytes:** differ in those frames, at those draws' pixels.
- **Readbacks:** differ the same way, for a GE or display readback, save picture or screenshot that covers them.
  Texture readbacks and render-to-texture results are unchanged (section 10.10).
- **Timing:** differs. Scaling runs on worker threads, but storing a finished copy is an upload on the renderer
  thread, the thread the guest runs on, and a budget reclaim drains the GE once (section 10.10), so a guest frame can
  take longer. Measured at render scale 4 with `SR_TEX_SCALE=4 SR_SMART_2D=on` against the same build with both
  off, in 200-second attract runs before the per-present store bound: *Hot Shots Tennis* presented 94.1% of its
  frame intervals at one VBLANK against 98.9% (625 two-VBLANK intervals against 99); *LittleBigPlanet* 98.7%
  against 97.1%, no loss. Storing is now bounded to 8 MiB of copies per present, the first copy of a present
  always (up to 16 MiB); the bounded build's pacing has not been measured. The VBLANK schedule, the GE timing model
  and the media clocks are unchanged.
- **I/O speed:** unchanged.
- **Tier:** Enhanced. **Default:** off, and Ace-only: in an authentic run both stay off whatever the environment says.
- **Nothing worse (section 11.2, item 4):** not shown. *Hot Shots Tennis* and *LittleBigPlanet* keep smart 2D off
  (section 10.10), and neither feature may be on by default before the runs item 4 lists exist.

#### 11.3.4. Texture Replacement (Not Yet Admitted)

The feature of section 10.12 (#1880). Its presentation is described there; this entry is what the guest can observe.

- **Status: guest-visible, not admitted under section 11.2.** Each present commits the frame it shows to guest VRAM,
  so a frame drawn with a pack's image in place of a guest texture is the frame the guest keeps. The feature stays
  outside section 11.2 until the requirements below exist. Until then it is reachable only through environment
  variables, for development.
- **Key.** None yet. `SR_TEX_REPLACE` and `SR_TEX_REPLACE_BUDGET_MB` are environment variables, not keys on the launch
  line (section 10.1), and have no library or per-title setting. Those come with the launch-line pull request that
  follows the Ace settings work (#2074), and only then is the feature admitted. The pack itself is the user's own:
  the project ships none.
- **Never silent.** A run with a pack loaded prints, after its `TEX_REPLACE: on` line, one line naming the feature as
  guest-visible, on through the environment and not admitted under section 11.2.
- **What it changes:** the frame a present commits, wherever a draw used a pack's image.
- **VRAM bytes:** differ in those frames, at those draws' pixels.
- **Readbacks:** differ the same way, for a GE or display readback, save picture or screenshot that covers them.
  Texture readbacks and render-to-texture results are unchanged: a render target is never replaced.
- **Timing:** differs. The first use of an image decodes its PNG on the renderer thread, the thread the guest runs
  on, and storing the image is an upload there (section 10.10, "Never waited on", bounds it to 8 MiB per present
  for scaled copies; a replacement is normally stored in the draw that finds it, so a pack of large images can
  lengthen the frames that first draw them). Not measured on a title. The VBLANK schedule, the GE timing model and the media
  clocks are unchanged.
- **I/O speed:** unchanged for the guest. The host reads the pack's files from its own disk, inside the folder only.
- **Tier:** Enhanced. **Default:** off, and Ace-only: in an authentic run the pack is not read whatever the
  environment says.
- **Nothing worse (section 11.2, item 4):** not shown. A pack is the user's own content, so what it does to a title
  is the pack's; the runs item 4 lists, for a title and a pack, do not exist yet, and the feature may not be on by
  default before they do.
