# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Curated classification metadata for the HLE registration manifest.

Plain Python data module on purpose, matching tools/compat_overrides.py: this
file is meant to be read and reviewed by humans in diffs, without a
serialization round-trip. tools/hle_manifest.py cross-checks every
entry here against the registrations actually extracted from src/rt/hle.c, so
stale handler names or NIDs fail the gate instead of rotting silently.

Classification model (per registered NID):
  fake_success           -- routed to a generic always-success handler (h_ok
                            family), a handler curated as `stub`, or a handler
                            mechanically detected by tools/hle_manifest.py as
                            doing nothing but `(void)s` / logging / `return 0`.
                            The call reports success but performs none of the
                            API's contract. These are the silent-corruption risks
                            issue #71 exists to surface.
  controlled_unsupported -- a dedicated handler that deliberately refuses the
                            operation with the API's own documented error
                            (e.g. the PSMF getters returning PSMF_ERR_NO_DATA
                            until the real demux is connected). Static refusals
                            registered with an explicit PSP error code are also
                            controlled_unsupported. Unsupported, but honest.
  dedicated              -- a handler written for this API. NOT a claim of
                            completeness; see HANDLER_STATUS.

Handler implementation-status values (HANDLER_STATUS):
  complete       -- believed to implement the exercised contract.
  partial        -- real implementation with known gaps.
  compatibility  -- works for the shipped title's usage; not general.
  stub           -- fabricates success; forces classification fake_success.
  controlled_unsupported -- refuses with the API's documented error; forces
                            classification controlled_unsupported.
  unreviewed     -- default for handlers nobody has yet audited. Reported
                    as `dedicated` with status `unreviewed`.
