# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: the public target list and the `make help` descriptions.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# Public targets are listed once so `make help` and phony-target behaviour cannot
# drift apart.  FORCE is intentionally separate: it is an implementation detail,
# not an entry-point a contributor should discover by accident.
PUBLIC_TARGETS := \
	help \
	check \
	contrib-check \
	contrib-refresh \
	test \
	native-core-tests \
	player-ui-tests \
	player-ui-regressions \
	fuzz-parsers \
	readiness \
	provenance-refresh \
	tree-lint \
	all \
	pipeline \
	compile \
	compiler-info \
	runtime-objects \
	portable-core-objects \
	format-portability-check \
	atrac3p-objects \
	player \
	public-safe-verify \
	production-smoke \
	production-smoke-staged \
	production-smoke-clean \
	production-smoke-gap \
	perf-benchmark \
	showcase \
	showcase-smoke \
	showcase-linux \
	display-smoke \
	display-smoke-run \
	display-smoke-picture-gate \
	display-smoke-gui \
	display-smoke-fsr1 \
	display-smoke-quit \
	display-smoke-player \
	display-smoke-clean \
	title-qualification \
	title-qualification-smoke \
	production-smoke-gap-clean \
	platform-ladder \
	platform-ladder-zero \
	platform-ladder-reloc \
	platform-ladder-gap \
	platform-ladder-sched \
	platform-ladder-fpu \
	platform-ladder-fs \
	platform-ladder-fs-negative \
	platform-ladder-title2 \
	platform-ladder-title2-negative \
	platform-ladder-overlay \
	platform-ladder-clean \
	profile-zero-e2e \
	cosim-selftest \
	cosim-selftest-split \
	cosim-selftest-opt \
	cosim-selftest-promoted \
	cosim-selftest-run \
	cosim-selftest-clean \
	cosim-mutants \
	cosim-selftest-llvm \
	cosim-mutants-llvm \
	snapshot-selftest \
	snapshot-selftest-run \
	verify-selftest \
	verify-selftest-run \
	clean \
	clean-fixtures \
	tidy \
	distclean \
	clean-all \
	clean-preview \
	verify \
	selftest \
	strbuf-selftest \
	sched-selftest \
	sched-selftest-one \
	selftest-repeat \
	heap-selftest \
	profiler-selftest \
	gui-present-selftest \
	clay-layout-selftest \
	ed25519-verify-selftest \
	nk-ui-core-selftest \
	nk-ui-player-screens-selftest \
	nk-ui-player-screens-capture \
	nk-ui-overlay-screens-selftest \
	nk-ui-overlay-screens-capture \
	nk-ui-overlay-host-selftest \
	nk-ui-sdlr-selftest \
	nk-ui-vk-selftest \
	nk-ui-focus-selftest \
	coro-selftest \
	coro-selftest-asm \
	hle-thread-selftest \
	aot-return-boundary-thread-selftest \
	hle-thread-selftest-build \
	hle-title-selftest \
	hle-title-selftest-one \
	dispatch-selftest \
	stale-code-selftest \
	cpu-lle-selftest \
	domain-mode-selftest \
	dispatch-isolation-selftest \
	dispatch-isolation-selftest-one \
	memory-watch-selftest \
	code-capture-selftest \
	asset-index-selftest \
	fp-convert-selftest \
	ge-float24-selftest \
	ge-cpu-profile-validate-selftest \
	ge-flight-overhead \
	vfpu-tables-selftest \
	watchpoints-file-selftest \
	ge-raster-ref-selftest \
	vfpu-interp-selftest \
	atrac3p-selftest \
	atrac3p-bridge-selftest \
	ge-texture-ref-selftest \
	fbcap-selftest \
	videorec-selftest \
	osk-text-entry-selftest \
	osk-overlay-selftest \
	ace-selftest \
	ace-smooth-selftest \
	fsr1-selftest \
	ace-ui-selftest \
	motion-match-selftest \
	osk-overlay-paint-selftest \
	psmf-producer-selftest \
	psmf-media-selftest \
	psmf-media-selftest-csc-mutant \
	psmf-media-focused-selftest \
	audio-selftest \
	audio-resampler-bench \
	atrac3p-title-accept \
	gpu-coherence-selftest \
	gpu-coherence-selftest-snapbar-mutants \
	gpu-snapsync-selftest \
	gpu-ace-selftest \
	tex-scale-selftest \
	tex-scale-gate-mutant \
	texrepl-selftest \
	texrepl-mutant \
	gpu-texscale-selftest \
	gpu-texscale-mutant \
	gpu-texrepl-mutant \
	ge-replay \
	ge-first-bad-draw-selftest \
	run \
	run_elf \
	vfpu_fuzz \
	vfpu_fuzz_build \
	shaders \
	shader-verify \
	shader-repro-verify \
	psp-oracle \
	psp-oracle-nakagawa \
	psp-oracle-vfpu \
	psp-oracle-vfpu-build \
	psp-oracle-nakagawa-smoke \
	psp-oracle-nakagawa-smoke-build \
	psp-oracle-nakagawa-smoke-generate \
	gpu-capture-selftest \
	gpu-selftest-status \
	form-sweep-build \
	form-sweep-link \
	form-sweep-selftest \
	form-sweep-deep

