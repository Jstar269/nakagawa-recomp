// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#ifndef NK_ENV_REGISTRY_H
#define NK_ENV_REGISTRY_H

/* The runtime's environment variables, one row each: name, kind, default, reading files, purpose.
 * A pure table (an X-macro, no definitions), so including it costs nothing: expand it with your
 * own X to build a list, a count or a help text.
 *
 * kind:  flag-presence  any value, even "0", turns it on (read as getenv(name) != NULL)
 *        flag-nonzero   on when set, non-empty and not "0"
 *        number         parsed integer or float
 *        path           file or directory
 *        string         free text or one of a few words
 *        list           comma- or colon-separated values, or a bitmask
 *        system         set by the host, read for its value (TEMP, TERM, SDL_*)
 *
 * Adding a switch means adding its row here; tools/test_env_registry.py fails on a name that is read
 * in src/rt but missing from the table, on a row nothing reads, and on a malformed row. A name ending
 * in _N stands for a numbered family (SR_WATCH_0 .. SR_WATCH_15). The readers keep the literal name
 * in their getenv calls: the source tests assert those literals. */
#define NK_ENV_REGISTRY(X) \
    X("ATRAC3P_FIXTURE", "path", "unset: the fixture test is skipped", "atrac3p_selftest.c", "Directory holding stream.bin and meta.txt that the ATRAC3+ self-test decodes as an extra fixture.") \
    X("FUZZ_CONSTRAINT", "string", "unset (finite floats only)", "vfpu_fuzz.c", "The value allow_nan_inf makes the VFPU fuzzer also generate NaN and infinity operands.") \
    X("FUZZ_SEED", "number", "unset (seed from argv or built-in value)", "vfpu_fuzz.c", "Seeds the random generator of the VFPU fuzz test tool; parsed with strtoul base 0 and overrides the argument.") \
    X("FUZZ_TRIALS", "number", "200 (or first command-line argument)", "vfpu_fuzz.c", "Sets the number of random trials run by the VFPU fuzz test program.") \
    X("PSP_ISO", "path", "unset: no ISO path from the environment", "iso_public.c", "Path of the disc image the public ISO reader opens when no image is given another way.") \
    X("PSP_VFPU_TABLES", "path", "assets/vfpu", "vfpu_tables.c", "Directory the runtime loads the VFPU lookup tables from instead of the default one.") \
    X("SCHED_DUMP", "flag-presence", "off", "sched.c", "Diagnostic switch read in sched.c that enables scheduler state dumping in the idle loop.") \
    X("SDL_VIDEODRIVER", "system", "unset (SDL picks the driver)", "gui.c", "Read with SDL_VIDEO_DRIVER to detect the offscreen driver for the headless presented-frames acceptance mode.") \
    X("SDL_VIDEO_DRIVER", "system", "unset", "gui.c", "Alias of SDL_VIDEODRIVER; a value of offscreen makes headless acceptance fail because a real presenter is required.") \
    X("SR_ACE", "string", "off", "ace.c", "Spec string that turns on the optional image-quality enhancements (filtering, text, readout, notice, wide, smooth).") \
    X("SR_ACE_REPORT", "path", "unset (no report written)", "ace.c", "Output path for the per-run Ace Mode JSON report.") \
    X("SR_ALLOC_GUARD", "flag-nonzero", "on (guard enabled when unset)", "hle.c", "Allocator guard stays on unless set with a value starting with 0, which disables it.") \
    X("SR_ALLOC_MAX", "number", "0xFFFFFFFF (no extra cap)", "hle.c", "Hexadecimal optional stricter cap on the size of a user-partition allocation request.") \
    X("SR_ALLOC_TRACE", "flag-presence", "off", "hle.c,recomp.c", "When set, prints a trace line for guest memory allocations and partition frees to stderr.") \
    X("SR_ARGLOG", "flag-presence", "off", "sched.c", "Logs a THREAD_ARG_COPY line to stderr when a started thread's argument block is copied.") \
    X("SR_ATRACLOG", "flag-presence", "off", "hle_audio_codec.c", "Enables ATRAC audio codec diagnostic logging.") \
    X("SR_AUDIODUMP", "path", "unset or empty: no dump written", "audio_unavailable.c", "Writes the mixed audio sent to the host device as a 16-bit PCM WAV file at this path.") \
    X("SR_AUDIOSTAT", "flag-presence", "off", "audio_unavailable.c,hle.c", "When set, enables audio statistics reporting.") \
    X("SR_AUDIO_BACKEND", "string", "unset (normal audio backend)", "audio_unavailable.c,psmf_media_selftest.c", "Audio backend choice; null, none, 0 or off select the silent null backend.") \
    X("SR_AUDIO_DISABLED", "string", "unset (audio enabled)", "audio_unavailable.c", "When set to exactly 1, host audio output is disabled and the null audio backend is used.") \
    X("SR_BLOCKLOG", "flag-presence", "off", "debug.h,sched.c", "Logs each time the scheduler parks a thread in a delay or object wait; read once per process.") \
    X("SR_BOOT_EVENT_FILE", "path", "unset (no event file)", "driver.c,gui.c,hle.c,hle_kernel_module.c,recomp.c", "File to which boot progress events are appended in addition to stderr.") \
    X("SR_BOOT_PATH", "path", "unset (no boot argument seeded)", "driver.c", "Guest boot path (under 4096 bytes) placed in the entry's argument block.") \
    X("SR_BREAKLOG", "flag-presence", "off", "debug.h", "Legacy switch that turns on the memory debug log class, like SR_OORLOG.") \
    X("SR_BREAK_FATAL", "flag-nonzero", "off: BREAK is logged and execution continues", "recomp.c", "Makes a guest BREAK instruction record a fatal event and abort the process.") \
    X("SR_CALLCOUNT", "flag-presence", "off", "hle.c", "When set, enables per-call counting of HLE function invocations.") \
    X("SR_CALLCOUNT_ALL", "flag-presence", "off (top 18 NIDs listed)", "hle.c", "Makes the HLE call-count report list every distinct NID instead of the top 18.") \
    X("SR_CBLOG", "flag-presence", "off", "debug.h,hle.c,hle_kernel_module.c,hle_kernel_thread.c,hle_umd.c", "Logs kernel callback creation, notification and registration events and enables the misc debug class.") \
    X("SR_CBSNAP", "flag-presence", "off", "sched.c", "Diagnostic switch read in sched.c that enables callback snapshots at each delivered vblank.") \
    X("SR_DATAROOT", "path", "unset", "hle_thread_selftest.c", "Extracted data root directory used to serve guest files from the host; the self-test saves and restores it.") \
    X("SR_DEBUG", "list", "unset (legacy per-subsystem variables decide)", "debug.h,hle.c", "Bitmask (strtoul, base 0) selecting debug log categories; 0x08 also enables GE display-list dumping.") \
    X("SR_DEBUG_THREAD_LOG", "flag-presence", "off", "hle_kernel_interrupt.c", "Diagnostic switch read in hle_kernel_interrupt.c for thread and interrupt logging.") \
    X("SR_DELAYLOG", "flag-presence", "off", "sched.c", "Logs scheduler thread-delay events; read once per process.") \
    X("SR_DEVELOPER_MODE", "flag-nonzero", "off", "gpu_sdl3vk/sdl3vk.c", "Marks a hand-started run as a developer run in the SDL3 Vulkan presenter.") \
    X("SR_DISC_ID", "string", "unset", "gui.c", "Disc identifier string shown in the in-game pause overlay, set by the launcher.") \
    X("SR_DISPLOG", "flag-presence", "off", "guest_interp.c,recomp.c", "Logs each guest dispatch with target, source pc and return address to stderr.") \
    X("SR_DLGLOG", "flag-presence", "off", "debug.h,hle_utility.c,savedata.c", "Prints diagnostic lines to stderr for utility dialogs and savedata dialog calls.") \
    X("SR_EXEC_SHARE_JSON", "path", "unset (no JSON written)", "perf.c", "File to which the execution-share profile is written as JSON at exit.") \
    X("SR_EXE_DIR", "path", "unset (executable directory is detected)", "hle_host.c", "Overrides the directory the host treats as the executable root; read as a wide string on Windows.") \
    X("SR_EXITSNAP", "flag-presence", "off", "hle_kernel_thread.c", "Arms capture of the last worker thread's CPU state at thread exit for later diagnostics.") \
    X("SR_EXIT_AT_VBLANK", "number", "0 (never exit)", "hle_display.c,sched.c", "Ends the run successfully once the vblank count reaches this value; also counts as pause automation.") \
    X("SR_EXPECT_HLE_DIAGNOSTICS", "string", "unset", "hle_thread_selftest.c", "Expected HLE diagnostics state that the title-config self-test compares against.") \
    X("SR_FBDIAG", "flag-presence", "off", "hle_display.c", "Prints a FRAMEBUF MISMATCH warning when the presented buffer differs from the GE draw target.") \
    X("SR_FBDUMP", "number", "off", "ge.c,hle_display.c", "Frame count (vcount) from which a framebuffer dump is captured; also enables the GE list snapshot.") \
    X("SR_FBSNAP", "number", "0 (off)", "debug.h,ge.c,hle_display.c", "Takes periodic framebuffer snapshots; a value below 2 selects 40 snapshots, otherwise the value is the snapshot count.") \
    X("SR_FBSNAP_AFTER", "number", "0", "hle_display.c", "Frame count after which framebuffer snapshots begin.") \
    X("SR_FBSNAP_WINDOWS", "list", "unset (no windows)", "hle_display.c", "Comma-separated lo-hi frame ranges (decimal) in which framebuffer snapshots are taken.") \
    X("SR_FIRST_FRAME_DUMP", "flag-presence", "off", "gui.c", "Writes the first presented frame's VRAM to a raw file in the working directory.") \
    X("SR_FLASH0_TEST_CASE", "string", "valid", "hle_thread_selftest.c", "Selects the synthetic flash0 font-test scenario: valid, tampered, missing-manifest, malformed-manifest or unlisted.") \
    X("SR_FLASH0_TEST_ROOT", "path", "unset (test requires it)", "hle_thread_selftest.c", "Synthetic flash0 root directory handed to the flash0 font self-tests.") \
    X("SR_FLIGHT", "string", "unset (flight recorder off)", "flight_recorder.c", "Flight recorder spec of the form classes;limit selecting event classes and a record limit.") \
    X("SR_FLIGHT_OUTPUT", "path", "flight-recorder.json", "flight_recorder.c", "File the flight recorder writes its JSON dump to.") \
    X("SR_FLIGHT_TRIGGER", "string", "unset: no trigger, recorder behaves as before", "flight_recorder.c", "Flight-recorder stop trigger spec: pc:ADDR, ge-draw:N, watch or checkpoint:KIND:N:AFTER.") \
    X("SR_FONTDIR", "path", "unset", "hle_thread_selftest.c", "Font cache directory; the flash0 self-test sets it to a test cache and restores it.") \
    X("SR_FONTLOG", "flag-presence", "off", "debug.h,flash0_font.c,hle_data.c,hle_font.c,hle_system.c", "Enables font and flash0 font-serving diagnostic logging to stderr.") \
    X("SR_FPS_CAP", "number", "native scanout rate (also for empty or 'native')", "gui.c", "Caps presented frames per second; values above 240 clamp to 240, 0 is accepted, invalid text uses native.") \
    X("SR_FSDIR", "path", "unset", "hle_thread_selftest.c", "Host directory used as the writable filesystem overlay; read here by self-tests that save and restore it.") \
    X("SR_FULLSCREEN", "flag-nonzero", "off (windowed)", "gpu_sdl3vk/sdl3vk.c", "When nonzero, the presenter asks SDL for desktop fullscreen before creating the Vulkan surface.") \
    X("SR_GAME_TITLE", "string", "unset (default window caption)", "driver.c,gui.c", "Title text for the window caption and pause overlay, sanitized before use; set by the launcher.") \
    X("SR_GEDUMP", "flag-presence", "off", "ge.c,hle.c", "Enables GE display list dump logging and framebuffer snapshots.") \
    X("SR_GELOG", "flag-presence", "off", "debug.h,ge.c,hle.c,hle_ge.c", "Enables GE command-list logging, including a message when a GE stall address is reached.") \
    X("SR_GEMAT", "flag-presence", "off", "ge.c", "When set, prints the world and view matrices of the first 16 primitives to stderr.") \
    X("SR_GEMATW", "flag-presence", "off", "ge.c", "Traces the first 120 GE matrix register writes to stderr.") \
    X("SR_GESIGNALLOG", "flag-presence", "off", "ge.c", "Logs the first few GE signal command occurrences per behavior to stderr.") \
    X("SR_GESTAT", "flag-presence", "off", "ge.c", "Prints per-frame GE statistics once per 60-vblank window to diagnose culled or rejected geometry.") \
    X("SR_GEUNK", "flag-nonzero", "on", "ge.c", "Unhandled GE commands are reported once each unless this is set to 0.") \
    X("SR_GEWATCH", "flag-presence", "off", "debug.h,ge.c,hle_display.c", "Logs GE primitive details and interleaves present lines with GE list lines to expose draw order.") \
    X("SR_GEWATCH_AFTER", "number", "0", "ge.c,hle_display.c", "Frame number from which SR_GEWATCH GE and present watch output starts.") \
    X("SR_GEWATCH_LIMIT", "number", "240", "ge.c", "Maximum number of GE watch trace lines; a non-positive value falls back to 240.") \
    X("SR_GE_ARM_RECT", "list", "unset", "debug.c,ge.c", "Comma list x0,y0,x1,y1 giving a rectangle that arms GE last-writer tracking.") \
    X("SR_GE_CAPTURE_EXIT", "flag-nonzero", "off", "ge.c", "Exits the run once the armed GE capture has been written.") \
    X("SR_GE_CAPTURE_FRAME", "number", "unset (no capture)", "ge.c", "Selects the GE frame number to capture.") \
    X("SR_GE_CAPTURE_MIN_PRIMS", "number", "1", "ge.c", "Minimum primitive count a frame needs before the GE capture is written.") \
    X("SR_GE_CAPTURE_PATH", "path", "logs/ge_capture.ngef", "ge.c", "Output file path for the GE command capture.") \
    X("SR_GE_CPU_PROFILE", "flag-nonzero", "off", "ge.c", "Enables CPU-side GE profiling counters; SR_GPU_CPU_PROFILE also enables it.") \
    X("SR_GE_ENQUEUE_TRACE", "flag-presence", "off", "hle_route.c", "Enables tracing of GE list enqueue calls in the route code.") \
    X("SR_GE_ENQUEUE_TRACE_WINDOWS", "list", "unset: trace off", "hle_ge.c", "Comma-separated FIRST-LAST ranges of GE enqueue ordinals to trace; an invalid value disables the trace.") \
    X("SR_GE_PRIM_CALIBRATION", "flag-nonzero", "off", "ge.c", "Enables primitive-profile calibration when primitive profiling is on.") \
    X("SR_GE_PRIM_PROFILE", "flag-nonzero", "off", "ge.c", "Enables the GE primitive profile, which also reads SR_GE_PRIM_CALIBRATION.") \
    X("SR_GE_PRIM_PROFILE_STRIDE", "number", "512", "ge.c", "Sampling stride for the GE primitive CPU profile.") \
    X("SR_GE_REPLAY_WALL_PROFILE", "flag-nonzero", "off", "ge_replay.c", "Enables wall-clock phase profiling of GE replay; SR_GPU_CPU_PROFILE also enables it.") \
    X("SR_GE_STRIP_CACHE_DISABLE", "flag-nonzero", "off (strip cache enabled)", "ge.c", "When nonzero, disables the GE strip cache.") \
    X("SR_GE_TIMING", "string", "off (enabled only when exactly \"1\")", "hle.c", "Set to 1 to turn on the measured GE time model and print a GE_TIMING banner.") \
    X("SR_GE_TRANSITION_TRACE", "path", "off", "ge.c", "Writes a GE transition trace to this file; 1 uses a default log path and 0 or empty disables it.") \
    X("SR_GPU_CAPTURE_TERMINAL_CASE", "string", "unset: lifecycle self-test runs", "gpu_sdl3vk/sdl3vk.c", "Names a terminal-fault case for the GPU capture self-test; when set, the lifecycle self-test is skipped.") \
    X("SR_GPU_COHERENCE_CASE", "string", "unset (run all cases)", "gpu_sdl3vk/ge_gpu.c", "Restricts the GPU coherence self-test to one case; only scaled-upload is accepted.") \
    X("SR_GPU_CPU_PROFILE", "flag-nonzero", "off", "ge.c,ge_replay.c,gpu_sdl3vk/ge_gpu.c", "Enables CPU-side profiling of GPU and GE replay paths, with wall-phase stats in replay.") \
    X("SR_GPU_GE", "flag-nonzero", "off (software GE)", "gui.c", "Enables the GPU GE renderer when set to a non-empty value other than 0.") \
    X("SR_GPU_LOG", "flag-nonzero", "off", "gpu_sdl3vk/ge_gpu.c", "Enables verbose logging in the Vulkan GE backend and also turns on its statistics.") \
    X("SR_GPU_NORTT", "flag-nonzero", "off", "gpu_sdl3vk/ge_gpu.c", "When nonzero, changes how a texture sourced from a render target is bound in the GPU path.") \
    X("SR_GPU_PRESENT_DBG", "flag-presence", "off (first 3 early exits still logged)", "gpu_sdl3vk/ge_gpu.c", "Logs every GPU present call and every early-exit decline to stderr.") \
    X("SR_GPU_SCALE", "number", "1 (falls back to SR_RESOLUTION_SCALE)", "gpu_sdl3vk/ge_gpu.c", "Internal render scale multiplier for the GPU GE renderer.") \
    X("SR_GPU_STATS", "flag-nonzero", "off", "gpu_sdl3vk/ge_gpu.c", "Enables statistics output in the Vulkan GE backend.") \
    X("SR_GPU_SUBMIT_BATCH", "number", "SUBMIT_BATCH_MAX", "gpu_sdl3vk/ge_gpu.c", "Number of GPU ops per submit batch; values outside 1 to the maximum are ignored.") \
    X("SR_GPU_SYNC_SUBMIT", "flag-nonzero", "off (asynchronous submit)", "gpu_sdl3vk/ge_gpu.c", "When enabled, GPU command submission is synchronous instead of asynchronous.") \
    X("SR_GPU_TEX_SHADOW_DISABLE", "flag-nonzero", "off (texture shadow enabled)", "gpu_sdl3vk/ge_gpu.c", "Disables the GPU texture shadow when set to a non-empty value other than 0.") \
    X("SR_GPU_TNL", "string", "off", "ge.c", "Set to exactly 1 to hand eligible transform-mode triangles to the GPU backend for vertex transform.") \
    X("SR_GPU_XFER_RING_KB", "number", "built-in default ring size", "gpu_sdl3vk/ge_gpu.c", "Transfer ring size in KiB (0 disables, above 65536 ignored).") \
    X("SR_HEADLESS", "flag-nonzero", "off", "audio_unavailable.c,gui.c,sched.c", "Marks the run headless: silent audio, headless presenter and no pause UI (audio check wants exactly 1).") \
    X("SR_HEADLESS_ACCEPT_PRESENTED_FRAMES", "string", "unset (off)", "gui.c,sched.c", "Requests the headless presented-frames acceptance checkpoint; any non-empty value enables it.") \
    X("SR_HEAP_BASE", "number", "first page-aligned address after the loaded image", "hle.c", "Hexadecimal base address for the user heap partition.") \
    X("SR_HEAP_DIAG", "flag-presence", "off", "recomp.c", "When set, prints live heap blocks and rejected foreign frees to stderr.") \
    X("SR_HEAP_REUSE_OFF", "flag-presence", "off (heap free-list reuse enabled)", "recomp.c", "Disables guest heap block reuse so free is a no-op, restoring bump-only allocation.") \
    X("SR_HEAP_WATCH", "flag-presence", "off", "recomp.c", "Tracks first writes to freed allocator headers and reports them to stderr.") \
    X("SR_HLELOG", "flag-presence", "off", "debug.h,hle.c", "Enables general HLE call logging.") \
    X("SR_HLE_DIAGNOSTICS", "flag-presence", "off", "title_config.c", "When set, enables HLE diagnostics if the title config has a valid diagnostics profile.") \
    X("SR_HOST_TIME_ZONE", "string", "unset (time zone keeps its seed)", "hle.c", "Host time zone as <minutes>,<0|1> (offset and daylight flag); invalid values are ignored with a message.") \
    X("SR_HUD", "flag-nonzero", "off", "gpu_sdl3vk/sdl3vk.c,perf.c", "Enables the developer HUD and performance counters when set to a non-empty value other than 0.") \
    X("SR_INLOG", "flag-presence", "off", "debug.h,gpu_sdl3vk/sdl3vk.c,hle_input.c", "Logs controller input: published button changes in the SDL backend and HLE input calls.") \
    X("SR_IOLOG", "flag-presence", "off", "debug.h,hle_data.c,hle_io.c", "When set, logs file I/O operations to stderr and enables the file-system debug category.") \
    X("SR_LAUNCHER_DISPLAY_BOUNDS", "list", "unset (primary display)", "gpu_sdl3vk/sdl3vk.c,gui.c", "Display bounds as x,y,w,h used to open the launcher window on a chosen display.") \
    X("SR_LOOSE_CONTENT_ROOTS", "list", "unset (no loose content roots)", "hle_thread_selftest.c", "Serialized list of extra loose-content root directories bound alongside the primary root.") \
    X("SR_MASTER_VOLUME", "number", "100", "audio_unavailable.c", "Master audio volume as a percentage, converted to a gain by the audio backend.") \
    X("SR_MEMSTICK", "path", "memstick", "hle_thread_selftest.c,vfs_path.h", "Host directory that serves as the guest ms0: memory stick root.") \
    X("SR_MODULE_DIR", "path", "place_game_here/EXTRACTED/decrypted", "hle_kernel_module.c,hle_thread_selftest.c", "Directory holding the decrypted guest modules the runtime loads.") \
    X("SR_MPEGAV", "flag-presence", "off", "hle.c", "Enables MPEG/PSMF audio-video sync logging (also enabled by SR_MPEGLOG).") \
    X("SR_MPEGLOG", "flag-presence", "off", "debug.h,h264_ffmpeg.c,h264_mf.c,hle.c,hle_mpeg.c,mpeg.c", "Enables MPEG and H.264 decoder diagnostic logging in the video and MPEG HLE code.") \
    X("SR_MSGLOG", "flag-presence", "off", "hle_kernel_sync.c", "When set, logs message pipe creation and calls to stderr.") \
    X("SR_NAN_TRAP_CONTEXT", "flag-nonzero", "off", "debug.c", "Dumps scheduler CPU state and guest memory context when a NaN trap fires.") \
    X("SR_NAN_TRAP_LIMIT", "number", "SR_NAN_TRAP_DEFAULT_LIMIT", "debug.c", "Limit for the NaN trap, accepted from 0 to 1000000; invalid values are rejected.") \
    X("SR_NIDLOG", "flag-presence", "off", "debug.h,hle.c", "Logs each HLE call by NID; read once per process.") \
    X("SR_NO2DZ", "flag-presence", "off", "ge.c", "When set, disables the depth test for through-mode (2D) primitives.") \
    X("SR_NOATEST", "flag-presence", "off", "ge.c", "Skips the GE alpha test so every fragment passes.") \
    X("SR_NOBILINEAR", "flag-presence", "off", "ge.c", "Forces nearest-texel sampling in the software GE instead of bilinear filtering.") \
    X("SR_NOCULL", "flag-presence", "off", "ge.c,gpu_sdl3vk/ge_gpu.c", "Disables back-face culling in the software and Vulkan GE paths.") \
    X("SR_NOFOG", "flag-presence", "off", "ge.c", "When set, disables fog in the GE rasterizer.") \
    X("SR_NOH264", "flag-presence", "off (hardware or library H.264 decoder used when available)", "h264_ffmpeg.c,h264_mf.c,h264_null.c", "Disables the platform H.264 decoder backends so decoder creation fails.") \
    X("SR_NOINPUT", "flag-presence", "off", "hle_route.c", "Treats input as already present so the route does not wait for pad input.") \
    X("SR_NOLIGHT", "flag-presence", "off", "ge.c", "Diagnostic switch read in ge.c that disables lighting in the GE transform path.") \
    X("SR_NOVBPACE", "string", "empty or 0 (paced)", "sched.c", "Value 1 turns vblank pacing off (turbo); empty or 0 keeps pacing; anything else is rejected.") \
    X("SR_OORLOG", "flag-presence", "off", "debug.h,recomp.c", "Logs out-of-range guest memory accesses to stderr and enables the memory debug category.") \
    X("SR_OSK_SCRIPT", "path", "unset", "hle_thread_selftest.c,hle_utility.c", "Path to a script of scripted answers for the on-screen keyboard dialog.") \
    X("SR_OSK_TEXT", "string", "unset: on-screen keyboard is answered by a person", "hle_thread_selftest.c,hle_utility.c", "Answers every on-screen keyboard field with this text; CANCEL cancels every field.") \
    X("SR_PAD", "list", "0x0008 (START)", "debug.h,hle_route.c", "Hex button mask for the automatic input pulse; also enables the input debug category.") \
    X("SR_PADPERIOD", "number", "240", "hle_route.c", "Period in frames of the automatic pad pulse; values of 0 or less use 240.") \
    X("SR_PADSCRIPT", "path", "unset", "hle_route.c,sched.c", "Path to a pad input script loaded by the route code; its presence also affects pause behavior.") \
    X("SR_PADSCRIPT_READ_BUDGET", "number", "1800", "hle_route.c", "Vblank count a pad script may be read for; a value below 1 or not a number is refused.") \
    X("SR_PADSTART", "number", "0", "hle_route.c", "Frame number before which the automatic input pulses are held back.") \
    X("SR_PADWIDTH", "number", "4", "hle_route.c", "Width in frames of each automatic pad pulse; 0 or less disables the pulse.") \
    X("SR_PARTITION_TOP", "string", "unset", "hle.c", "Read with SR_HEAP_BASE when initializing the user memory partition bounds.") \
    X("SR_PAUSE_SCRIPT", "string", "unset: no pause script", "sched.c", "Pause script of <vcount>:<host_ms>[:user] entries separated by commas; an invalid value exits with an error.") \
    X("SR_PCSAMPLE", "flag-presence", "off", "sched.c", "When set, enables scheduler PC sampling.") \
    X("SR_PERF", "flag-nonzero", "off", "perf.c", "Enables runtime performance counters with stderr reporting; an explicit 0 also keeps CSV/JSON output off.") \
    X("SR_PERF_CSV", "path", "unset (no CSV)", "perf.c", "File the performance counters are written to as CSV; setting it enables perf collection.") \
    X("SR_PERF_JSON", "path", "unset: no JSON performance file", "perf.c", "File path to which the performance recorder writes its JSON report.") \
    X("SR_PIXWHO", "list", "unset", "ge.c", "Comma pair x,y selecting a pixel whose writers are reported.") \
    X("SR_PPM_DUMP", "path", "unset (no dump)", "driver.c", "Output file path for a 64x64 PPM dump of the framebuffer at exit.") \
    X("SR_PRESENT_SCALING", "string", "sharp", "gpu_sdl3vk/sdl3vk.c", "Present scaling mode: sharp, integer or bilinear; unknown values warn and use sharp.") \
    X("SR_PRESENT_TRACE", "flag-presence", "off", "gui.c,hle_display.c", "Emits a boot event for each presented frame on the offscreen sink and traces display presents.") \
    X("SR_PROFILE", "flag-nonzero", "off", "recomp.c", "When set and not 0, enables hot-path profiling with a dump at exit.") \
    X("SR_PROFILE_DUMP_VBLANKS", "number", "0 (no periodic dump)", "hle_display.c", "Dumps the profile every N vertical blanks.") \
    X("SR_PSMF_MEDIA_DEBUG", "flag-presence", "off", "psmf_media_selftest.c", "Prints media chain counters in the PSMF media selftest.") \
    X("SR_PSMF_SELFTEST_DEBUG", "flag-presence", "off", "psmf_producer_selftest.c", "Makes the PSMF producer self-test print extra statistics lines to stderr.") \
    X("SR_QUIET", "flag-nonzero", "off", "driver.c", "When nonzero, redirects stdout and stderr to the null device.") \
    X("SR_REAL_MODULE_START", "string", "unset (module_start runs for libfont only)", "hle_kernel_module.c,hle_thread_selftest.c", "1 lets module_start run for any module, 0 for none; other values are ignored with a warning.") \
    X("SR_REDUCE_MOTION", "string", "Automatic (system setting)", "gui.c,overlay/overlay_host.c", "Player's Reduce motion choice for the overlay, falling back to the platform preference.") \
    X("SR_RESOLUTION_SCALE", "number", "1", "gpu_sdl3vk/ge_gpu.c", "Internal render scale for the Vulkan GE backend; SR_GPU_SCALE is read first as the other spelling.") \
    X("SR_ROUTE_LEARN", "flag-presence", "off", "hle_route.c", "When set, enables route learning mode in the input route code.") \
    X("SR_ROUTE_NO_EXIT", "flag-presence", "off (route failure exits the process)", "hle_route.c", "Keeps the process running instead of exiting after a route failure report.") \
    X("SR_RTRACE", "flag-presence", "off", "ge.c", "Enables the GE rendering trace, with its frame budget set by SR_RTRACE_FRAMES.") \
    X("SR_RTRACE_FRAMES", "number", "2", "ge.c", "Number of frames traced per window when SR_RTRACE is set; a non-positive value gives 2.") \
    X("SR_SAMPLE_OUT", "path", "samples.csv", "hle_route.c", "Output CSV file for state samples taken by the sampling route.") \
    X("SR_SAMPLE_SPEC", "path", "unset (no state sampling)", "hle_route.c", "Path of a spec file listing guest state samples to take.") \
    X("SR_SASLOG", "flag-presence", "off", "hle_audio.c", "Enables SAS audio diagnostic logging.") \
    X("SR_SAVEDATA_LIST_SELECT", "string", "unset: the user picks from the list", "savedata.c", "Save name to choose automatically in the savedata list dialog; CANCEL cancels it.") \
    X("SR_STALE_DETECT", "flag-nonzero", "off", "stale_code.c", "When nonzero, enables stale translated-code detection.") \
    X("SR_STATLOG", "flag-presence", "off", "debug.h,hle_io.c", "Logs memory stick Getstat results to stderr.") \
    X("SR_STORE_CONTEXT_LIMIT", "number", "1", "debug.c", "Number of store-context entries kept, accepted from 1 to 1024.") \
    X("SR_STORE_CONTEXT_MEM", "string", "unset", "debug.c", "Debug spec naming a register, words and offset of memory to store as context, parsed with sscanf.") \
    X("SR_STORE_CONTEXT_PC", "number", "unset", "debug.c", "Guest PC (decimal or 0x hex) used as store context for the value watch logging.") \
    X("SR_SYSLOG", "flag-presence", "off", "debug.h,hle.c,sched.c", "Enables system and thread-exit diagnostic logging to stderr.") \
    X("SR_SYSTEM_REGISTRY", "path", "registry/system.json under the app data directory", "hle_data.c", "Overrides the location of the system registry file.") \
    X("SR_T111PC", "flag-presence", "off", "sched.c", "Enables a scheduler trace of a specific thread program counter, limited by SR_T111PC_MAX.") \
    X("SR_T111PC_MAX", "number", "built-in limit; accepts 1 to 256", "sched.c", "Maximum number of entries for the T111 PC trace; out-of-range values are ignored.") \
    X("SR_TEST_EXITGAME_ATEXIT", "flag-presence", "off", "hle_thread_selftest.c", "Self-test switch that registers an atexit probe in the exit-game test.") \
    X("SR_TEST_EXITGAME_INTERRUPT", "flag-presence", "off", "hle_thread_selftest.c", "Test hook in the HLE thread selftest that clears the current thread when set.") \
    X("SR_TEST_GUEST_MODULE_LOAD", "string", "unset: the test returns immediately", "hle_thread_selftest.c", "Selects the mode of the guest-module-load binding self-test.") \
    X("SR_TEXDUMP", "flag-presence", "off", "ge.c", "When set, enables texture dumping.") \
    X("SR_TEXDUMP_AFTER", "number", "0", "ge.c", "First GE frame from which textures are dumped when SR_TEXDUMP is set.") \
    X("SR_THLOG", "flag-presence", "off", "debug.h,sched.c", "Enables scheduler and thread logging, including the sched debug class.") \
    X("SR_TRACE", "path", "logs/trace_window.txt", "flight_recorder.c", "Output file for the PC trace window; used only when SR_TRACE_PC is set.") \
    X("SR_TRACE_F", "list", "unset", "flight_recorder.c", "Comma list of frame indices (each below 128) that open the trace window.") \
    X("SR_TRACE_LIMIT", "number", "0 (no limit)", "flight_recorder.c", "Maximum number of records written by the trace window (strtoul, base 0).") \
    X("SR_TRACE_PC", "string", "unset (no trace window)", "flight_recorder.c", "PC window spec for the trace window recorder, written to the SR_TRACE path.") \
    X("SR_TRACE_V", "list", "unset: no vblank indices", "flight_recorder.c", "Comma-separated vblank indices (below 128) in which the PC trace window records.") \
    X("SR_TRACK_LAST_WRITER", "flag-nonzero", "off", "debug.c", "When set and not 0, enables tracking of the last writer of memory words.") \
    X("SR_UI_ROOT", "path", "unset (overlay default)", "gui.c", "Root directory of the overlay UI pages and fonts.") \
    X("SR_USER_DATA_ROOT", "path", "platform data directory", "hle_host.c", "Absolute directory used as the user data root; an invalid value is refused.") \
    X("SR_VALUE_WATCH_N", "string", "unset", "debug.c", "Value watch slot N (0..15), formatted 0xVALUE,label; reports when the watched value is seen.") \
    X("SR_VBLANK_Q_US", "number", "16000", "sched.c", "Vblank pacing quantum in microseconds.") \
    X("SR_VBLOG", "flag-presence", "off", "debug.h,sched.c", "Enables per-VBLANK scheduler logging.") \
    X("SR_VDUMP", "flag-presence", "off", "ge.c", "Dumps GE vertex data for each vertex type seen.") \
    X("SR_VIDEO", "string", "unset: the Vulkan presenter is used", "gui.c,osk_text_entry.c,sched.c", "Selects the presenter: gdi for the Win32 window, offscreen for headless, otherwise Vulkan.") \
    X("SR_VRAMDUMP", "string", "unset (off)", "hle_display.c", "Selection of VRAM dumps to write; requires SR_VRAMDUMP_DIR to name an output directory.") \
    X("SR_VRAMDUMP_DIR", "path", "unset (VRAM dump refused)", "hle_display.c", "Existing output directory required when SR_VRAMDUMP selects VRAM dumps.") \
    X("SR_VSYNC", "number", "1 (vsync on)", "gpu_sdl3vk/sdl3vk.c", "Set to 0 to request presenting without vsync; unset or non-zero keeps vsync.") \
    X("SR_VULKAN_VALIDATION", "flag-nonzero", "off", "gpu_sdl3vk/sdl3vk.c", "Enables Vulkan validation layers when the value parses as a nonzero integer.") \
    X("SR_WAKELOG", "flag-presence", "off", "debug.h,hle.c,sched.c", "When set, logs thread wakeups to stderr and enables the misc debug category.") \
    X("SR_WATCH", "string", "unset (no watch)", "flight_recorder.c", "Memory watch region given as ADDR:LEN.") \
    X("SR_WATCHDOG_EXIT", "number", "0 (disabled)", "hle_display.c", "Aborts after this many vblanks with no new frame when above zero.") \
    X("SR_WATCHPOINTS_FILE", "path", "watchpoints.json", "debug.c", "JSON file listing memory watchpoints to load.") \
    X("SR_WATCH_CONTEXT_FPR", "list", "unset", "debug.c", "Comma pair index,value naming a floating-point register and value for watch context.") \
    X("SR_WATCH_CONTEXT_LIMIT", "number", "1", "debug.c", "Number of context entries kept for a memory watch; accepts 1 to 1024.") \
    X("SR_WATCH_CONTEXT_PC", "number", "unset", "debug.c", "Guest PC (any C integer base, up to 32 bits) used as the watch context.") \
    X("SR_WATCH_N", "string", "unset", "debug.c", "Memory watch slot N (0..15), formatted 0xSTART,0xEND,label.") \
    X("SR_WINDOW_HIDDEN", "flag-nonzero", "off", "gpu_sdl3vk/sdl3vk.c", "When nonzero, the presenter creates a hidden window for headless routes.") \
    X("SR_YIELDLOG", "flag-presence", "off", "sched.c", "Logs the first 200 worker-thread yields to stderr.") \
    X("TEMP", "system", "falls back to TMPDIR, then current dir or /tmp", "audio_unavailable.c,profiler_selftest.c", "Host temporary directory used for scratch files by the audio and profiler code.") \
    X("TMPDIR", "system", "unset: falls back to the current directory", "audio_unavailable.c,profiler_selftest.c,vfs_contained_selftest.c", "Host temp directory (after TEMP) where self-tests place their scratch files.")

#endif /* NK_ENV_REGISTRY_H */