Only deviations from `unreviewed` are listed; keep entries evidence-based and
cite the issue that tracks finishing the handler where one exists.
"""

# Handlers that unconditionally fabricate success for every NID routed to
# them. Anything registered to one of these is classified fake_success.
GENERIC_SUCCESS_HANDLERS = {
    "h_ok",
}

HANDLER_STATUSES = {
    "complete",
    "partial",
    "compatibility",
    "stub",
    "controlled_unsupported",
    "unreviewed",
}

# Curated metadata per reviewed handler. Every handler named here must exist in
# hle.c's extracted registrations (tools/test_hle_manifest.py enforces it).
#
# Semantic status invariants enforced by tools/hle_manifest.py:
#   - 'complete': MUST carry a non-empty 'evidence' list citing contract source
#     (public ABI/doc, source-owned test name, hardware oracle id, or guest-module takeover).
#     Any referenced source files must exist in the repository tree.
#   - 'partial' and 'compatibility': MUST carry a non-empty 'limitation' string.
#     Handlers without recorded limitations must cite a factual gap from code or
#     be marked "limitation not yet reviewed (#341)".
HANDLER_METADATA = {
    # This marker handler is intercepted by sr_syscall; each registration
    # carries its PSP-visible refusal code in sr_hle_register_unsupported.
    "h_ControlledUnsupported": {
        "status": "controlled_unsupported",
        "description": "Intercepted by sr_syscall; carries refusal code in sr_hle_register_unsupported.",
    },
    # Stores g_sdk_version for SDK-dependent paths; the retained-state
    # contract for the variants routed to it is implemented.
    "h_SetCompiledSdkVersion": {
        "status": "complete",
        "evidence": [
            "src/rt/sdkver_selftest.c",
            "tools/test_sdkver_c.py",
            "public ABI (PSPSDK sdkver.h: sceKernelSetCompiledSdkVersion)",
        ],
        "description": "Stores g_sdk_version for SDK-dependent paths; retained-state contract verified across all registered variants.",
    },
    "h_SysClock2USec": {
        "status": "partial",
        "limitation": "uses the runtime's microsecond system-clock representation and splits it into low/high outputs; hardware conversion and error-precedence cells are not measured",
    },
    # Suspend/Resume/Rotate are scheduler operations: suspension is a flag on a thread
    # that keeps its wait semantics, rotation moves the head of one priority's ready
    # queue behind its peers.  Measured cells are the DORMANT, double-suspend and
    # resume-of-a-non-suspended-thread codes; the rest is project-defined.
    "h_SuspendThread": {
        "status": "partial",
        "limitation": "suspend is a scheduler flag (no nesting count); a waiting thread still completes its wait while suspended and runs only after resume; DORMANT and double-suspend codes are hardware measured, while the code for suspending UID 0 or the calling thread reuses the thread-object refusal code without a suspend-specific measurement, and interrupt/dispatch-context precedence is unmeasured",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_suspend_resume_errors_and_ready_thread",
            "src/rt/hle_thread_selftest.c:test_suspended_waiter_keeps_wait_semantics",
            "src/rt/sched_selftest.c:test_suspended_wait_still_completes",
        ],
    },
    "h_ResumeThread": {
        "status": "partial",
        "limitation": "resume clears the scheduler suspension flag and applies strict-priority preemption; the not-suspended and DORMANT codes are hardware measured, while resume of UID 0 (the running caller) answers the not-suspended code without a measurement and interrupt/dispatch-context precedence is unmeasured",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_suspend_resume_errors_and_ready_thread",
            "src/rt/sched_selftest.c:test_suspend_resume_error_codes",
        ],
    },
    "h_RotateThreadReadyQueue": {
        "status": "partial",
        "limitation": "rotates the cyclic slot-order ready queue of one priority (0 selects the caller's priority) and yields when the caller leads that queue; no range-error code is returned for an out-of-range priority because none is sourced, and ordering relative to threads that become ready after the rotation is not hardware measured",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_rotate_ready_queue_selection_order",
            "src/rt/hle_thread_selftest.c:test_rotate_equal_priority_yields_to_peers",
            "src/rt/sched_selftest.c:test_rotate_ready_queue_moves_head_behind_peers",
        ],
    },
    "h_SysClock2USecWide": {
        "status": "partial",
        "limitation": "takes the 64-bit clock as the $a0/$a1 pair and writes its low/high words through $a2/$a3 on the same microsecond representation as h_SysClock2USec; invalid-output-pointer error code and hardware conversion are not measured",
    },
    "h_ReferProfilerNull": {
        "status": "partial",
        "limitation": "no profiler is modeled, so both sceKernelReferThreadProfiler and sceKernelReferGlobalProfiler always report NULL, like firmware with profiling off; NULL-ness on retail firmware is not hardware-measured",
    },
    "h_CtrlGetSamplingMode": {
        "status": "partial",
        "limitation": "reports the mode retained by sceCtrlSetSamplingMode but does not change the sampled SceCtrlData; the invalid-pointer error code (ILLEGAL_ADDR) is not hardware measured",
    },
    "h_AllocMemoryBlock": {
        "status": "partial",
        "limitation": "models main-user-partition Low placement with the four-byte options header; valid High, Addr, LowAligned, HighAligned, and extended-options forms are refused, and allocation fragmentation/error precedence are not hardware measured",
    },
    "h_GetMemoryBlockAddr": {
        "status": "partial",
        "limitation": "resolves UIDs created by the modeled Low allocation path; output-pointer and unknown-UID results are source-tested but not hardware measured",
    },
    "h_FreeMemoryBlock": {
        "status": "partial",
        "limitation": "releases UIDs created by the modeled Low allocation path; invalid-UID result is source-tested but not hardware measured",
    },
    "h_DisplayWaitVblankStartMulti": {
        "status": "partial",
        "limitation": "waits the requested positive count of scheduler VBLANK periods; zero-count behavior fails closed as not implemented, while callback/context precedence and hardware timing are not measured",
    },
    # sceDisplayGetFramePerSec: writes the measured 60000/1001 float refresh
    # rate (59.9400599f) into $f0 under the unified display clock. The API has
    # no parameters; the full observable contract is the float bits.
    "h_DisplayGetFramePerSec": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_display_frame_per_sec_float_return",
            "tools/test_hle_manifest.py:FindingRuleTests.test_float_return_metadata_is_live_and_dedicated",
            "hardware oracle / issue #80 display-clock measurement (59.9400599f in $f0)",
        ],
        "description": "Writes measured 60000/1001 float refresh rate (59.9400599f) into $f0 under unified display clock.",
    },
    # These two handlers are reached by the source-owned display-smoke route.
    # Its fixed 8888 framebuffer flip and ordinary guest-thread vblank wait
    # establish that narrow production path, while the API edges below remain
    # explicitly outside the route's evidence (#341).
    "h_DisplaySetFrameBuf": {
        "status": "partial",
        "evidence": [
            "Makefile:display-smoke-run",
            "fixtures/display_smoke/generate.py:verify",
            "fixtures/display_smoke/generate.py:run",
        ],
        "limitation": "display-smoke covers only a sync=1 8888 flip with stride 512; other format, address, stride, and error-precedence cases remain outside this route (#341)",
    },
    "h_DisplayWaitVblankStart": {
        "status": "partial",
        "evidence": [
            "Makefile:display-smoke-run",
            "fixtures/display_smoke/generate.py:run",
            "tools/test_sched_invariants.py:test_the_two_display_nids_have_separate_handlers",
        ],
        "limitation": "display-smoke covers ordinary guest-thread waits; interrupt-context behavior and callback wait variants remain outside this route (#341)",
    },
    # sceGeListEnQueue is reached by the display-smoke route with one bounded,
    # unstalled list (PRIM, FINISH, END) that the host GE runs to completion at
    # enqueue. The flight recorder pins that single production path.
    "h_GeListEnQueue": {
        "status": "partial",
        "evidence": [
            "Makefile:display-smoke-run",
            "fixtures/display_smoke/generate.py:flight_smoke",
            "src/rt/hle_thread_selftest.c:test_flight_recorder_ge_present_events",
        ],
        "limitation": "display-smoke covers one unstalled synchronous list; ring-buffer stall deferral, a full list table (slot 0 is reused), argument and priority validation, and asynchronous execution timing remain outside this route (#341)",
    },
    "h_GeListEnQueueHead": {
        "status": "partial",
        "limitation": "idle-list execution follows the shared enqueue path; head ordering is refused while a list is stalled, and asynchronous queue behavior remains unmodeled",
    },
    "h_GeBreak": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ge_break_continue",
        ],
        "limitation": "models synchronous display list pause (mode 0) and queue cancellation (mode 1); argument and parameter buffer inspection are K1/read checked only, asynchronous hardware command boundary timing remains unmodeled (#341); the 0x80000025 result when no list is active and the paused (2) and cancelled (4) sync statuses are not hardware-measured",
    },
    "h_GeContinue": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ge_break_continue",
        ],
        "limitation": "resumes a paused display list using the synchronous GE runner; hardware timing and multi-queue priority ordering remain unmodeled (#341); the 0x80000025 result when no list is paused is not hardware-measured",
    },
    # scePsmfPlayerGetVideoData / GetAudioData. Both drive the project-authored
    # PSMF producer and a host codec backend, and return 0 only for output a
    # decoder actually produced: the video getter validates the caller's stride
    # and pointer contract, warms up the documented three frames, converts the
    # picture into the guest display buffer only after preflighting every
    # destination row, and reports the picture's presentation time; the audio
    # getter stages one decoded ATRAC3+ frame as 2048 stereo s16 samples. A
    # compressed access unit is never presented as a decoded frame or PCM, and
    # a stream that runs dry reports NO_MORE_DATA. They remain partial because
    # the player above them is still host HLE (original guest PRX execution is
    # the target), the picture path needs a host backend to exist at all (the
    # null backend yields no pictures by design), and no hardware-comparison
    # tier has been measured for either getter.
    "h_PsmfGetVideo": {
        "status": "partial",
        "limitation": "host HLE player; requires host codec backend; no hardware comparison tier measured (#341)",
    },
    "h_PsmfGetAudio": {
        "status": "partial",
        "limitation": "host HLE player; requires host codec backend; stages 2048 stereo s16 frames; no hardware comparison tier measured (#341)",
    },
    "h_MpegAvcQueryYCbCrSize": {
        "status": "partial",
        "limitation": "the YCbCr size formula, pointer preflight, and guest geometry are source-tested; firmware mode coverage and the hardware-oracle contract remain open (#302)",
    },
    "h_MpegAvcInitYCbCr": {
        "status": "partial",
        "limitation": "Init zeroes a modeled guest allocation; its plane bytes are not asserted to match the PSP header/plane layout (#302)",
    },
    "h_MpegAvcDecodeYCbCr": {
        "status": "partial",
        "limitation": "Decode marks decoded pictures ready; guest plane bytes remain modeled zeroes, not the PSP layout, while Csc writes the retained decoded picture (#302)",
    },
    "h_MpegAvcDecodeStopYCbCr": {
        "status": "partial",
        "limitation": "Stop clears tracked state, but the firmware buffered-picture status is explicitly unmeasured rather than reported as a guessed frame count (#302)",
    },
    "h_MpegAvcCopyYCbCr": {
        "status": "partial",
        "limitation": "Copy copies the modeled guest bytes and retained picture state; PSP plane layout and overlap behavior still need an oracle (#302)",
    },
    "h_MpegAvcCsc": {
        "status": "partial",
        "limitation": "Csc writes the retained decoded picture; guest plane bytes are modeled, not the PSP layout, and range/coherency behavior remains open (#302)",
    },
    "h_MpegQueryPcmEsSize": {
        "status": "partial",
        "limitation": "the public 320-byte LPCM ES/output sizes are retained, but no PCM stream is exposed by this runtime (#302)",
    },
    "h_MpegGetPcmAu": {
        "status": "controlled_unsupported",
        "description": "PCM access-unit production is deliberately refused with NO_DATA; no PCM decoder or hardware-oracle queue contract is available (#302)",
    },
    "h_MpegChangeGetAuMode": {
        "status": "partial",
        "limitation": "decode mode is validated and retained; skip mode fails closed because its queue/timestamp effect is not measured (#302)",
    },
    # SAS waveform/ATRAC3 entry points whose source codecs are not implemented
    # by this runtime. They validate the core/voice identity and return the
    # documented invalid-state error instead of fabricating success.
    "h_SasUnsupportedVoice": {
        "status": "controlled_unsupported",
        "description": "SAS waveform/ATRAC3 voice entry points without implemented codecs; returns invalid state error 0x80420002.",
    },
    # sceDmacMemcpy / sceDmacTryMemcpy. The measured contract is implemented and
    # regression-tested through production dispatch: the illegal-size and
    # illegal-address classes, whole-span validation with overflow-safe
    # arithmetic, failure atomicity (no byte written, no GPU dirty),
    # memmove-correct same-pointer and overlapping copies, and full-span copies
    # (the PSP-3000 size matrix copied fully valid spans completely through
    # 0x100000; there is no API-wide 0xC000 ceiling, see docs/ARCHITECTURE.md).
    # The handlers remain partial because concurrent-DMA BUSY behavior and the
    # precedence of validation for an invalid truncated tail are not established
    # by the available evidence.
    "h_DmacMemcpy": {
        "status": "partial",
        "limitation": "concurrent-DMA BUSY behavior and invalid truncated-tail validation precedence unmodeled (#303, #341)",
    },
    "h_DmacTryMemcpy": {
        "status": "partial",
        "limitation": "concurrent-DMA BUSY behavior and invalid truncated-tail validation precedence unmodeled (#303, #341)",
    },
    # This returns no error while the virtual ISO-backed UMD model reports its
    # always-ready PRESENT|READY|READABLE state. Other drive-error states remain
    # outside the modeled contract.
    "h_UmdGetErrorStat": {
        "status": "compatibility",
        "limitation": "returns no error under virtual ISO drive; other drive-error states unmodeled (#341)",
    },
    # Common Memory Stick devctls have explicit outputs; unsupported devices or
    # command pairs remain visible per pair and are tracked under issue #341.
    "h_IoDevctl": {
        "status": "partial",
        "limitation": "Memory Stick devctl callback events unmodeled (#341)",
    },
    "h_IoMkdir": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
            "src/rt/hle.c:h_IoMkdir",
        ],
    },
    "h_IoRemove": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
            "src/rt/hle.c:h_IoRemove",
        ],
    },
    # Async IoFileMgr requests retain per-fd results and callbacks. Their
    # deterministic import-boundary service interval is synthetic timing, not
    # a PSP scheduler measurement; open also returns its descriptor at submit.
    "h_IoOpenAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_fd_namespace"],
        "limitation": "open completes its host lookup at submission to return the PSP-visible fd; async timing and callback ordering are synthetic, not hardware-measured",
    },
    "h_IoReadAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP timing and callback ordering remain unmeasured",
    },
    "h_IoWriteAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP timing and callback ordering remain unmeasured",
    },
    "h_IoLseekAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; 64-bit seek timing and callback ordering are not hardware-measured",
    },
    "h_IoLseek32Async": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP timing and callback ordering remain unmeasured",
    },
    "h_IoIoctlAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; PSP ioctl timing and callback ordering remain unmeasured",
    },
    "h_IoCloseAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_fd_namespace"],
        "limitation": "request completion uses a deterministic synthetic import-boundary scheduler; close callback timing remains unmeasured",
    },
    "h_IoWaitAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the host operation runs when the guest waits; PSP blocking, wake, and callback interleaving remain unmeasured",
    },
    "h_IoWaitAsyncCB": {
        "status": "partial",
        "limitation": "callback dispatch is modeled at the wait boundary; PSP blocking and callback interleaving remain unmeasured",
    },
    "h_IoPollAsync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the pending interval is a deterministic synthetic import-boundary policy, not PSP-measured timing",
    },
    "h_IoGetAsyncStat": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the pending interval is a deterministic synthetic import-boundary policy, not PSP-measured timing",
    },
    "h_IoChangeAsyncPriority": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "priority selects queued host requests, but PSP priority ordering and its interaction with callbacks are not hardware-measured",
    },
    "h_IoSetAsyncCallback": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "completion queues the modeled thread callback; PSP callback argument and interleaving behavior remain unmeasured",
    },
    "h_IoRmdir": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "empty-directory removal and nonempty refusal are tested with synthetic roots; PSP error precedence beyond those cases remains unmeasured",
    },
    "h_IoChdir": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "the modeled current directory is per-thread and resolves contained Memory Stick paths; PSP behavior for other devices is not established",
    },
    "h_IoSync": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "Memory Stick handles are flushed and synced; other devices and nonzero unknown arguments fail closed with IO_SYNC_DEVICE or IO_SYNC_ARGUMENT",
    },
    "h_IoChstat": {
        "status": "partial",
        "evidence": ["src/rt/hle_thread_selftest.c:test_io_async_and_path_imports"],
        "limitation": "only the mode field is supported; attribute, size, and timestamp fields fail closed with IO_CHSTAT_NON_MODE_FIELDS",
    },
    "h_Memset": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_sysclib_memory_imports",
            "src/rt/hle.c:h_Memset",
            "public C library contract (memset)",
        ],
        "description": "Fills a fully validated guest span and returns the guest destination.",
    },
    "h_Strlen": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_sysclib_memory_imports",
            "src/rt/hle.c:h_Strlen",
            "public C library contract (strlen)",
        ],
        "description": "Returns the byte length through a guest NUL terminator within readable memory.",
    },
    "h_ReferThreadStatus": {
        "status": "partial",
        "limitation": "run clocks use host scheduler time and preemption/release counters are modeled rather than PSP-measured (#311)",
    },
    # Mailboxes (issue #339). Measured contracts live in docs/HARDWARE_ORACLE.md
    # (campaign psp-hw-20260917): unknown id 0x8002019B, timeout 0x800201A8 with
    # remaining 0, CancelReceiveMbx waiters 0x800201A9, empty poll 0x800201B2,
    # FIFO circular next links, attr 0x400 ascending msgPriority, waitType 5.
    # Each handler stays partial until its own named gaps are closed.
    "h_CreateMbx": {
        "status": "partial",
        "limitation": "NULL-name create error and interrupt-context placement unmeasured (#339, #341)",
    },
    "h_DeleteMbx": {
        "status": "partial",
        "limitation": "delete-while-waited is corroborated by campaign psp-hw-20260917 but not mailbox-measured; interrupt-context placement unmeasured (#339, #341)",
    },
    "h_SendMbx": {
        "status": "partial",
        "limitation": "invalid-message-pointer error class unmeasured; interrupt-context placement unmeasured (#339, #341)",
    },
    "h_ReceiveMbx": {
        "status": "partial",
        "limitation": "ILLEGAL_CONTEXT precedence for a blocking receive unmeasured; priority-mailbox circular linking not separately asserted beyond campaign facts (#339, #341)",
    },
    "h_ReceiveMbxCB": {
        "status": "partial",
        "limitation": "callback-dispatch interleaving during a mailbox wait unmeasured; ILLEGAL_CONTEXT precedence unmeasured (#339, #341)",
    },
    "h_PollMbx": {
        "status": "partial",
        "limitation": "invalid-message-pointer error class unmeasured (#339, #341)",
    },
    "h_CancelReceiveMbx": {
        "status": "partial",
        "limitation": "cancel with zero waiters and invalid numWait-thread pointer error class unmeasured (#339, #341)",
    },
    "h_ReferMbxStatus": {
        "status": "partial",
        "limitation": "size-field caller contract (whether Refer preserves or overwrites size) unmeasured (#339, #341)",
    },
    # sceRtc family (issue #341). Contract sources: the public PSPSDK header
    # psprtc.h (declarations, 0-on-success/<0-on-error, pspRtcCheckValidErrors
    # component ranges) and the firmware-measured PSPAutotests tests/rtc corpus
    # (convert.expected, rtc.expected, arithmetic.expected).  PSPAutotests
    # records firmware CRASHING on several NULL arguments (rtc.c/convert.c
    # "Crash." comments), so the NULL edges have no firmware return code to
    # match; the runtime fails closed with SCE_KERNEL_ERROR_ILLEGAL_ADDR there
    # and the tests declare that as a product boundary, not an equivalence.
    "h_RtcGetCurrentTick": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_time_domains_are_coherent",
            "src/rt/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle.c:h_RtcGetCurrentTick",
            "public PSPSDK contract (psprtc.h: sceRtcGetCurrentTick, 0 on success, <0 on error)",
            "PSPAutotests tests/rtc/rtc.expected (tick advances across a 2 ms delay)",
        ],
    },
    "h_RtcGetCurrentClock": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_time_domains_are_coherent",
            "src/rt/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle.c:h_RtcGetCurrentClock",
            "public PSPSDK contract (psprtc.h: sceRtcGetCurrentClock, tz is minutes from UTC)",
            "PSPAutotests tests/rtc/rtc.expected (0/+13/+60/-60/-600000/INT_MAX/-INT_MAX all return 0; -60 is one hour before the UTC baseline)",
        ],
    },
    "h_RtcGetCurrentClockLocal": {
        "status": "partial",
        "limitation": "timezone/daylight comes from the fixed UTC constant until #77 adds the settable system profile, so LocalTime matches only a UTC-configured PSP (#77, #80, #341)",
    },
    "h_RtcConvertUtcToLocal": {
        "status": "partial",
        "limitation": "runs on the fixed UTC timezone constant until #77 (non-UTC console local time unimplemented) and its checked-overflow failure class is not autotest-verified (#77, #341)",
    },
    "h_RtcConvertLocalToUtc": {
        "status": "partial",
        "limitation": "runs on the fixed UTC timezone constant until #77 (non-UTC console local time unimplemented) and its checked-overflow failure class is not autotest-verified (#77, #341)",
    },
    "h_RtcGetTick": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_rtc_conversion_errors_and_full_range",
            "src/rt/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle.c:h_RtcGetTick",
            "public PSPSDK contract (psprtc.h: sceRtcGetTick, pspRtcCheckValidErrors component ranges)",
            "PSPAutotests tests/rtc/convert.expected (year 0/10000 -> 0x800001fe with output untouched; year 10/9998/9999 exact ticks)",
        ],
    },
    "h_RtcSetTick": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_rtc_conversion_errors_and_full_range",
            "src/rt/hle_thread_selftest.c:test_rtc_pointer_validation_and_measured_conversions",
            "src/rt/hle.c:h_RtcSetTick",
            "public PSPSDK contract (psprtc.h: sceRtcSetTick, 0 on success, <0 on error)",
            "PSPAutotests tests/rtc/convert.expected (checkSetTick: 835072 -> 0001-01-01, 62135596800000000 -> 1970-01-01)",
        ],
    },
    "h_RtcGetWin32FileTime": {
        "status": "partial",
        "limitation": "cold-first-call error reporting may differ from firmware (PSPAutotests convert.c notes errors report properly only after a prior error and that the rules are hard to determine); component bounds beyond the measured year/epoch/day-carry cases stay fail-closed rather than measured (#341)",
    },
}

# handler name -> status mapping. Preserved for direct consumers and gate checks.
HANDLER_STATUS = {
    h: meta["status"] if isinstance(meta, dict) else meta
    for h, meta in HANDLER_METADATA.items()
}

# handler name -> evidence list (for complete handlers)
HANDLER_EVIDENCE = {
    h: meta.get("evidence", [])
    for h, meta in HANDLER_METADATA.items()
    if isinstance(meta, dict) and "evidence" in meta
}

# handler name -> limitation string (for partial/compatibility handlers)
HANDLER_LIMITATIONS = {
    h: meta.get("limitation", "")
    for h, meta in HANDLER_METADATA.items()
    if isinstance(meta, dict) and "limitation" in meta
}

# Alias-consistency rules: every static registration whose *registered name*
# starts with `name_prefix` must route to `required_handler`. This is how the
# gate rejects a firmware-variant NID that silently bypasses shared retained
# state (the sceKernelSetCompiledSdkVersion603_605 regression recorded on
# issue #71).
ALIAS_RULES = (
    {
        "name_prefix": "sceKernelSetCompiledSdkVersion",
        "required_handler": "h_SetCompiledSdkVersion",
        "why": "every SetCompiledSdkVersion firmware variant must update g_sdk_version",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/71",
    },
)

# Canonical names for NIDs with a history of being registered under a wrong
# or generic label. Source: current PPSSPP NID tables (public), mirrored by the
# tracked tools/nid_corpus.json and its generated src/rt/nid_names.h. A
# registration whose name disagrees with this map is a `mislabeled_nid`
# finding, so the fixed label cannot silently regress.
#
# tools/hle_manifest.py now also cross-checks EVERY registration against
# src/rt/nid_names.h automatically (the exhaustive NID->name integrity pass,
# issues #75/#78/#83/#86), so this map is only needed for NIDs whose canonical
# name cannot come from the generated table or to override it deliberately.
KNOWN_NID_NAMES = {
    0x1B4217BC: "sceKernelSetCompiledSdkVersion603_605",
    # --- sceSasCore routing integrity ---
    0x9EC3676A: "__sceSasSetADSRmode",
    0x33D4AB37: "__sceSasRevType",
    # --- issue #78 (sceReg routing) ---
    0x0CAE832B: "sceRegCloseCategory",
    0x1D8A762E: "sceRegOpenCategory",
    0x28A8E98A: "sceRegGetKeyValue",
    0x92E41280: "sceRegOpenRegistry",
    0xD4475AA8: "sceRegGetKeyInfo",
    0xFA8A5739: "sceRegCloseRegistry",
    # --- issue #83 (sceDisplay VBLANK routing) ---
    0x36CDFADE: "sceDisplayWaitVblank",
    # --- issue #86 (scePower routing) ---
    0x2085D15D: "scePowerGetBatteryLifePercent",
    0x0AFD0D8B: "scePowerIsBatteryExist",
    0x87440F5E: "scePowerIsPowerOnline",
    0xFDB5BFE9: "scePowerGetCpuClockFrequencyInt",
    0x478FE6F5: "scePowerGetBusClockFrequency",
}

# NIDs whose PSP ABI returns a single-precision float through $f0 (the MIPS
# float-return convention) while $v0 carries the integer status.  Routing one
# of these to a generic integer-return stub (h_ok family) reports success in
# $v0 but leaves $f0 stale/poisoned for the guest.  The gate rejects any
# registration whose handler is not proven to write the float return.
# Provenance: the #80 display-clock campaign recorded
# sceDisplayGetFramePerSec (0xDBA6C4C4) returning the measured 59.9400599f
# through $f0; the runtime implements it under the unified display clock.
FLOAT_RETURN_NIDS = {
    0xDBA6C4C4: {
        "name": "sceDisplayGetFramePerSec",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/80",
    },
}

# Handlers proven to write the float return into $f0 before returning.  A
# float-return NID must route to one of these; anything else (including the
# generic integer-return h_ok) is a float_return_handler_mismatch finding.
FLOAT_RETURN_HANDLERS = {
    "h_DisplayGetFramePerSec",
}

# Tracking issue for each canonical NID above, so a `mislabeled_nid` finding in
# the gate output points at the routing-correctness owner. Every key here must
# appear in KNOWN_NID_NAMES and must be registered in hle.c
# (tools/hle_manifest.py enforces both).
#
# NOTE: a corrected label and a dedicated handler are separate checks. The SAS
# NIDs above now route through handlers with their canonical signatures. The
# remaining issue links cover the unrelated canonical-name audits below.
KNOWN_NID_ISSUES = {
    0x0CAE832B: "https://github.com/Jstar269/nakagawa-recomp/issues/78",
    0x1D8A762E: "https://github.com/Jstar269/nakagawa-recomp/issues/78",
    0x28A8E98A: "https://github.com/Jstar269/nakagawa-recomp/issues/78",
    0x92E41280: "https://github.com/Jstar269/nakagawa-recomp/issues/78",
    0xD4475AA8: "https://github.com/Jstar269/nakagawa-recomp/issues/78",
    0xFA8A5739: "https://github.com/Jstar269/nakagawa-recomp/issues/78",
    0x36CDFADE: "https://github.com/Jstar269/nakagawa-recomp/issues/83",
    0x2085D15D: "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    0x0AFD0D8B: "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    0x87440F5E: "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    0xFDB5BFE9: "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    0x478FE6F5: "https://github.com/Jstar269/nakagawa-recomp/issues/86",
}

# Acknowledged findings. The gate requires the live finding set to equal this
# waiver set exactly: a new finding fails CI, and a waiver whose finding no
# longer reproduces is stale and also fails CI (so fixes must retire their
# waiver in the same change). Never add a waiver without an issue link.
#
# Currently empty: the 0x1b4217bc alias_mismatch/mislabeled_nid waivers were
# retired when the 603_605 SDK-version variant was rerouted to
# h_SetCompiledSdkVersion under its canonical name (issue #71).
WAIVERS = ()