PROJECT_LATIN_STYLE_TARGETS := $(foreach i,1 2 3 4 5 6 7 9 10 11 12 13 14 15,project-latin-$(i)-font)
PROJECT_FONT_SLOT_TARGETS := project-japanese-font project-latin-font project-latin-small-font project-korean-font $(PROJECT_LATIN_STYLE_TARGETS)
INTERNAL_TARGETS := FORCE compile-inputs project-japanese-font project-latin-font project-latin-small-font project-korean-font project-latin-1-font project-latin-2-font project-latin-3-font project-latin-4-font project-latin-5-font project-latin-6-font project-latin-7-font project-latin-9-font project-latin-10-font project-latin-11-font project-latin-12-font project-latin-13-font project-latin-14-font project-latin-15-font project-fonts player-vulkan-check player-state-test-bin input-settings-test-bin package-builder-test-bin sdl3-check ui-font-selftest vfpu_fuzz_validate_synthetic provenance-record-gap-check native-core-objects-core native-core-objects-player
.PHONY: $(PUBLIC_TARGETS) $(INTERNAL_TARGETS)

HELP_DESCRIPTION_help := list every public Make target and its purpose
HELP_DESCRIPTION_check := run public-safe docs, policy, audit, native, and fast checks
HELP_DESCRIPTION_contrib-check := run only the local gates that apply to your changed files
HELP_DESCRIPTION_contrib-refresh := reproduce the public provenance controls for your changed paths
HELP_DESCRIPTION_test := run the complete Python tooling test suite
HELP_DESCRIPTION_native-core-tests := build and run host-side native core tests
HELP_DESCRIPTION_fuzz-parsers := run bounded native parser mutation fuzzing
HELP_DESCRIPTION_readiness := run the strict pre-PR gate with external authority
HELP_DESCRIPTION_provenance-refresh := refresh controls through hosted generation and stage them
HELP_DESCRIPTION_tree-lint := report tracked files that no other tracked file names (reporting only)
HELP_DESCRIPTION_all := generate and compile the current title runtime
HELP_DESCRIPTION_pipeline := generate image, imports, and recomputed source artifacts
HELP_DESCRIPTION_compile := compile and link the generated runtime
HELP_DESCRIPTION_compiler-info := print the effective compiler and build settings
HELP_DESCRIPTION_runtime-objects := build runtime and decoder objects
HELP_DESCRIPTION_portable-core-objects := build host-neutral runtime objects
HELP_DESCRIPTION_format-portability-check := compile the portable core against a 32-bit newlib-shaped stdint shim with -Werror=format
HELP_DESCRIPTION_atrac3p-objects := build ATRAC decoder objects
HELP_DESCRIPTION_player := build the native player
HELP_DESCRIPTION_player-ui-tests := build and run native player UI tests (needs SDL3)
HELP_DESCRIPTION_player-ui-regressions := run scripted native player UI event and recovery flows (needs SDL3)
HELP_DESCRIPTION_public-safe-verify := build public-safe host-neutral core objects
HELP_DESCRIPTION_production-smoke := run the public production-composition smoke test
HELP_DESCRIPTION_production-smoke-staged := run the production smoke from a staging directory outside the build tree
HELP_DESCRIPTION_production-smoke-clean := remove production smoke artifacts
HELP_DESCRIPTION_production-smoke-gap := run the public AOT-gap dispatch smoke test
HELP_DESCRIPTION_perf-benchmark := run the public source-owned SR_PERF benchmark matrix and overhead check
HELP_DESCRIPTION_showcase := build packaged source-owned PSP showcase demos (requires PSPDEV)
HELP_DESCRIPTION_showcase-smoke := run bundled demos headlessly with telemetry checks
HELP_DESCRIPTION_showcase-linux := build and smoke the showcase demos on this POSIX host (Linux CI gate)
HELP_DESCRIPTION_display-smoke := build the display smoke fixture
HELP_DESCRIPTION_display-smoke-run := run display smoke and compare its GE/present flight bundle
HELP_DESCRIPTION_display-smoke-picture-gate := compare display smoke frames with an accepted reference (SKIP, PASS, FAIL)
HELP_DESCRIPTION_display-smoke-gui := run the display smoke with its GUI
HELP_DESCRIPTION_display-smoke-fsr1 := compare the display smoke with the FSR 1 presentation upscaler off and on (needs a Vulkan device)
HELP_DESCRIPTION_display-smoke-quit := close the display smoke window mid-run and check the orderly quit
HELP_DESCRIPTION_display-smoke-player := build the player and run display smoke
HELP_DESCRIPTION_display-smoke-clean := remove display smoke artifacts
HELP_DESCRIPTION_title-qualification := validate public title manifests, input profiles and staging declarations
HELP_DESCRIPTION_title-qualification-smoke := launch the public display fixture through the native player and write its bring-up report
HELP_DESCRIPTION_production-smoke-gap-clean := remove AOT-gap smoke artifacts
HELP_DESCRIPTION_platform-ladder := run the complete public platform ladder
HELP_DESCRIPTION_platform-ladder-zero := run the zero-base platform fixture
HELP_DESCRIPTION_platform-ladder-reloc := run the relocation platform fixture
HELP_DESCRIPTION_platform-ladder-gap := run the interpreter-gap platform fixture
HELP_DESCRIPTION_platform-ladder-sched := run the scheduler platform fixture
HELP_DESCRIPTION_platform-ladder-fpu := run the FPU platform fixture
HELP_DESCRIPTION_platform-ladder-fs := run the filesystem platform fixture
HELP_DESCRIPTION_platform-ladder-fs-negative := run the negative filesystem fixture
HELP_DESCRIPTION_platform-ladder-title2 := run the second-title platform fixture
HELP_DESCRIPTION_platform-ladder-title2-negative := run the negative second-title fixture
HELP_DESCRIPTION_platform-ladder-overlay := run the guest-placed overlay module fixture
HELP_DESCRIPTION_platform-ladder-clean := remove platform-ladder artifacts
HELP_DESCRIPTION_profile-zero-e2e := run the manifest-driven profile-zero production route
HELP_DESCRIPTION_cosim-selftest := run the source-owned AOT/interpreter cosimulation
HELP_DESCRIPTION_cosim-selftest-split := run the cosimulation with every body split at other entries
HELP_DESCRIPTION_cosim-selftest-opt := run the cosimulation with generated code at COSIM_OPT_LEVEL (#1730)
HELP_DESCRIPTION_cosim-selftest-promoted := run the cosimulation with codegen register promotion (#284)
HELP_DESCRIPTION_cosim-selftest-run := build and run the cosimulation harness
HELP_DESCRIPTION_cosim-selftest-clean := remove cosimulation artifacts
HELP_DESCRIPTION_cosim-mutants := run the cosimulation negative corpus
HELP_DESCRIPTION_cosim-selftest-llvm := run the cosimulation with the opt-in LLVM IR backend (#330)
HELP_DESCRIPTION_cosim-mutants-llvm := run the LLVM IR backend's cosimulation mutation campaign (#330)
HELP_DESCRIPTION_snapshot-selftest := run snapshot execution against the reference interpreter (#1842)
HELP_DESCRIPTION_snapshot-selftest-run := build and run the snapshot runner and its generated-C mutant
HELP_DESCRIPTION_verify-selftest := run sampled in-play differential checking on a synthetic module (#1843)
HELP_DESCRIPTION_verify-selftest-run := build and run the verify harness, its generated-C mutant and the capture replay
HELP_DESCRIPTION_form-sweep-build := generate, translate, build and run one form sweep (#1844)
HELP_DESCRIPTION_form-sweep-link := build and run the form-sweep harness over an already translated module
HELP_DESCRIPTION_form-sweep-selftest := sweep every scalar instruction form codegen emits against the reference interpreter, with a generated mutant (#1844)
HELP_DESCRIPTION_form-sweep-deep := the form sweep with every instance and 2048 states per instance (#1844)
HELP_DESCRIPTION_clean := remove current-title build outputs
HELP_DESCRIPTION_clean-fixtures := remove smoke, cosimulation, and oracle artifacts
HELP_DESCRIPTION_tidy := remove intermediates while preserving linked binaries
HELP_DESCRIPTION_distclean := remove intermediates and ephemeral build logs
HELP_DESCRIPTION_clean-all := remove all build and fixture outputs
HELP_DESCRIPTION_clean-preview := preview allowlisted workspace clean (CONFIRM=1 to apply)
HELP_DESCRIPTION_verify := compare generated output against external oracle data
HELP_DESCRIPTION_selftest := run the runtime selftest
HELP_DESCRIPTION_strbuf-selftest := run the checked-formatting selftest
HELP_DESCRIPTION_sched-selftest := run the scheduler selftest suite
HELP_DESCRIPTION_sched-selftest-one := run one scheduler selftest build
HELP_DESCRIPTION_selftest-repeat := rerun the built scheduler and HLE thread selftests until one fails
HELP_DESCRIPTION_heap-selftest := run the heap and allocator selftest
HELP_DESCRIPTION_profiler-selftest := run the profiler selftest
HELP_DESCRIPTION_gui-present-selftest := run the host present pacing selftest
HELP_DESCRIPTION_clay-layout-selftest := build and run the vendored Clay layout selftest
HELP_DESCRIPTION_ed25519-verify-selftest := check nk_ed25519_verify (vendored Monocypher) against the RFC 8032 vectors and nk_minisign_verify against real minisign output
HELP_DESCRIPTION_nk-ui-player-screens-selftest := build and run nk_ui Library, Title detail and router snapshots
HELP_DESCRIPTION_nk-ui-player-screens-capture := save offscreen synthetic Library and Title detail captures (set NK_UI_CAPTURE_DIR)
HELP_DESCRIPTION_nk-ui-overlay-host-selftest := build and run the in-game overlay host on SDL3_ttf, real key events and the SDL_Renderer backend (#1159)
HELP_DESCRIPTION_nk-ui-overlay-screens-selftest := build and run the in-game overlay page snapshots and flow checks (#1159)
HELP_DESCRIPTION_nk-ui-overlay-screens-capture := save offscreen captures of every in-game overlay page at three sizes (set NK_UI_CAPTURE_DIR)
HELP_DESCRIPTION_nk-ui-core-selftest := build and run the nk_ui core selftest (synthetic layout, focus and scale)
HELP_DESCRIPTION_nk-ui-sdlr-selftest := build and run the nk_ui SDL_Renderer and SDL3_ttf selftest (software snapshots; SKIPs without SDL3_ttf)
HELP_DESCRIPTION_nk-ui-vk-selftest := build and run the nk_ui Vulkan backend selftest (headless device, offscreen readback; SKIPs without Vulkan)
HELP_DESCRIPTION_nk-ui-focus-selftest := build and run the nk_ui focus graph, input model and prompt glyph selftest (SDL translation leg SKIPs without SDL3)
HELP_DESCRIPTION_coro-selftest := run the coroutine selftest
HELP_DESCRIPTION_coro-selftest-asm := run the coroutine selftest on the Android stack-switch backend (POSIX hosts)
HELP_DESCRIPTION_hle-thread-selftest := build and run the HLE thread selftest
HELP_DESCRIPTION_aot-return-boundary-thread-selftest := run the real recomp/scheduler thread-boundary regression
HELP_DESCRIPTION_hle-thread-selftest-build := build the HLE thread selftest only
HELP_DESCRIPTION_hle-title-selftest := run title-configured HLE selftests
HELP_DESCRIPTION_hle-title-selftest-one := run one title-configured HLE selftest
HELP_DESCRIPTION_dispatch-selftest := run the production dispatch selftest
HELP_DESCRIPTION_stale-code-selftest := run the stale translated-code detector selftest
HELP_DESCRIPTION_cpu-lle-selftest := run the LLE COP0/exception interpreter selftest
HELP_DESCRIPTION_domain-mode-selftest := run the LLE domain-mode and import-seam selftest
HELP_DESCRIPTION_dispatch-isolation-selftest := run dispatch isolation selftests
HELP_DESCRIPTION_dispatch-isolation-selftest-one := run one dispatch isolation selftest
HELP_DESCRIPTION_code-capture-selftest := capture code installed at run time, translate it and bind it by its code
HELP_DESCRIPTION_memory-watch-selftest := run the SR_WATCH store-watch selftest
HELP_DESCRIPTION_asset-index-selftest := run the asset-index selftest
HELP_DESCRIPTION_fp-convert-selftest := run the FPU conversion selftest
HELP_DESCRIPTION_ge-float24-selftest := run the GE float24/row-sum/reciprocal reference selftest
HELP_DESCRIPTION_ge-cpu-profile-validate-selftest := run the GE CPU-profile validator selftest on the production GE object
HELP_DESCRIPTION_ge-flight-overhead := measure the disarmed flight recorder cost on the GE draw path (recorder linked vs absent; no game data)
HELP_DESCRIPTION_vfpu-tables-selftest := run the VFPU table-loader selftest
HELP_DESCRIPTION_watchpoints-file-selftest := run the watchpoints-file selftest
HELP_DESCRIPTION_ge-raster-ref-selftest := run the GE triangle setup and rasterization reference selftest
HELP_DESCRIPTION_vfpu-interp-selftest := run the VFPU interpreter selftest
HELP_DESCRIPTION_atrac3p-selftest := run the ATRAC3+ decoder selftest
HELP_DESCRIPTION_atrac3p-bridge-selftest := run the ATRAC3+ HLE bridge selftest
HELP_DESCRIPTION_ge-texture-ref-selftest := run the GE texture sampling reference selftest
HELP_DESCRIPTION_fbcap-selftest := run the presenter-neutral frame capture selftest
HELP_DESCRIPTION_videorec-selftest := run the run-recorder (video and audio capture) selftest
HELP_DESCRIPTION_osk-text-entry-selftest := run the on-screen keyboard's non-blocking text-entry selftest
HELP_DESCRIPTION_osk-overlay-selftest := run the in-window keyboard's state-machine selftest (pure C)
HELP_DESCRIPTION_ace-selftest := run Ace Mode's CPU-only selftest (launch line, 30 Hz detector, readout, report; pure C)
HELP_DESCRIPTION_ace-smooth-selftest := run Ace Mode's in-between frames selftest on a headless Vulkan device (SKIPs without one)
HELP_DESCRIPTION_fsr1-selftest := run the FSR 1 presentation upscaler selftest on a headless Vulkan device (SKIPs without one)
HELP_DESCRIPTION_ace-ui-selftest := run Ace Mode's in-game UI selftest (layout, then a headless Vulkan render; SKIPs the GPU leg without Vulkan)
HELP_DESCRIPTION_motion-match-selftest := run the draw matcher's selftest for render-state motion (synthetic PRIM lists; pure C)
HELP_DESCRIPTION_osk-overlay-paint-selftest := run the in-window keyboard painter's exact-pixel selftest
HELP_DESCRIPTION_psmf-producer-selftest := run the source-owned bounded PSMF producer selftest
HELP_DESCRIPTION_psmf-media-selftest := run the source-owned PSMF-to-decoder media selftest
HELP_DESCRIPTION_psmf-media-selftest-csc-mutant := prove the media selftest kills a Csc that reports success without writing pixels
HELP_DESCRIPTION_psmf-media-focused-selftest := run the media selftest linked against only the media HLE files
HELP_DESCRIPTION_audio-selftest := run the SDL3 host audio output selftest
HELP_DESCRIPTION_audio-resampler-bench := run the synthetic-input audio resampler microbenchmark (no game data)
HELP_DESCRIPTION_atrac3p-title-accept := run the optional ATRAC3+ title acceptance route
HELP_DESCRIPTION_gpu-coherence-selftest := run the GPU coherence selftest
HELP_DESCRIPTION_gpu-coherence-selftest-snapbar-mutants := prove the GPU coherence selftest kills three broken snapshot-copy barrier variants
HELP_DESCRIPTION_gpu-snapsync-selftest := run the GPU snapshot-sync selftest
HELP_DESCRIPTION_gpu-ace-selftest := run Ace Mode's texture filtering selftest on a real Vulkan device (SKIPs without one)
HELP_DESCRIPTION_tex-scale-selftest := run the xBRZ texture scaling selftest against the pinned upstream goldens (no GPU)
HELP_DESCRIPTION_tex-scale-gate-mutant := prove the texture scaling selftest kills a build without the Ace gate
HELP_DESCRIPTION_texrepl-selftest := run the texture replacement pack selftest (PPSSPP hashes, pack layout, lookup order, budget; no GPU)
HELP_DESCRIPTION_texrepl-mutant := prove the texture replacement selftest kills a palette identity without the CLUTFORMAT word
HELP_DESCRIPTION_gpu-texscale-selftest := run texture scaling and the smart 2D filter on a real Vulkan device (SKIPs without one)
HELP_DESCRIPTION_gpu-texscale-mutant := prove the texture scaling selftest kills a cache that binds a stale scaled copy
HELP_DESCRIPTION_gpu-texrepl-mutant := prove the replace cells kill a stub lookup and a palette identity without the CLUTFORMAT word
HELP_DESCRIPTION_ge-replay := run the graphics-engine replay selftest
HELP_DESCRIPTION_ge-first-bad-draw-selftest := prove ge_replay --first-bad-draw finds the draw an executed Vulkan mutant changes (SKIPs without Vulkan)
HELP_DESCRIPTION_run := build and launch the current title runtime
HELP_DESCRIPTION_run_elf := build the reference interpreter runner
HELP_DESCRIPTION_vfpu_fuzz := build and run the VFPU differential fuzzer
HELP_DESCRIPTION_vfpu_fuzz_build := build the VFPU differential fuzzer only
HELP_DESCRIPTION_shaders := regenerate embedded shader artifacts
HELP_DESCRIPTION_shader-verify := verify embedded shader hashes
HELP_DESCRIPTION_shader-repro-verify := recompile and compare shader bytes
HELP_DESCRIPTION_psp-oracle := run the scalar Nakagawa PSP oracle stream
HELP_DESCRIPTION_psp-oracle-nakagawa := run the scalar host and PSP comparison helper
HELP_DESCRIPTION_psp-oracle-vfpu := emit the host VFPU oracle stream
HELP_DESCRIPTION_psp-oracle-vfpu-build := build the host VFPU oracle
HELP_DESCRIPTION_psp-oracle-nakagawa-smoke := run the generated-code PSP oracle smoke
HELP_DESCRIPTION_psp-oracle-nakagawa-smoke-build := build the generated-code PSP oracle smoke
HELP_DESCRIPTION_psp-oracle-nakagawa-smoke-generate := generate the PSP oracle smoke artifacts
HELP_DESCRIPTION_gpu-capture-selftest := run the GPU capture selftest
HELP_DESCRIPTION_gpu-selftest-status := build and run the GPU selftests, reporting PASS, SKIP or FAIL per binary

.SECONDARY:

help:
	$(info Nakagawa Recomp Make targets)
	$(info Usage: mingw32-make [VARIABLE=value ...] TARGET)
	$(info )
	$(foreach target,$(PUBLIC_TARGETS),$(info   $(target) - $(HELP_DESCRIPTION_$(target))))
	@:
