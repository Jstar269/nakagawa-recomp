# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors
# Copyright (C) 2026 the Nakagawa Recomp authors
# Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
# Modified by Nakagawa Recomp contributors since 2026-07-19.
# See NOTICE.md for upstream lineage and modification provenance.

# Split out of the root Makefile (#1531), text unchanged: the native runtime self-tests and the native-core-tests aggregate.
# The root Makefile includes it where the text used to be, so evaluation order is unchanged.

# One configuration of the matrix. SCHED_SELFTEST_CONFIG names the flavour; the title
# configuration is generated fresh into build/<game>/title-config/<flavour>/.
SCHED_SELFTEST_CONFIG ?= generic
SCHED_SELFTEST_DIR := $(BUILD_DIR)/title-config/$(SCHED_SELFTEST_CONFIG)
SCHED_SELFTEST_MANIFEST := $(SCHED_SELFTEST_MANIFEST_$(SCHED_SELFTEST_CONFIG))
SCHED_SELFTEST_CONFIG_ARG := $(if $(strip $(SCHED_SELFTEST_MANIFEST)),--manifest $(strip $(SCHED_SELFTEST_MANIFEST)),)

sched-selftest-one: $(TITLE_CONFIG_TOOL) tools/title_manifest.py src/rt/nested_frames.c src/rt/nested_frames.h
	"$(PYTHON)" $(TITLE_CONFIG_TOOL) $(SCHED_SELFTEST_CONFIG_ARG) --output $(SCHED_SELFTEST_DIR)/sr_title_config.h
	$(CC) $(CFLAGS) -I$(SCHED_SELFTEST_DIR) -DSR_SCHED_LIVENESS_TEST $(LDFLAGS) -o $(BUILD_DIR)/sched_selftest_$(SCHED_SELFTEST_CONFIG).exe \
		tests/native/sched_selftest.c src/rt/nested_frames.c src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/title_config.c src/rt/ace.c $(LIBS)
	$(BUILD_DIR)/sched_selftest_$(SCHED_SELFTEST_CONFIG).exe

# selftest-repeat — rerun the already built scheduler and HLE thread selftests until one fails
# (#1846). A rare ordering bug in the cooperative scheduler or the thread HLE can pass the single
# run each CI leg makes for weeks; the nightly matrix repeats them SELFTEST_REPEAT times and
# names the iteration and binary that failed. It builds nothing: run sched-selftest and
# hle-thread-selftest first, with the same GAME_NAME/BUILD_DIR. Output of a passing run is
# discarded; a failing run's output is printed. POSIX shell recipe (Linux nightly leg).
SELFTEST_REPEAT ?= 20
SELFTEST_REPEAT_EXES = $(foreach cfg,$(SCHED_SELFTEST_CONFIGS),$(BUILD_DIR)/sched_selftest_$(cfg).exe) $(BUILD_DIR)/hle_thread_selftest.exe

selftest-repeat:
	@case "$(SELFTEST_REPEAT)" in ''|*[!0-9]*|0) echo "selftest-repeat: SELFTEST_REPEAT must be a positive integer, got '$(SELFTEST_REPEAT)'" >&2; exit 2;; esac
	@for exe in $(SELFTEST_REPEAT_EXES); do \
		[ -x "$$exe" ] || { echo "selftest-repeat: $$exe is missing; run sched-selftest and hle-thread-selftest first" >&2; exit 2; }; \
	done
	@log="$(BUILD_DIR)/selftest-repeat.log"; i=1; \
	while [ "$$i" -le "$(SELFTEST_REPEAT)" ]; do \
		for exe in $(SELFTEST_REPEAT_EXES); do \
			if ! "$$exe" >"$$log" 2>&1; then \
				cat "$$log"; \
				echo "selftest-repeat: FAILED on iteration $$i of $(SELFTEST_REPEAT): $$exe" >&2; \
				exit 1; \
			fi; \
		done; \
		echo "selftest-repeat: iteration $$i of $(SELFTEST_REPEAT) passed"; \
		i=$$((i + 1)); \
	done

# heap-selftest — white-box unit tests for the guest heap allocator's boundary-tag
# coalescing (tests/native/heap_selftest.c). No game inputs needed; #includes recomp.c for
# direct access to the allocator statics and sr_heap_stats(), and drives the same
# sr_newlib_malloc/free entry points the generated code calls. Asserts forward and
# backward merges, the #122 regression (a large allocation must succeed after the
# arena is fragmented and released), refusal to merge across a live block even with
# a forged boundary tag, clean payload across merge seams, bounded free-list growth
# under randomized churn, and corruption/overflow guardrails. Exit code 0 = all
# invariants hold. vfpu_tables.c is linked because the inlined recomp.c now calls
# sr_vfpu_load() and reads the VFPU LUT pointers (issue #187); without it the
# standalone binary fails to link after the table-loader integration.
heap-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/heap_selftest.exe \
		tests/native/heap_selftest.c src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/vfpu_tables.c src/rt/title_config.c src/rt/perf.c $(LIBS) -lm
	$(BUILD_DIR)/heap_selftest.exe

native-core-tests: heap-selftest

# profiler-selftest — production profiler hash-table regression suite. Exercises PC zero as a
# real key and a deliberately saturated 64-probe collision window without game inputs.
profiler-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -DSR_PROFILER_SELFTEST \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables $(LDFLAGS) \
		-Wl,--gc-sections -o $(BUILD_DIR)/profiler_selftest.exe \
		tests/native/profiler_selftest.c src/rt/recomp.c src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/perf.c $(LIBS) -lm
	$(BUILD_DIR)/profiler_selftest.exe

# Also covers the SR_PERF line formats, PERF_AUDIO included (#1296).
native-core-tests: profiler-selftest

gui-present-selftest:
	$(CC) $(CFLAGS) -Isrc/ui -DSR_GUI_PRESENT_SELFTEST -ffunction-sections -fdata-sections \
		$(LDFLAGS) -Wl,--gc-sections -o $(BUILD_DIR)/gui_present_selftest.exe \
		src/rt/gui.c src/rt/ace.c $(LIBS)
	$(BUILD_DIR)/gui_present_selftest.exe

# vfpu-tables-selftest — fail-closed VFPU table loader regression suite (issue #187):
# SHA-256 known-answer vectors, value-domain validators against synthetic corrupt
# buffers, file-level loader tests against temporary table roots (truncated, extra
# data, wrong content of the same length, endian-swapped, missing files), repeated
# initialization and concurrent first use. No game inputs required; the success
# path validates against the committed assets/vfpu/ tables.
vfpu-tables-selftest:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_tables_selftest.exe -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		tests/native/vfpu_tables_selftest.c src/rt/vfpu_tables.c $(LIBS)
	$(BUILD_DIR)/vfpu_tables_selftest.exe

# watchpoints-file-selftest — bounded parser regression for the derived
# watchpoints.json runtime artifact (issue #188): the canonical JSON
# fixture round-trips into the expected native watchpoint set, plus fail-closed
# cases (wrong version/format, malformed JSON, out-of-range/reversed/oversized
# strbuf-selftest — unit and adversarial tests for sr_buf_append / strbuf.h.
# Exercises bounded formatting, hostile cursors, negative returns, canary bounds,
# and golden trace formatting invariants.
strbuf-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/strbuf_selftest.exe tests/native/strbuf_selftest.c
	$(BUILD_DIR)/strbuf_selftest.exe

watchpoints-file-selftest:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/watchpoints_file_selftest.exe \
		tests/native/watchpoints_file_selftest.c src/rt/watchpoints_file.c $(LIBS)
	$(BUILD_DIR)/watchpoints_file_selftest.exe

# ge-texture-ref-selftest — project-authored GE texture sampling reference model
# (issue #703): 5650/5551/4444 unpacking, CLUT shift/mask, power-of-two wrap and
# clamp, and 4-bit bilinear filtering under every SPEC_AMBIGUITY reading.
# Strict C99; not wired into the renderer.  No game inputs.
ge-texture-ref-selftest:
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt \
		-o $(BUILD_DIR)/ge_texture_ref_selftest.exe tests/native/ge_texture_ref_selftest.c src/rt/ge_texture_ref.c -lm
	$(BUILD_DIR)/ge_texture_ref_selftest.exe

native-core-tests: ge-texture-ref-selftest fbcap-selftest

# fbcap-selftest — the presenter-neutral frame capture service (src/rt/fbcap.c, issue #57)
# driven through its public API with synthetic frames: exact P6 header, size, channel order
# and pixels for host-served and read-back captures (one frame publishes identical bytes
# either way), skipped-present cancel, refused re-arm, failed publication resolving as
# failed, and the parent-directory contract. Strict C99; no GPU, window or game input.
fbcap-selftest:
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		-o $(BUILD_DIR)/fbcap_selftest.exe \
		tests/native/fbcap_selftest.c src/rt/fbcap.c src/rt/fbcap_policy.c
	$(BUILD_DIR)/fbcap_selftest.exe $(BUILD_DIR)/fbcap_selftest

# osk-text-entry-selftest - the on-screen keyboard's text entry (src/rt/osk_text_entry.c)
# with a fake native input box: a poll never waits for the person, the box runs off the
# polling thread, answers are handed over once, an abandoned box is closed whether or not it
# exists yet, and the offscreen presenter opens no box. Windows uses a real, never-shown
# window for the abandon cases; other hosts check the no-box path. No game data.
native-core-tests: osk-text-entry-selftest
osk-text-entry-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt \
		-o $(BUILD_DIR)/osk_text_entry_selftest$(EXE_EXT) \
		tests/native/osk_text_entry_selftest.c src/rt/osk_text_entry.c src/rt/osk_overlay.c
	$(BUILD_DIR)/osk_text_entry_selftest$(EXE_EXT)

# ace-selftest - Ace Mode's CPU-only cells (src/rt/ace.c): the SR_ACE line fails closed, the
# 30 Hz detector's classes, hysteresis and feed, the speed readout's definitions, the per-run
# report and an off run's silence. Built from ace.c and its test alone: a reference from ace.c
# to anything but the C library fails this link, which is the S1 link test for ace.c.
native-core-tests: ace-selftest
ace-selftest:
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt \
		-o $(BUILD_DIR)/ace_selftest$(EXE_EXT) tests/native/ace_selftest.c src/rt/ace.c
	$(BUILD_DIR)/ace_selftest$(EXE_EXT) $(BUILD_DIR)

# osk-overlay-selftest - the in-window on-screen keyboard's state machine (src/rt/osk_overlay.c):
# navigation, typing, the guest's length bound with UTF-16 pairs, backspace, confirm and cancel,
# the UTF-16 answer, the inputtype rules, edge-based pad input and the session the HLE keyboard
# polls. Pure C with no window, no SDL and no game data, so it runs on every host.
native-core-tests: osk-overlay-selftest
osk-overlay-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt \
		-o $(BUILD_DIR)/osk_overlay_selftest$(EXE_EXT) \
		tests/native/osk_overlay_selftest.c src/rt/osk_overlay.c
	$(BUILD_DIR)/osk_overlay_selftest$(EXE_EXT)

# atrac3p-selftest — standalone ATRAC3+ decoder regression suite (PR-A,
# src/rt/atrac3p/). Public checks are source-owned (create validation, NULL/
# oversized/garbage rejection with the nb_samples=0 contract, a deterministic
# terminator-frame decode through the production entry point, determinism
# across instances/reset/flush, destroy(NULL)). The optional private fixture
# hook (ATRAC3P_FIXTURE=<dir> with stream.bin + meta.txt, see
# tests/native/atrac3p_selftest.c) decodes and SHA-256s the whole stream with the
# same hash primitive as the VFPU table loader; it is reported as SKIP when
# unset and must never be committed. No game inputs required.

atrac3p-selftest:
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(LDFLAGS) \
		-o $(BUILD_DIR)/atrac3p_selftest.exe \
		tests/native/atrac3p_selftest.c $(ATRAC3P_SRCS) src/rt/vfpu_tables.c -lm
	$(BUILD_DIR)/atrac3p_selftest.exe

# atrac3p-bridge-selftest — regression suite for the PSP ATRAC3+ HLE decode
# bridge (PR-B, src/rt/atrac3p_bridge.c). Source-owned tests: create
# validation, NULL/contract-violation rejection, the deterministic terminator
# and MONO transform-path canaries through the production bridge entry point,
# determinism across instances/reset/recreate, destroy(NULL). No game inputs,
# no fixtures required. With NK_PCM_WAV_DIR=<dir> it also writes the decoded canary
# PCM there as WAV files (tests/native/pcm_wav_dump.c), to listen to while debugging.
atrac3p-bridge-selftest:
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil -Itests/native $(LDFLAGS) \
		-o $(BUILD_DIR)/atrac3p_bridge_selftest.exe \
		tests/native/atrac3p_bridge_selftest.c src/rt/atrac3p_bridge.c src/rt/perf.c tests/native/pcm_wav_dump.c \
		$(ATRAC3P_SRCS) src/rt/vfpu_tables.c -lm
	$(BUILD_DIR)/atrac3p_bridge_selftest.exe

# atrac3p-title-accept — PRIVATE title acceptance (PR-C, #286/#32). Decodes the
# lawful private title ATRAC3+ stream (canonically the extracted bgm_title.sgb)
# through the production PR-A decoder/PR-B bridge and asserts valid, nonzero,
# deterministic PCM. Prints aggregate statistics only; never writes or hashes
# retail content. SKIPs (exit 77) when the private input is absent — never a
# pass. NOT part of CI; the evidence is the user's private title route only.
ATRAC3P_TITLE ?= place_game_here/EXTRACTED/PSP_GAME/USRDIR/data/sound/bgm/bgm_title.sgb

atrac3p-title-accept:
	$(CC) $(CFLAGS) -Isrc/rt/atrac3p -Isrc/rt/atrac3p/libavcodec \
		-Isrc/rt/atrac3p/libavutil $(LDFLAGS) \
		-o $(BUILD_DIR)/atrac3p_title_accept.exe \
		src/rt/atrac3p_title_accept.c src/rt/atrac3p_bridge.c \
		$(ATRAC3P_SRCS) src/rt/vfpu_tables.c -lm
	$(BUILD_DIR)/atrac3p_title_accept.exe "$(ATRAC3P_TITLE)"

# vfpu-interp-selftest — executable regression for issue #184: quad/vector VFPU
# memory ops are all-or-nothing (span preflight before any lane commit, no prefix
# consumption on rejection), plus the vcrs width guard and vrot active-lane
# overlap scan, plus the issue #40 codegen-vs-interpreter differential for
# vhdp/vmscl overlap encodings. The white-box TU includes the real
# recomp.c/vfpu_tables.c/vfpu_interp.c (heap_selftest pattern); only
# scheduler/driver plumbing is stubbed. No game inputs or private data required.
vfpu-interp-selftest: $(GENERIC_TITLE_CONFIG_HEADER) $(BUILD_DIR)/vfpu_overlap_diff_cases.h
	$(CC) $(CFLAGS) -DSR_FLIGHT_RECORDER_STANDALONE -I$(GENERIC_TITLE_CONFIG_DIR) -I$(BUILD_DIR) $(LDFLAGS) -o $(BUILD_DIR)/vfpu_interp_selftest.exe \
		tests/native/vfpu_interp_selftest.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/perf.c $(LIBS) -lm
	$(BUILD_DIR)/vfpu_interp_selftest.exe

$(BUILD_DIR)/vfpu_overlap_diff_cases.h: tools/vfpu_overlap_diff_gen.py tools/codegen.py
	"$(PYTHON)" tools/vfpu_overlap_diff_gen.py $@

# Canonical Allegrex/VFPU float-to-word fixed-vector regression. Expected
# results are explicit source-owned constants and the same vectors run under
# every available host rounding mode. CI adds the UBSan float-cast-overflow gate.
fp-convert-selftest:
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(BUILD_DIR)/fp_convert_selftest.exe \
		tests/native/fp_convert_selftest.c -lm
	$(BUILD_DIR)/fp_convert_selftest.exe

# ge-float24-selftest — project-authored GE float24, row-sum transform and
# reciprocal-framework reference model (issue #695): exhaustive format checks,
# contract vectors and a differential against a second, host-double model of the
# same rules under every SPEC_AMBIGUITY policy reading.  Strict C99; not wired into the renderer.
ge-float24-selftest:
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt \
		-o $(BUILD_DIR)/ge_float24_selftest.exe tests/native/ge_float24_selftest.c src/rt/ge_float24.c -lm
	$(BUILD_DIR)/ge_float24_selftest.exe

native-core-tests: ge-float24-selftest

# ge-cpu-profile-validate-selftest — ge_cpu_profile_validate() (issue #282) against the
# counters the production GE object produces for source-owned display lists: every
# primitive type in both modes, indexed draws, clipped and rejected triangles, rejected
# points and sprite corners, patches, block transfer and CLUT load, the GPU hook seam, and
# each profile switch; then faults injected into copies of those blocks, each of which
# must fail exactly the checks it breaks.  Run twice because ge.c latches the strip-cache
# switch per process.  No game input.
GE_CPU_PROFILE_VALIDATE_SELFTEST := $(BUILD_DIR)/ge_cpu_profile_validate_selftest$(EXE_EXT)

ge-cpu-profile-validate-selftest: $(RT_GE_O)
	$(CC) $(CFLAGS) -Werror $(LDFLAGS) -o $(GE_CPU_PROFILE_VALIDATE_SELFTEST) \
		tests/native/ge_cpu_profile_validate_selftest.c $(RT_GE_O) src/rt/ge_capture.c \
		src/rt/flight_recorder.c src/rt/perf.c -lm
	$(GE_CPU_PROFILE_VALIDATE_SELFTEST)
	$(GE_CPU_PROFILE_VALIDATE_SELFTEST) --strip-cache-disabled

native-core-tests: ge-cpu-profile-validate-selftest

# ge-flight-overhead -- cost of the disarmed flight recorder on the GE paths (#320): the
# ge-cpu-profile-validate-selftest harness's --bench mode on the production ge.c, built once
# with the recorder linked (SR_FLIGHT unset) and once with -USR_FLIGHT_RECORDER_LINKED, run
# in alternating pairs.  Not part of native-core-tests: it measures, it does not gate.
# GE_FLIGHT_ROUNDS is the samples per workload and pair; GE_FLIGHT_PAIRS the pair count.
GE_FLIGHT_DIR := $(BUILD_DIR)/ge-flight-overhead
GE_FLIGHT_ROUNDS ?= 30
GE_FLIGHT_PAIRS ?= 5
GE_FLIGHT_SRCS := tests/native/ge_cpu_profile_validate_selftest.c src/rt/ge_capture.c src/rt/flight_recorder.c src/rt/perf.c

ge-flight-overhead:
	"$(PYTHON)" -c "import os, sys; [os.makedirs(d, exist_ok=True) for d in sys.argv[1:]]" $(GE_FLIGHT_DIR)/linked $(GE_FLIGHT_DIR)/absent
	$(CC) $(GE_CFLAGS) -c src/rt/ge.c -o $(GE_FLIGHT_DIR)/linked/ge.o
	$(CC) $(GE_CFLAGS) -USR_FLIGHT_RECORDER_LINKED -c src/rt/ge.c -o $(GE_FLIGHT_DIR)/absent/ge.o
	$(CC) $(CFLAGS) -O2 $(LDFLAGS) -o $(GE_FLIGHT_DIR)/linked/bench$(EXE_EXT) $(GE_FLIGHT_SRCS) $(GE_FLIGHT_DIR)/linked/ge.o -lm
	$(CC) $(CFLAGS) -O2 -USR_FLIGHT_RECORDER_LINKED $(LDFLAGS) -o $(GE_FLIGHT_DIR)/absent/bench$(EXE_EXT) $(GE_FLIGHT_SRCS) $(GE_FLIGHT_DIR)/absent/ge.o -lm
	"$(PYTHON)" tools/ge_flight_overhead.py --dir "$(GE_FLIGHT_DIR)" --rounds $(GE_FLIGHT_ROUNDS) --pairs $(GE_FLIGHT_PAIRS)

# Production-backend regression for the historical repeated-adoption RAM runaway.
# This intentionally does not link HLE or define SR_CORO_LIFECYCLE_TEST: it proves
# the ordinary coroutine implementation itself keeps one stable main identity.
coro-selftest:
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/coro_selftest.exe \
		tests/native/sr_coro_selftest.c src/rt/sr_coro.c $(LDFLAGS) \
		$(if $(filter Windows_NT,$(OS)),-lpsapi,-lm)
	$(BUILD_DIR)/coro_selftest.exe

# The same selftest on the stack-switch backend Android uses (bionic has no swapcontext), built
# for this POSIX host with SR_CORO_BACKEND_ASM. x86-64 and AArch64 System V hosts only; the
# Windows fiber backend has no such variant, so the target refuses there.
coro-selftest-asm:
ifeq ($(OS),Windows_NT)
	$(error coro-selftest-asm needs a POSIX host (Linux or WSL); Windows uses Win32 fibers)
else
	$(CC) $(CFLAGS) -DSR_CORO_BACKEND_ASM -o $(BUILD_DIR)/coro_selftest_asm.exe \
		tests/native/sr_coro_selftest.c src/rt/sr_coro.c $(LDFLAGS) -lm
	$(BUILD_DIR)/coro_selftest_asm.exe
endif

native-core-tests: coro-selftest
ifneq ($(OS),Windows_NT)
native-core-tests: coro-selftest-asm
endif

# ge-raster-ref-selftest — project-authored GE triangle setup and rasterization
# reference model (issue #696): 12.4 snapping, top-left coverage with a tiling
# oracle, leftmost-anchored affine planes over the 256-segment setup reciprocal
# framework, and the integer colour-factor product, under every SPEC_AMBIGUITY
# reading.  Strict C99; not wired into the renderer.  No game inputs.
ge-raster-ref-selftest:
	$(CC) -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc/rt \
		-o $(BUILD_DIR)/ge_raster_ref_selftest.exe tests/native/ge_raster_ref_selftest.c src/rt/ge_raster_ref.c -lm
	$(BUILD_DIR)/ge_raster_ref_selftest.exe

native-core-tests: ge-raster-ref-selftest

# hle-thread-selftest — executable production-HLE ThreadMan/IoFileMgr regression suite.
# Links real hle.c, includes real sched.c for a controlled synthetic scheduler
# world. ThreadMan handlers use sr_syscall's registered-NID path; the focused
# IoFileMgr fixture calls test-only wrappers around the same production handlers.
# No game input is required. The PGD implementation is linked for the production
# HLE descriptor teardown path; unrelated host subsystems are supplied by narrow stubs.
# hle-thread-selftest-build -- compile and link the bounded production-HLE selftest.
#
# The executable is split from its run because this test previously exhausted host RAM
# (a joiner parked with a loop that re-adopted the main coroutine every iteration). The
# corrected lifecycle path is now bounded from the inside: `16fbb0a` made main-coroutine
# adoption idempotent and `1d8d494` added lifecycle counters that fail fast if the runaway
# pattern returns. The instrumentation macro is defined by this target alone; no production
# target compiles it, and the test refuses to build without it. The normal target is a safe,
# bounded runnable gate (45603 checks in the current measured run, of which 82 come from the
# issue #88 interrupt-context conformance matrix in src/rt/intr_conformance.h); the --psp-oracle
# sub-mode below
# remains available when only one scalar production-HLE stream is needed.
# The selftest's scratch tree is the caller's BUILD_ROOT (tests/native/hle_thread_selftest.c,
# SR_SELFTEST_BUILD_ROOT), so a run from a scratch root never writes the checkout's build/.
HLE_SELFTEST_DEFINES := -DSR_HLE_THREAD_SELFTEST -DSR_CORO_LIFECYCLE_TEST -DSR_SCHED_LIVENESS_TEST -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\"
HLE_AOT_BOUNDARY_SELFTEST ?= 0
HLE_AOT_BOUNDARY_DEFINE := $(if $(filter 1,$(HLE_AOT_BOUNDARY_SELFTEST)),-DSR_HLE_AOT_BOUNDARY_TEST,)
HLE_THREAD_SELFTEST_SOURCE ?= tests/native/hle_thread_selftest.c
# hle.c includes atrac3p_bridge.h and calls into the PR-B decode bridge, so any
# target that compiles it needs the same include paths and bridge/decoder
# sources the $(BUILD_DIR)/hle.o rule and `compile` already use. Without the
# -I flags this target does not even reach the linker: avcodec.h fails on
# libavutil/attributes.h.

PSMF_FUZZ_ITERS ?= 0
MEDIA_FUZZ_ITERS ?= 0

# $(1) is the optional fuzz iteration count. native-core-tests expands this macro
# instead of recursing into $(MAKE), which would re-parse this Makefile (#735 item 7).
define PSMF_PRODUCER_SELFTEST_CMD
$(CC) $(CFLAGS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -o $(BUILD_DIR)/psmf_producer_selftest.exe src/rt/psmf_producer.c tests/native/psmf_producer_selftest.c
$(BUILD_DIR)/psmf_producer_selftest.exe $(if $(1),--fuzz-iters $(1),)
endef

psmf-producer-selftest:
	$(call PSMF_PRODUCER_SELFTEST_CMD,$(PSMF_FUZZ_ITERS))

# psmf-media-selftest -- the synthetic PSMF fixture through the real H.264 backend and into a
# guest display buffer.  The fixture is generated by the selftest itself (no retail media, no
# checked-in bitstream), so this target needs nothing but a compiler and, on Windows, the
# Media Foundation libraries the runtime already links.  Off Windows h264_null.c supplies the
# seam, the decode half reports SKIP, and the demux half still runs.
ifeq ($(OS),Windows_NT)
PSMF_MEDIA_LIBS := -lmfplat -lole32
else
PSMF_MEDIA_LIBS :=
endif

PSMF_MEDIA_RUNTIME_OBJS := $(filter-out $(BUILD_DIR)/driver.o $(HLE_OBJS) $(BUILD_DIR)/mpeg.o $(BUILD_DIR)/psmf_producer.o,$(RT_OBJS))
# iso_public.o is filtered out with the rest of the production-only objects: the fixture
# serves its synthetic PSMF through its own memory-backed iso_lookup/iso_read (the same
# pattern the executable HLE harness uses), so the player's source boundary stays testable
# without mounting an image.  Kept as a second pass rather than folded into the list above
# so the canonical filter-out tail stays byte-identical for tools/test_build_truth.py.
PSMF_MEDIA_RUNTIME_OBJS := $(filter-out $(BUILD_DIR)/iso_public.o $(BUILD_DIR)/iso_unavailable.o,$(PSMF_MEDIA_RUNTIME_OBJS))

# The link's object list goes through a response file. The shell silently drops the tail of a
# command longer than about 8191 bytes, and an absolute BUILD_ROOT puts 55 object paths on the
# link line (about 10.6 KB under a long scratch root, cut inside the last object name). make
# writes the list itself, so the link command carries one @file argument whatever the root length.
# -Wno-unused-function: unused static helpers, mostly src/rt/hle.c; 103 warnings without it (#735 item 9).
psmf-media-selftest: $(PSMF_MEDIA_RUNTIME_OBJS) $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o
	$(CC) $(CFLAGS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -c src/rt/psmf_producer.c -o $(BUILD_DIR)/psmf_producer_media_selftest.o
	$(CC) $(CFLAGS) $(FLIGHT_IDENTITY_DEFS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -ffunction-sections -fdata-sections -c tests/native/psmf_media_selftest.c -o $(BUILD_DIR)/psmf_media_selftest.o
	$(file >$(BUILD_DIR)/psmf_media_selftest.rsp,$(BUILD_DIR)/psmf_producer_media_selftest.o $(BUILD_DIR)/psmf_media_selftest.o $(PSMF_MEDIA_RUNTIME_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(FLIGHT_IDENTITY_DEFS) $(FUZZ_SAN_FLAGS) $(HLE_INCLUDES) -DSR_MPEG_MEDIA_SELFTEST -Isrc/rt -std=c11 -Wno-unused-function -ffunction-sections -fdata-sections \
		-o $(BUILD_DIR)/psmf_media_selftest.exe \
		$(HLE_SRCS) @$(BUILD_DIR)/psmf_media_selftest.rsp \
		$(PSMF_MEDIA_LIBS) $(LDFLAGS) $(LIBS) -Wl,--gc-sections
	$(BUILD_DIR)/psmf_media_selftest.exe $(if $(MEDIA_FUZZ_ITERS),--fuzz-iters $(MEDIA_FUZZ_ITERS),)

# Prove the guest pixel assertion detects a Csc implementation that returns success but writes
# nothing. The mutation is compiled only into this synthetic fixture translation unit.
# -Wno-unused-function: unused static helpers, mostly src/rt/hle.c; 103 warnings without it (#735 item 9).
psmf-media-selftest-csc-mutant: $(PSMF_MEDIA_RUNTIME_OBJS) $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o
	$(CC) $(CFLAGS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -c src/rt/psmf_producer.c -o $(BUILD_DIR)/psmf_producer_media_selftest_csc_mutant.o
	$(CC) $(CFLAGS) $(FLIGHT_IDENTITY_DEFS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -ffunction-sections -fdata-sections -DSR_MPEG_CSC_NO_WRITE_MUTANT -c tests/native/psmf_media_selftest.c -o $(BUILD_DIR)/psmf_media_selftest_csc_mutant.o
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(FLIGHT_IDENTITY_DEFS) $(FUZZ_SAN_FLAGS) $(HLE_INCLUDES) -DSR_MPEG_MEDIA_SELFTEST -Isrc/rt -std=c11 -Wno-unused-function -ffunction-sections -fdata-sections \
		-o $(BUILD_DIR)/psmf_media_selftest_csc_mutant.exe \
		$(BUILD_DIR)/psmf_producer_media_selftest_csc_mutant.o $(BUILD_DIR)/psmf_media_selftest_csc_mutant.o \
		$(HLE_SRCS) $(PSMF_MEDIA_RUNTIME_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o \
		$(PSMF_MEDIA_LIBS) $(LDFLAGS) $(LIBS) -Wl,--gc-sections
	@$(BUILD_DIR)/psmf_media_selftest_csc_mutant.exe > $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log 2>&1; result=$$?; \
		if test $$result -eq 0; then cat $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log; echo "Csc no-write mutant unexpectedly passed" >&2; exit 1; fi; \
		if ! grep -Fq "FAIL: ycbcr: CSC converts every pixel of a varied picture from the guest planes, nearest chroma" $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log; then cat $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log; echo "Csc mutant was not killed by the decoder-independent CSC write assertion" >&2; exit 1; fi; \
		if ! grep -Fq "SKIP: the YCbCr decode layout needs a real H.264 backend" $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log && \
		   ! grep -Fq "FAIL: ycbcr-decode: CSC writes the measured conversion of the decoded samples" $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log; then cat $(BUILD_DIR)/psmf_media_selftest_csc_mutant.log; echo "Csc mutant was not killed by the guest pixel assertion" >&2; exit 1; fi; \
		echo "psmf-media-selftest-csc-mutant: killed by the CSC write assertions"

# psmf-media-focused-selftest -- the same media selftest (#307), linked against six HLE files, not
# all twenty-seven: hle_mpeg.c and the presentation boundary its A/V measurement drives
# (display, audio, audio_codec, route, input), plus tests/native/hle_focus_media_stubs.c for the
# calls into the rest of HLE. tools/hle_focus_check.py then reads the executable's symbol table
# and fails if any excluded HLE file was linked. $(HLE_OBJS) are only the symbol reference.
# The runtime's src/core objects (nk_font, nk_json, nk_xb, nk_input_profile and the platform file)
# come from native-core-objects-core, which native-core-tests builds for the product core tests
# already: the same sources, no runtime-only define, so the focused link does not compile them a
# third time (tools/test_native_core_build.py counts one compile per flag set).
HLE_MEDIA_FOCUS_SRCS := src/rt/hle_mpeg.c src/rt/hle_display.c src/rt/hle_audio.c \
                        src/rt/hle_audio_codec.c src/rt/hle_route.c src/rt/hle_input.c
# NC_OBJ_DIR is set here because the prerequisite list below is read before the Native Product Core
# Tests block that used to define it.
NC_OBJ_DIR := $(BUILD_ROOT)/native-core
PSMF_MEDIA_FOCUSED_CORE_RT := $(addprefix $(BUILD_DIR)/,$(notdir $(patsubst %.c,%.o,$(filter src/core/%,$(RT_SRCS)))))
PSMF_MEDIA_FOCUSED_RT_OBJS := $(filter-out $(PSMF_MEDIA_FOCUSED_CORE_RT),$(PSMF_MEDIA_RUNTIME_OBJS))
PSMF_MEDIA_FOCUSED_OBJS := $(PSMF_MEDIA_FOCUSED_RT_OBJS) \
                           $(patsubst $(BUILD_DIR)/%,$(NC_OBJ_DIR)/core/%,$(PSMF_MEDIA_FOCUSED_CORE_RT))
psmf-media-focused-selftest: native-core-objects-core $(PSMF_MEDIA_FOCUSED_RT_OBJS) $(HLE_OBJS) $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o tests/native/hle_focus_media_stubs.c tools/hle_focus_check.py
	$(CC) $(CFLAGS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -c src/rt/psmf_producer.c -o $(BUILD_DIR)/psmf_producer_media_selftest.o
	$(CC) $(CFLAGS) $(FLIGHT_IDENTITY_DEFS) $(FUZZ_SAN_FLAGS) -Isrc/rt -std=c11 -Werror -ffunction-sections -fdata-sections -c tests/native/psmf_media_selftest.c -o $(BUILD_DIR)/psmf_media_selftest.o
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(HLE_INCLUDES) -DSR_MPEG_MEDIA_SELFTEST -Isrc/rt -std=c11 -Werror -c tests/native/hle_focus_media_stubs.c -o $(BUILD_DIR)/hle_focus_media_stubs.o
	$(file >$(BUILD_DIR)/psmf_media_focused_selftest.rsp,$(BUILD_DIR)/psmf_producer_media_selftest.o $(BUILD_DIR)/psmf_media_selftest.o $(PSMF_MEDIA_FOCUSED_OBJS) $(RT_GE_O) $(ATRAC3P_OBJS) $(BUILD_DIR)/atrac3p_bridge.o)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(FLIGHT_IDENTITY_DEFS) $(FUZZ_SAN_FLAGS) $(HLE_INCLUDES) -DSR_MPEG_MEDIA_SELFTEST -Isrc/rt -std=c11 -Wno-unused-function -ffunction-sections -fdata-sections \
		-o $(BUILD_DIR)/psmf_media_focused_selftest.exe \
		$(HLE_MEDIA_FOCUS_SRCS) $(BUILD_DIR)/hle_focus_media_stubs.o @$(BUILD_DIR)/psmf_media_focused_selftest.rsp \
		$(PSMF_MEDIA_LIBS) $(LDFLAGS) $(LIBS) -Wl,--gc-sections
	$(PYTHON) tools/hle_focus_check.py --exe $(BUILD_DIR)/psmf_media_focused_selftest.exe --objdir $(BUILD_DIR) \
		--include $(HLE_MEDIA_FOCUS_SRCS) --stub $(BUILD_DIR)/hle_focus_media_stubs.o --require hle_register_psmf_player_handlers sr_hle_test_psmf_av_reset
	$(BUILD_DIR)/psmf_media_focused_selftest.exe $(if $(MEDIA_FUZZ_ITERS),--fuzz-iters $(MEDIA_FUZZ_ITERS),)

# The hosted gate for the focused link (#307): native-core-tests runs on every pull request.
native-core-tests: psmf-media-focused-selftest

audio-selftest:
	$(CC) $(CFLAGS) -Isrc/rt -DSR_AUDIO_SELFTEST \
		src/rt/audio_sdl3.c src/rt/perf.c -lSDL3 -lm \
		-o $(BUILD_DIR)/audio_selftest$(EXE_EXT)
	$(BUILD_DIR)/audio_selftest$(EXE_EXT)

# audio-resampler-bench -- fixed-seed, synthetic-input timing and bit-exact hash of the
# sr_audio_resampler_* streaming converter. Uses $(CFLAGS) so RUNTIME_OPT (default -O0)
# selects the optimisation level for both sides of a comparison.
audio-resampler-bench:
	$(CC) $(CFLAGS) -Isrc/rt -o $(BUILD_DIR)/audio_resampler_bench$(EXE_EXT) \
		fixtures/audio_resampler_bench/audio_resampler_bench.c src/rt/audio_resample.c src/rt/perf.c -lm
	$(BUILD_DIR)/audio_resampler_bench$(EXE_EXT)

# -Wno-unused-function: unused static helpers, mostly src/rt/hle.c; 75 warnings without it (#735 item 9).
hle-thread-selftest-build: $(RT_GE_O) $(H264_SUB_OBJS) $(GENERIC_TITLE_CONFIG_HEADER) $(HLE_SRCS) $(HLE_HDRS) src/rt/nested_frames.c src/rt/nested_frames.h src/rt/stale_code.c src/rt/stale_code.h src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/audio_resample.c src/rt/audio_resample.h
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) -DSR_HLE_THREAD_SELFTEST -DSR_CORO_LIFECYCLE_TEST -DSR_SCHED_LIVENESS_TEST -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(HLE_AOT_BOUNDARY_DEFINE) \
		$(HLE_INCLUDES) \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wno-unused-function \
		$(LDFLAGS) -Wl,--gc-sections $(REPRODUCIBLE_LINK_FLAG) -o $(BUILD_DIR)/hle_thread_selftest.exe \
		$(HLE_THREAD_SELFTEST_SOURCE) $(HLE_SRCS) src/rt/ace.c src/rt/audio_resample.c src/rt/osk_overlay.c src/rt/flash0_font.c src/rt/pgf_public.c src/rt/guest_printf.c src/rt/archive_vfs.c src/core/nk_xb.c src/core/nk_json.c src/core/nk_font.c $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c src/rt/flight_recorder.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/sr_coro.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/mp3_bridge.c src/rt/vfpu_tables.c \
		src/rt/h264_mf.c src/rt/h264_null.c src/rt/h264_ffmpeg.c $(H264_SUB_OBJS) \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c $(LIBS)

hle-thread-selftest: hle-thread-selftest-build
	$(BUILD_DIR)/hle_thread_selftest.exe

aot-return-boundary-thread-selftest:
	$(MAKE) --no-print-directory hle-thread-selftest-build HLE_AOT_BOUNDARY_SELFTEST=1
	$(BUILD_DIR)/hle_thread_selftest.exe --aot-return-boundary

# hle-title-selftest — run the same production-HLE executable against the generic
# profile and both public fixture profiles. The optional configured profile is
# invoked by tools/test_hle_title_config_behavior.py with a temporary synthetic
# manifest; no title-specific fixture is committed merely to make this gate pass.
HLE_TITLE_SELFTEST_CONFIGS := generic fixture-a fixture-b
HLE_TITLE_SELFTEST_MANIFEST_generic :=
HLE_TITLE_SELFTEST_MANIFEST_fixture-a := assets/titles/pspdev-phase5.json
HLE_TITLE_SELFTEST_MANIFEST_fixture-b := assets/titles/synthetic.json
HLE_TITLE_CONFIG ?= generic
HLE_TITLE_MANIFEST ?= $(HLE_TITLE_SELFTEST_MANIFEST_$(HLE_TITLE_CONFIG))
HLE_TITLE_SELFTEST_DIR := $(BUILD_DIR)/title-config/hle-$(HLE_TITLE_CONFIG)
HLE_TITLE_SELFTEST_HEADER := $(HLE_TITLE_SELFTEST_DIR)/sr_title_config.h
HLE_TITLE_SELFTEST_CONFIG_ARG := $(if $(strip $(HLE_TITLE_MANIFEST)),--manifest $(strip $(HLE_TITLE_MANIFEST)),)
HLE_TITLE_SELFTEST_EXE := $(BUILD_DIR)/hle_title_production_selftest_$(HLE_TITLE_CONFIG).exe

hle-title-selftest:
	$(MAKE) --no-print-directory hle-title-selftest-one HLE_TITLE_CONFIG=generic HLE_TITLE_MANIFEST=
	$(MAKE) --no-print-directory hle-title-selftest-one HLE_TITLE_CONFIG=fixture-a HLE_TITLE_MANIFEST=assets/titles/pspdev-phase5.json
	$(MAKE) --no-print-directory hle-title-selftest-one HLE_TITLE_CONFIG=fixture-b HLE_TITLE_MANIFEST=assets/titles/synthetic.json

# -Wno-unused-function: unused static helpers, mostly src/rt/hle.c; 225 warnings without it across the three hle-title-selftest configs (#735 item 9).
hle-title-selftest-one: $(RT_GE_O) $(H264_SUB_OBJS) $(TITLE_CONFIG_TOOL) tools/title_manifest.py tests/native/hle_thread_selftest.c $(HLE_SRCS) $(HLE_HDRS) src/rt/audio_resample.c src/rt/audio_resample.h src/rt/osk_overlay.c src/rt/flash0_font.c src/rt/pgf_public.c src/rt/guest_printf.c src/rt/archive_vfs.c src/core/nk_xb.c src/core/nk_json.c src/rt/prx_loader.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC)
	"$(PYTHON)" $(TITLE_CONFIG_TOOL) $(HLE_TITLE_SELFTEST_CONFIG_ARG) --output $(HLE_TITLE_SELFTEST_HEADER)
	$(CC) $(CFLAGS) -I$(HLE_TITLE_SELFTEST_DIR) $(HLE_SELFTEST_DEFINES) $(HLE_INCLUDES) \
		-ffunction-sections -fdata-sections \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -Wno-unused-function \
		$(LDFLAGS) -Wl,--gc-sections $(REPRODUCIBLE_LINK_FLAG) -o $(HLE_TITLE_SELFTEST_EXE) \
		tests/native/hle_thread_selftest.c $(HLE_SRCS) src/rt/ace.c src/rt/audio_resample.c src/rt/osk_overlay.c src/rt/flash0_font.c src/rt/pgf_public.c src/rt/guest_printf.c src/rt/archive_vfs.c src/core/nk_xb.c src/core/nk_json.c src/core/nk_font.c $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c src/rt/flight_recorder.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/sr_coro.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/mp3_bridge.c src/rt/vfpu_tables.c \
		src/rt/h264_mf.c src/rt/h264_null.c src/rt/h264_ffmpeg.c $(H264_SUB_OBJS) \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c $(LIBS)
	$(HLE_TITLE_SELFTEST_EXE) --title-config

# Emit one scalar production-HLE comparison stream. Keep this target separate from the normal
# selftest so a deliberate FAIL remains visible in the record without turning the build recipe
# into a second standalone oracle implementation.
psp-oracle-nakagawa: hle-thread-selftest-build
	"$(PYTHON)" tools/psp_oracle/run_nakagawa.py --executable "$(BUILD_DIR)/hle_thread_selftest.exe" --output "$(PSP_ORACLE_OUTPUT)" -- --psp-oracle --case "$(PSP_ORACLE_CASE)" --artifact "$(BUILD_DIR)/hle_thread_selftest.exe" --source-commit "$(PSP_ORACLE_SOURCE_COMMIT)" --model "$(PSP_ORACLE_MODEL)" --firmware "$(PSP_ORACLE_FIRMWARE)"

psp-oracle: psp-oracle-nakagawa

# Production generated-code smoke stream.  The PSP-side ELF must be built from
# fixtures/psp_oracle with CASE=smoke first; this target then runs the normal
# codegen on that exact ELF and links the generated guest body into the existing
# production-HLE selftest executable.  No host-side sum implementation is used.
psp-oracle-nakagawa-smoke-generate: $(PSP_ORACLE_SMOKE_STAMP)

$(PSP_ORACLE_SMOKE_STAMP): $(PSP_ORACLE_SMOKE_ELF) tools/psp_oracle/build_nakagawa_smoke.py tools/codegen.py tools/analyze.py tools/analyze_cfg.py
	"$(PYTHON)" tools/psp_oracle/build_nakagawa_smoke.py --elf "$(PSP_ORACLE_SMOKE_ELF)" --out-dir "$(PSP_ORACLE_SMOKE_DIR)"
	"$(PYTHON)" -c "from pathlib import Path; Path(r'$(PSP_ORACLE_SMOKE_STAMP)').write_text('generated\n', encoding='ascii')"

$(PSP_ORACLE_SMOKE_HEADER) $(PSP_ORACLE_SMOKE_CHUNK) $(PSP_ORACLE_SMOKE_ADAPTER): $(PSP_ORACLE_SMOKE_STAMP)

# Smoke compile warnings (issue #735 item 9): no blanket -w.  The -Wno-<warning> flags on
# the compile line below are the only suppressions; each one has its reason here.
#   -Wno-unused-function: hle.c static helpers unused in this configuration (77 sites), as in the sibling selftest recipes.
#   -Wno-format-truncation: hle_thread_selftest.c test-path snprintf calls into 256/260-byte buffers; gcc cannot bound the %s arguments (13 sites).
#   -Wno-missing-field-initializers: intr_conformance.h IcProbe rows omit the trailing skip field on purpose.
#   -Wno-infinite-recursion: a generated guest loop in smoke_recomp_0.c ends in a dead tail call after an unconditional branch.
#   -Wno-sign-compare -Wno-unused-parameter -Wno-parentheses -Wno-pointer-sign: FFmpeg n4.4 ATRAC3/ATRAC3+ subset sources in src/rt/atrac3p; import deltas are documented there.
$(PSP_ORACLE_SMOKE_EXE): $(PSP_ORACLE_SMOKE_STAMP) $(H264_SUB_OBJS) $(PSP_ORACLE_SMOKE_HEADER) $(PSP_ORACLE_SMOKE_CHUNK) $(PSP_ORACLE_SMOKE_ADAPTER) tests/native/hle_thread_selftest.c $(HLE_SRCS) $(HLE_HDRS) src/rt/audio_resample.c src/rt/audio_resample.h src/rt/osk_overlay.c src/rt/flash0_font.c src/rt/pgf_public.c src/rt/guest_printf.c src/rt/archive_vfs.c src/core/nk_xb.c src/core/nk_json.c src/rt/prx_loader.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/sr_coro.c $(PGD_BACKEND_SRC) $(RT_GE_O) $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(HLE_SELFTEST_DEFINES) $(HLE_INCLUDES) -DSR_PSP_ORACLE_SMOKE \
		-ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables \
		-Wno-unused-function -Wno-format-truncation -Wno-missing-field-initializers \
		-Wno-infinite-recursion -Wno-sign-compare -Wno-unused-parameter -Wno-parentheses -Wno-pointer-sign \
		-I"$(PSP_ORACLE_SMOKE_DIR)" $(LDFLAGS) \
		-Wl,--gc-sections $(REPRODUCIBLE_LINK_FLAG) -o "$(PSP_ORACLE_SMOKE_EXE)" \
		tests/native/hle_thread_selftest.c $(HLE_SRCS) src/rt/ace.c src/rt/audio_resample.c src/rt/osk_overlay.c src/rt/flash0_font.c src/rt/pgf_public.c src/rt/guest_printf.c src/rt/archive_vfs.c src/core/nk_xb.c src/core/nk_json.c src/core/nk_font.c $(PLAYER_PLAT_SOURCES) src/rt/prx_loader.c src/rt/flight_recorder.c src/rt/nested_frames.c src/rt/stale_code.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/sr_coro.c src/rt/title_config.c src/rt/psmf_producer.c src/rt/savedata.c $(PGD_BACKEND_SRC) \
		src/rt/atrac3p_bridge.c $(ATRAC3P_SRCS) src/rt/mp3_bridge.c src/rt/vfpu_tables.c \
		src/rt/h264_mf.c src/rt/h264_null.c src/rt/h264_ffmpeg.c $(H264_SUB_OBJS) \
		src/rt/fbcap_policy.c $(RT_GE_O) src/rt/ge_capture.c \
		"$(PSP_ORACLE_SMOKE_DIR)/smoke_entry.c" "$(PSP_ORACLE_SMOKE_DIR)/smoke_recomp_0.c" $(LIBS)

psp-oracle-nakagawa-smoke-build: $(PSP_ORACLE_SMOKE_EXE)

psp-oracle-nakagawa-smoke: psp-oracle-nakagawa-smoke-build
	"$(PYTHON)" tools/psp_oracle/run_nakagawa.py --executable "$(PSP_ORACLE_SMOKE_EXE)" --output "$(PSP_ORACLE_SMOKE_OUTPUT)" -- --psp-oracle --case sum-1-to-100 --artifact "$(PSP_ORACLE_SMOKE_EXE)" --source-commit "$(PSP_ORACLE_SOURCE_COMMIT)" --model "$(PSP_ORACLE_MODEL)" --firmware "$(PSP_ORACLE_FIRMWARE)"

# dispatch-selftest — host-neutral unit tests for the guest code-address table (issue #45).# No game inputs needed; compiles tests/native/dispatch_selftest.c against the real primitives in
# dispatch_table.h. Asserts that guest address 0 is a first-class key (register/look up,
# hash collisions involving 0 in both orders, L1 caching, re-registration), that a real
# function at address 0 executes while an unregistered lookup does not, and that occupancy
# is independent of the key. Exit code 0 = all invariants hold.
dispatch-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/dispatch_selftest.exe tests/native/dispatch_selftest.c
	$(BUILD_DIR)/dispatch_selftest.exe

# stale-code-selftest — host-neutral unit tests for the TD-27 opt-in stale
# translated-code detector (src/rt/stale_code.*). No game inputs needed; links
# only the standalone detector TU. Runs twice: gate off (an overwritten
# invalidate must stay silent) and SR_STALE_DETECT=1 (an overwritten
# translated block must fire loudly, pristine invalidates must stay silent).
# Exit code 0 = all invariants hold.
stale-code-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/stale_code_selftest.exe tests/native/stale_code_selftest.c src/rt/stale_code.c
	$(BUILD_DIR)/stale_code_selftest.exe
	"$(PYTHON)" -c "import os,subprocess,sys; e=dict(os.environ); e['SR_STALE_DETECT']='1'; p=os.path.abspath(r'$(BUILD_DIR)/stale_code_selftest.exe'); sys.exit(subprocess.run([p], env=e).returncode)"

# cpu-lle-selftest — host-neutral unit tests for the LLE Phase 1 COP0,
# exception, and eret helpers (spec 3.2/3.3) plus the interpreter lane
# (spec 3.5). No game inputs needed; #includes recomp.c/cpu_lle.c for direct
# access to the exact helpers the generated code calls and links the real
# guest_interp.c, proving helper and interpreter behavior from one binary.
# Asserts exception EPC/Cause/EXL/BD (including delay-slot faults), eret
# return + EXL clear, user-mode traps, RI on unsupported encodings, Status /
# Cause write masking, vector selection/validation, and the LLE gate that
# keeps default-lane syscall/break fail-closed. Exit code 0 = all hold.
CPU_LLE_AOT_FIXTURE_C := $(BUILD_DIR)/cpu_lle_aot_flow_fixture.c

$(CPU_LLE_AOT_FIXTURE_C): tools/test_cpu_lle.py tools/codegen.py tools/analyze.py tools/analyze_cfg.py
	"$(PYTHON)" tools/test_cpu_lle.py --generate-lle-aot-flow-fixture $@

cpu-lle-selftest: $(GENERIC_TITLE_CONFIG_HEADER) $(CPU_LLE_AOT_FIXTURE_C)
	$(CC) $(CFLAGS) -DSR_INSTRUCTION_TRACE -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/cpu_lle_selftest.exe -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		tests/native/cpu_lle_selftest.c $(CPU_LLE_AOT_FIXTURE_C) src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/vfpu_tables.c src/rt/title_config.c src/rt/perf.c -lm
	$(BUILD_DIR)/cpu_lle_selftest.exe

# domain-mode-selftest — host-neutral unit tests for the LLE Phase 1
# per-domain HLE/LLE table and the sr_import_call() seam (spec section 4).
# Same white-box shape as cpu-lle-selftest: #includes recomp.c/cpu_lle.c/
# domain_mode.c for direct access to the exact seam the generated stubs call
# and links the real guest_interp.c, proving the LLE guest-export lane runs
# through the production linked-call boundary. No game inputs needed. Asserts
# HLE-everywhere defaults, set/get/reset/lock, the library table, NID/export
# registries (conflicts fail closed), HLE passthrough, COSIM accounting, LLE
# hit/miss/dispatch-reject, and fallback hit/miss/reject. Exit code 0 = all.
domain-mode-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/domain_mode_selftest.exe \
		tests/native/domain_mode_selftest.c src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/stale_code.c src/rt/vfpu_tables.c src/rt/title_config.c src/rt/perf.c -lm
	$(BUILD_DIR)/domain_mode_selftest.exe

# dispatch-isolation-selftest — executable proof that the TYPED dispatch binding a
# title configuration owns (dispatch aliases) acts only where that configuration names
# it. No game inputs needed; tests/native/dispatch_isolation_selftest.c
# #includes recomp.c and drives the real dispatch() entry point, asserting its observable
# effect on CpuState.
#
# Same three-configuration matrix as sched-selftest, and for the same reason: the SAME
# dispatch source is built against a generic, a fixture-A and a fixture-B configuration,
# so a binding that leaked back into generic dispatch fails at least one of the three.
# The two fixtures carry deliberately DISJOINT synthetic address families, so neither can
# satisfy the other's expectations by coincidence. Exit code 0 = all invariants hold.
DISPATCH_ISO_CONFIGS := generic fixture-a fixture-b
DISPATCH_ISO_MANIFEST_generic :=
DISPATCH_ISO_MANIFEST_fixture-a := assets/titles/pspdev-phase5.json
DISPATCH_ISO_MANIFEST_fixture-b := assets/titles/synthetic.json

dispatch-isolation-selftest:
	@$(foreach cfg,$(DISPATCH_ISO_CONFIGS),$(MAKE) --no-print-directory dispatch-isolation-selftest-one DISPATCH_ISO_CONFIG=$(cfg)$(NEWLINE)	)

# One configuration of the matrix. The generated header lands in its own directory so the
# three builds cannot share one.
DISPATCH_ISO_CONFIG ?= generic
DISPATCH_ISO_DIR := $(BUILD_DIR)/title-config/$(DISPATCH_ISO_CONFIG)
DISPATCH_ISO_MANIFEST := $(DISPATCH_ISO_MANIFEST_$(DISPATCH_ISO_CONFIG))
DISPATCH_ISO_CONFIG_ARG := $(if $(strip $(DISPATCH_ISO_MANIFEST)),--manifest $(strip $(DISPATCH_ISO_MANIFEST)),)

dispatch-isolation-selftest-one: $(TITLE_CONFIG_TOOL) tools/title_manifest.py
	"$(PYTHON)" $(TITLE_CONFIG_TOOL) $(DISPATCH_ISO_CONFIG_ARG) --output $(DISPATCH_ISO_DIR)/sr_title_config.h
	$(CC) $(CFLAGS) -I$(DISPATCH_ISO_DIR) $(LDFLAGS) \
		-o $(BUILD_DIR)/dispatch_isolation_selftest_$(DISPATCH_ISO_CONFIG).exe \
		tests/native/dispatch_isolation_selftest.c src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/guest_interp.c src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c src/rt/vfpu_tables.c src/rt/perf.c \
		$(LIBS) -lm
	$(BUILD_DIR)/dispatch_isolation_selftest_$(DISPATCH_ISO_CONFIG).exe

# memory-watch-selftest — exercise the opt-in shared AOT/interpreter store hook
# against the source-owned synthetic guest in dispatch_isolation_selftest.c: once with the
# watch on the interpreter's data word, once on a main-RAM word, where a store with nothing
# armed takes the accessors' inline path (every width there must still be reported).
memory-watch-selftest: export SR_WATCH = 0x00601000:4
memory-watch-selftest: DISPATCH_ISO_CONFIG = generic
memory-watch-selftest: dispatch-isolation-selftest-one
	$(BUILD_DIR)/dispatch_isolation_selftest_$(DISPATCH_ISO_CONFIG).exe --watch 0x08801000:4

native-core-tests: memory-watch-selftest

# code-capture-selftest — code a game installs at run time (#1933), end to end with no game
# inputs: tests/native/code_capture_selftest.c installs a synthetic routine in unowned RAM and
# calls it, which stops and, under SR_CODE_CAPTURE, writes a capture; tools/code_capture.py
# translates the capture; the same harness linked with that translation proves the binding
# takes the call only while memory holds the captured code. Exit code 0 = all hold.
CODE_CAPTURE_SELFTEST_DIR := $(BUILD_DIR)/code-capture-selftest
CODE_CAPTURE_SELFTEST_SRCS := src/rt/sr_coro.c src/rt/flight_recorder.c src/rt/guest_interp.c \
	src/rt/cpu_lle.c src/rt/domain_mode.c src/rt/stale_code.c src/rt/title_config.c \
	src/rt/vfpu_tables.c src/rt/perf.c

code-capture-selftest: $(GENERIC_TITLE_CONFIG_HEADER)
	"$(PYTHON)" -c "import pathlib, sys; d = pathlib.Path(sys.argv[1]); d.mkdir(parents=True, exist_ok=True); [p.unlink() for p in d.glob('*.nkcap')]" "$(CODE_CAPTURE_SELFTEST_DIR)"
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -o $(BUILD_DIR)/code_capture_selftest_capture.exe \
		tests/native/code_capture_selftest.c $(CODE_CAPTURE_SELFTEST_SRCS) -lm
	$(BUILD_DIR)/code_capture_selftest_capture.exe $(CODE_CAPTURE_SELFTEST_DIR)
	"$(PYTHON)" tools/code_capture.py translate --out $(CODE_CAPTURE_SELFTEST_DIR)/code_capture_unit.c $(CODE_CAPTURE_SELFTEST_DIR)
	$(CC) $(RECOMP_FLAGS) -Isrc/rt -DSR_SDL3VK -c $(CODE_CAPTURE_SELFTEST_DIR)/code_capture_unit.c \
		-o $(CODE_CAPTURE_SELFTEST_DIR)/code_capture_unit.o
	$(CC) $(CFLAGS) -I$(GENERIC_TITLE_CONFIG_DIR) $(LDFLAGS) -DCODE_CAPTURE_TRANSLATED \
		-o $(BUILD_DIR)/code_capture_selftest_bound.exe tests/native/code_capture_selftest.c \
		$(CODE_CAPTURE_SELFTEST_DIR)/code_capture_unit.o $(CODE_CAPTURE_SELFTEST_SRCS) -lm
	$(BUILD_DIR)/code_capture_selftest_bound.exe $(CODE_CAPTURE_SELFTEST_DIR)

native-core-tests: code-capture-selftest

# asset-index-selftest — host-neutral dynamic extracted-data index regression (issue #223).
# The production Windows HLE supplies the path enumeration and wide I/O; this target proves the
# shared ownership/growth/sort/lookup core with a synthetic long host path and no game input.
asset-index-selftest:
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc/rt \
		-o $(BUILD_DIR)/asset_index_selftest.exe tests/native/asset_index_selftest.c
	$(BUILD_DIR)/asset_index_selftest.exe

# gpu-coherence-selftest — Vulkan-backed production-path regression for CPU writes that
# overlap persistent GPU targets. The harness owns synthetic guest memory only; target
# acquire, dirty notification, reacquire, and readback all execute ge_gpu.c's real path.
# The scaled-upload case (#1296), the per-fragment mip level case (#1373), the readback-rows
# case (#323, also on its SR_GPU_READBACK_FULL reference path), the undrawn-rows case and the
# widescreen Fill case (#1881) run again alone at render scales 2 and 4; the sprite-scaled and
# uv-edge-clamp cases (#2110) run alone at scales 2, 3 and 4. Each is selected by arguments: a
# VAR=value prefix needs a POSIX shell, and cmd.exe does not take one.
GPU_COHERENCE_BIN := $(BUILD_DIR)/gpu_coherence_selftest.exe

$(GPU_COHERENCE_BIN): shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -DSR_GPU_COHERENCE_SELFTEST -ffunction-sections -fdata-sections -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(LDFLAGS) -Wl,--gc-sections -o $(GPU_COHERENCE_BIN) \
		tests/native/gpu_coherence_selftest.c src/rt/ge_capture.c src/rt/ge_raster_ref.c $(RT_GE_O) src/rt/flight_recorder.c src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c src/rt/gpu_sdl3vk/ace_hud.c $(LIBS)

gpu-coherence-selftest: $(GPU_COHERENCE_BIN)
	$(GPU_COHERENCE_BIN)
	$(GPU_COHERENCE_BIN) --scale 2 --case scaled-upload
	$(GPU_COHERENCE_BIN) --scale 4 --case scaled-upload
	$(GPU_COHERENCE_BIN) --scale 2 --case mip-lod
	$(GPU_COHERENCE_BIN) --scale 4 --case mip-lod
	$(GPU_COHERENCE_BIN) --scale 2 --case readback-rows
	$(GPU_COHERENCE_BIN) --scale 4 --case readback-rows
	$(GPU_COHERENCE_BIN) --scale 4 --case readback-rows --readback-full 1
	$(GPU_COHERENCE_BIN) --scale 2 --case undrawn-rows
	$(GPU_COHERENCE_BIN) --scale 4 --case undrawn-rows
	$(GPU_COHERENCE_BIN) --scale 4 --case undrawn-rows --readback-full 1
	$(GPU_COHERENCE_BIN) --scale 2 --case wide-fill
	$(GPU_COHERENCE_BIN) --scale 4 --case wide-fill
	$(GPU_COHERENCE_BIN) --gpu-timestamps unset --case gpu-timestamps-off
	$(GPU_COHERENCE_BIN) --gpu-timestamps 0 --case gpu-timestamps-off
	@rc=0; $(GPU_COHERENCE_BIN) --gpu-timestamps 1 --case gpu-timestamps || rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP gpu-coherence-selftest gpu-timestamps (exit 77; the reason is printed above)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL gpu-coherence-selftest gpu-timestamps (exit $$rc)"; exit $$rc; fi
	$(GPU_COHERENCE_BIN) --scale 2 --case sprite-scaled
	$(GPU_COHERENCE_BIN) --scale 3 --case sprite-scaled
	$(GPU_COHERENCE_BIN) --scale 4 --case sprite-scaled
	$(GPU_COHERENCE_BIN) --scale 2 --case uv-edge-clamp
	$(GPU_COHERENCE_BIN) --scale 3 --case uv-edge-clamp
	$(GPU_COHERENCE_BIN) --scale 4 --case uv-edge-clamp
	@$(MAKE) --no-print-directory gpu-coherence-selftest-snapbar-mutants

# gpu-coherence-selftest-snapbar-mutants — the submit-batch case must kill each
# SR_GPU_SNAPBAR_MUTANT in ge_gpu.c: 1 records one barrier command per image (the stream before
# the pair), 2 leaves the target SHADER_READ_ONLY after a copy op, 3 drops the write from the
# barriers' destination access. Each builds its own binary and must fail with its own message;
# exit 77 (no Vulkan device) is a SKIP, as for the selftest itself.
gpu-coherence-selftest-snapbar-mutants: shader-verify $(RT_GE_O)
	@for m in 1 2 3; do \
		case $$m in \
			1) kill="barrier commands, want";; \
			2) kill="copy ops left the target outside COLOR_ATTACHMENT_OPTIMAL";; \
			*) kill="weaker than to_layout";; \
		esac; \
		bin=$(BUILD_DIR)/gpu_coherence_snapbar_mutant$$m.exe; \
		$(CC) $(CFLAGS) -DSR_GPU_COHERENCE_SELFTEST -DSR_GPU_SNAPBAR_MUTANT=$$m -ffunction-sections -fdata-sections \
			-DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" $(LDFLAGS) -Wl,--gc-sections -o $$bin \
			tests/native/gpu_coherence_selftest.c src/rt/ge_capture.c src/rt/ge_raster_ref.c $(RT_GE_O) src/rt/flight_recorder.c src/rt/perf.c \
			$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c src/rt/gpu_sdl3vk/ace_hud.c $(LIBS) || exit 1; \
		$$bin --case submit-batch > $$bin.log 2>&1; result=$$?; \
		if test $$result -eq 77; then echo "snapbar mutant $$m: SKIP (no Vulkan device)"; exit 77; fi; \
		if test $$result -eq 0; then cat $$bin.log; echo "snapbar mutant $$m unexpectedly passed" >&2; exit 1; fi; \
		if ! grep -Fq "$$kill" $$bin.log; then cat $$bin.log; echo "snapbar mutant $$m failed for another reason" >&2; exit 1; fi; \
		echo "gpu-coherence-selftest-snapbar-mutants: mutant $$m killed ($$kill)"; \
	done

# gpu-snapsync-selftest — production-path regression for the explicit guest-VRAM
# snapshot boundary. It proves ordinary presentation remains async, then verifies
# target-scoped synchronization closes the generation gap and rejects unsafe geometry.
# gpu-ace-selftest - Ace Mode's texture filtering (src/rt/ace.h) on a real device: SR_ACE asks for
# 16x before the device is made; through-mode draws keep the measured PSP sample points and get
# no anisotropic sampler, a transformed linear draw gets one, a nearest one does not. Exit 77
# (SKIP) without Vulkan or without anisotropic filtering.
gpu-ace-selftest: shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -DSR_GPU_COHERENCE_SELFTEST -DSR_GPU_ACE_SELFTEST -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		-ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections \
		-o $(BUILD_DIR)/gpu_ace_selftest.exe \
		tests/native/gpu_coherence_selftest.c src/rt/ge_capture.c src/rt/ge_raster_ref.c $(RT_GE_O) src/rt/flight_recorder.c src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c src/rt/gpu_sdl3vk/ace_hud.c $(LIBS)
	@rc=0; $(BUILD_DIR)/gpu_ace_selftest.exe || rc=$$?; \
	if [ $$rc -eq 77 ]; then echo "SKIP gpu-ace-selftest (exit 77; the reason is printed above)"; \
	elif [ $$rc -ne 0 ]; then echo "FAIL gpu-ace-selftest (exit $$rc)"; exit $$rc; fi

gpu-snapsync-selftest: shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -DSR_GPU_COHERENCE_SELFTEST -DSR_GPU_SNAPSHOT_SYNC_SELFTEST -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		-ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections \
		-o $(BUILD_DIR)/gpu_snapsync_selftest.exe \
		tests/native/gpu_coherence_selftest.c src/rt/ge_capture.c src/rt/ge_raster_ref.c $(RT_GE_O) src/rt/flight_recorder.c src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c src/rt/gpu_sdl3vk/ace_hud.c $(LIBS)
	$(BUILD_DIR)/gpu_snapsync_selftest.exe

# gpu-capture-selftest — deterministic present-source capture regression (issue #57): the
# production present path is armed and driven with synthetic pixels; the published P6 PPMs
# are byte-checked (header, channel order, row pitch, no trailing bytes). Exit 77 = SKIP
# when Vulkan or the validation layer is unavailable.
GPU_CAPTURE_BIN := $(BUILD_DIR)/gpu_capture_selftest.exe

$(GPU_CAPTURE_BIN): shader-verify
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -DSR_SELFTEST_BUILD_ROOT=\"$(BUILD_ROOT)\" \
		$(LDFLAGS) -Wl,--gc-sections -o $(GPU_CAPTURE_BIN) \
		tests/native/gpu_capture_selftest.c src/rt/perf.c \
		$(SDL3VK_SRCS) $(LIBS)

gpu-capture-selftest: $(GPU_CAPTURE_BIN)
	$(GPU_CAPTURE_BIN)
	SR_GPU_CAPTURE_TERMINAL_CASE=surface-lost $(GPU_CAPTURE_BIN)
	SR_GPU_CAPTURE_TERMINAL_CASE=unclassified $(GPU_CAPTURE_BIN)
	SR_GPU_CAPTURE_TERMINAL_CASE=device-lost $(GPU_CAPTURE_BIN)

# gpu-selftest-status — outcome-preserving status seam for the GPU selftest binaries.
# A failing recipe always leaves Make with its own generic status, so a child SKIP
# (exit 77, "Vulkan or the validation layer unavailable") cannot travel out through a
# Make exit code and would be misread as a failure. This target therefore prints one
# machine-readable line per binary:
#
#     GPU_SELFTEST_RESULT <name> PASS|SKIP|FAIL <exit-code>
#
# and exits nonzero only when a binary ran and failed for a real reason, so a build
# failure and an executed semantic failure stay failures while a runtime SKIP stays a
# SKIP. GPU_SELFTEST_BINS/GPU_SELFTEST_PREREQS are overridable so the status logic can
# be exercised with synthetic children on hosts without Vulkan.
GPU_SELFTEST_BINS ?= $(GPU_COHERENCE_BIN) $(GPU_CAPTURE_BIN)
GPU_SELFTEST_PREREQS ?= $(GPU_COHERENCE_BIN) $(GPU_CAPTURE_BIN)

gpu-selftest-status: $(GPU_SELFTEST_PREREQS)
	@status=0; \
	for sr_bin in $(GPU_SELFTEST_BINS); do \
	  sr_name=`basename $$sr_bin`; \
	  sr_rc=0; \
	  "$$sr_bin" || sr_rc=$$?; \
	  case $$sr_rc in \
	    0) sr_verdict=PASS ;; \
	    77) sr_verdict=SKIP ;; \
	    *) sr_verdict=FAIL; status=1 ;; \
	  esac; \
	  echo "GPU_SELFTEST_RESULT $$sr_name $$sr_verdict $$sr_rc"; \
	done; \
	exit $$status

# Standalone seconds-scale GE fixture replay. Fixtures are private game-derived inputs and
# stay ignored; this target builds only the generic reader/rasterizer/backend executable.
ge-replay: shader-verify $(RT_GE_O)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections $(LDFLAGS) -Wl,--gc-sections \
		-o $(BUILD_DIR)/ge_replay.exe \
		src/rt/ge_replay.c src/rt/ge_capture.c $(RT_GE_O) src/rt/flight_recorder.c src/rt/perf.c \
		$(SDL3VK_SRCS) src/rt/gpu_sdl3vk/ge_gpu.c src/rt/gpu_sdl3vk/ace_hud.c $(LIBS)

# selftest — compile and run the C++ reference interpreter unit tests.
# Requires g++ with C++17. Exit code 0 = all tests passed.
selftest:
	g++ -std=c++17 -O1 -Isrc/ref -Isrc/rt -o $(BUILD_DIR)/selftest.exe \
		src/ref/selftest.cpp src/ref/interp.cpp src/ref/run_elf.cpp \
		-DSR_SELFTEST_ONLY -fno-exceptions
	$(BUILD_DIR)/selftest.exe
	@echo "selftest passed"
