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
    # ReferSemaStatus reads the modeled semaphore: live count, create-time initCount,
    # maxCount, attr and name recorded at create, and the blocked-waiter count.
    "h_ReferSemaStatus": {
        "status": "partial",
        "limitation": "reports the modeled count, create-time initCount, maxCount, attr, name and blocked-waiter count; the caller size word is left as written and the full 56-byte span is written, a convention copied from ReferEventFlagStatus that is not hardware measured; unknown-UID and bad-pointer codes are source-selftested only",
    },
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
    "h_USec2SysClock": {
        "status": "partial",
        "limitation": "stores the unsigned microsecond duration as the low word of the runtime's microsecond SceKernelSysClock representation and writes a zero high word; hardware conversion and error-precedence cells are not measured",
    },
    "h_USec2SysClockWide": {
        "status": "partial",
        "limitation": "returns the unsigned microsecond duration in $v0 with a zero $v1 high word under the runtime's microsecond SceKernelSysClock representation; hardware conversion is not measured",
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
    # Kernel alarms are guest-time timers whose handlers run in interrupt context from the
    # scheduler's interrupt-service step.  The only measured cells are the unknown-alarm-id
    # code and "a non-zero handler return reschedules"; the rest is project-defined.
    "h_SetAlarm": {
        "status": "partial",
        "limitation": "the handler runs in interrupt context at the next interrupt-service point once the guest-time deadline passes (so it can run later than the deadline, and only while CPU interrupts are enabled) and a non-zero return re-arms the alarm that many microseconds after delivery, which is hardware measured only as 'reschedules'; a NULL handler returns the invalid-address code and a full alarm table the out-of-resources code without a measurement, SetAlarm(0) is accepted without a measurement, the interrupt-context rules for blocking waits made from a handler are the per-API ones (unmeasured for most waits), and the SysClock variants (sceKernelSetSysClockAlarm, sceKernelReferAlarmStatus) are not implemented",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_alarm_one_shot_fires_once_at_its_time_with_its_argument",
            "src/rt/hle_thread_selftest.c:test_alarm_rearm_by_return_value",
            "src/rt/hle_thread_selftest.c:test_alarm_handler_runs_under_interrupt_context_rules",
            "src/rt/hle_thread_selftest.c:test_alarm_idle_scheduler_advances_to_the_deadline",
            "src/rt/sched_selftest.c:test_alarm_runs_at_its_deadline_in_interrupt_context",
            "src/rt/sched_selftest.c:test_alarm_is_a_wake_source_for_the_idle_scheduler",
        ],
    },
    "h_CancelAlarm": {
        "status": "partial",
        "limitation": "an unknown, cancelled or already-finished alarm UID returns the hardware-measured unknown-alarm code; treating a one-shot that has already fired as unknown (its UID is released when the handler returns 0) is the project's model and is not separately measured",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_alarm_registration_and_error_codes",
            "src/rt/hle_thread_selftest.c:test_alarm_cancel_before_firing_prevents_the_handler",
            "src/rt/sched_selftest.c:test_alarm_cancel_and_ordering",
        ],
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
        "limitation": "waits the requested positive count of scheduler VBLANK periods; zero count returns SCE_KERNEL_ERROR_INVALID_VALUE (0x800001fe) before the wait-context check, following the historical waits-matrix transcription; callback behavior and hardware timing are not measured",
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
    # scePower clock getters, the devkit version word and sceCtrlPeekLatch, measured on the PSP-3000
    # 6.61 oracle 2026-10-15. Defaults are measured; values after a Set call and the non-clearing peek
    # are modelled and say so in their limitations.
    "h_PowerGetPllClockFrequencyFloat": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_power_identity_getters_measured",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-001 case pll-clock-float (0x435E0000 = 222.0)",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-002 case pll-clock-float-v0 ($f0 0x435E0000; $v0 not modelled)",
        ],
        "limitation": "returns the float of the retained PLL clock in $f0: the 222.0 default is measured on PSP-3000 6.61 (PSP-HLE-POWER-001, PSP-HLE-POWER-002), while values after scePowerSetClockFrequency follow the modelled request and are not hardware measured; $v0 is 0, a runtime choice: PSP-HLE-POWER-002 read a different word in $v0 after each call, neither 0 nor the clock, which is not modelled",
    },
    "h_PowerGetCpuClockFrequencyFloat": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_power_identity_getters_measured",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-001 case cpu-clock-float (0x435E0000 = 222.0)",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-002 case cpu-clock-float-v0 ($f0 0x435E0000; $v0 not modelled)",
        ],
        "limitation": "returns the float of the retained CPU clock in $f0: the 222.0 default is measured on PSP-3000 6.61 (PSP-HLE-POWER-001, PSP-HLE-POWER-002), while values after scePowerSetClockFrequency follow the modelled request and are not hardware measured; $v0 is 0, a runtime choice: PSP-HLE-POWER-002 read a different word in $v0 after each call, neither 0 nor the clock, which is not modelled",
    },
    "h_PowerGetBusClockFrequencyFloat": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_power_identity_getters_measured",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-001 case bus-clock-float (0x42DE0000 = 111.0)",
            "hardware oracle 2026-10-15 PSP-HLE-POWER-002 case bus-clock-float-v0 ($f0 0x42DE0000; $v0 not modelled)",
        ],
        "limitation": "returns the float of the retained bus clock in $f0: the 111.0 default is measured on PSP-3000 6.61 (PSP-HLE-POWER-001, PSP-HLE-POWER-002), while values after scePowerSetClockFrequency follow the modelled request and are not hardware measured; $v0 is 0, a runtime choice: PSP-HLE-POWER-002 read a different word in $v0 after each call, neither 0 nor the clock, which is not modelled",
    },
    "h_PowerSetCpuClockFrequency": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_psp_measured_cpu_clock_user_mode_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 user mode at PLL 222 (1 and 222 accepted; 0, 333, 334 and 223 refused with 0x800001fe)",
        ],
        "limitation": "accepts 1..retained PLL and refuses the rest with 0x800001fe, measured only at PLL 222; the clock the getters report after an accepted request is the retained request, not measured",
    },
    "h_PowerGetBatteryLifeTime": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (0 on AC; 376 minutes on battery)",
        ],
        "limitation": "reports the two measured samples for the modeled power source (external power by default); no discharge model",
    },
    "h_PowerGetBatteryChargingStatus": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_psp_measured_power_battery_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (0 on AC with a full battery and on battery)",
        ],
        "limitation": "reports the modeled charging state, measured 0 on AC with a full battery and on battery; the status of a charging battery is not measured",
    },
    "h_KernelDevkitVersion": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_kernel_devkit_version_measured",
            "src/rt/sdkver.h:SR_PSP_FIRMWARE_DEVKIT_VERSION",
            "hardware oracle 2026-10-15 PSP-SYSTEM-001 case model-profile (0x06060110)",
        ],
        "limitation": "returns the 6.61 devkit word for every firmware: only the measured firmware is modelled, and other firmware devkit words are not",
    },
    "h_CtrlPeekLatch": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ctrl_peek_latch_keeps_the_latch",
            "hardware oracle 2026-10-15 PSP-HLE-CTRL-LATCH-001 cases latch-peek-initial and latch-peek-after-poll (no input gives make break press 0 and release 0xFFFFFFFF)",
        ],
        "limitation": "the transition and return words are the sceCtrlReadLatch model, source-tested; the non-clearing peek is the project's model and is not hardware measured, because PSP-HLE-CTRL-LATCH-001 cannot tell clearing from keeping (its release field stays saturated with no input)",
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
        "limitation": "models synchronous display list pause (mode 0) and queue reset (mode 1); argument and parameter buffer inspection are K1/read checked only, asynchronous hardware command boundary timing remains unmodeled (#341); PSP-measured in the eligible PSP-GE-CONTROL-001 2026-10-15 re-run: the 0x80000020 no-active-list result, the invalid-mode result 0x80000107 (mode -1 only), the mode 0 queue id and the paused sync statuses (4, 2) for a list broken mid-draw, the mode 1 result 0 with one list stalled at its start, and the 0x80000100 sync result for a list the reset removed; a list enqueued with no stall address completes inside sceGeListEnQueue, so the runtime cannot reproduce the mid-draw break and returns 0x80000020 with sync peeks of 0 on that sequence; breaking a stalled list, a mode 1 reset of a paused list and modes above 1 are unmeasured and follow the same codes",
    },
    "h_GeContinue": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ge_break_continue",
        ],
        "limitation": "resumes a paused display list using the synchronous GE runner; hardware timing and multi-queue priority ordering remain unmodeled (#341); the 0 result when no list is paused is PSP-measured (PSP-GE-CONTROL-001, eligible 2026-10-15 re-run)",
    },
    "h_GeListDeQueue": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ge_break_continue",
        ],
        "limitation": "a stalled list returns BUSY (0x80000021) and stays queued, PSP-measured (PSP-GE-CONTROL-001, eligible 2026-10-15 re-run); paused, completed, cancelled and unknown queue ids are refused as NOT_IMPLEMENTED because no dequeue result is measured for them (#341)",
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
    # The licensee key is recorded for sceNpDrmEdataSetupKey, which stays refused: no key
    # material ships with the project and nothing decrypts.
    "h_NpDrmSetLicenseeKey": {
        "status": "partial",
        "limitation": "records the 16-byte key and returns 0; the 0x80550902 refusal of a null or unreadable key follows PPSSPP and is not hardware measured; the key is never used because sceNpDrmEdataSetupKey is refused",
    },
    "h_NpDrmClearLicenseeKey": {
        "status": "partial",
        "limitation": "forgets the recorded key and returns 0; not hardware measured",
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
        "description": "SAS waveform/ATRAC3 voice entry points without implemented codecs; returns PSP_SAS_ERROR_INVALID_STATE (0x80420016, pspsascore.h).",
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
    # disc present (activated 0x32 or deactivated 0x12). Other drive-error states
    # remain outside the modeled contract.
    "h_UmdGetErrorStat": {
        "status": "compatibility",
        "limitation": "returns no error while the virtual ISO disc is present, activated or not; the deactivated drive's error stat and other drive-error states are unmodeled (#341)",
    },
    "h_UmdDeactivate": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_psp_measured_umd_state_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (drive state 0x32 at entry, 0x12 after sceUmdDeactivate)",
        ],
        "limitation": "moves the modeled drive to the measured deactivated state 0x12; its own return value (0) and argument checks are not hardware measured",
    },
    "h_UmdUnRegisterUMDCallBack": {
        "status": "partial",
        "limitation": "clears the single modeled UMD callback registration; unknown IDs return 0x800201a1 (the kernel callback UNKNOWN_UID code), a project choice because PSPSDK documents only a negative error and the exact code is not hardware measured",
    },
    "h_IoClose": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-write)",
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-open-0000..0012)",
            "src/rt/hle_thread_selftest.c:test_umd_io_console_model",
            "src/rt/hle_thread_selftest.c:test_io_async_and_path_imports",
        ],
        "limitation": "the measured cells establish close result 0 for one freshly written host0 file and for the eleven successful fixed UMD/disc/msstor0p1 opens; double close, directory descriptors, standard descriptors and error precedence are not hardware measured, and the refusal while an asynchronous request is pending (ASYNC_BUSY) is covered by the host selftest only (#341)",
    },
    "h_IoDclose": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-dopen-0004 and io-dopen-0005 close results)",
            "src/rt/hle_thread_selftest.c:test_umd_io_console_model",
        ],
        "limitation": "the measured cells establish close result 0 for the two successful disc0:/ and disc0:/PSP_GAME directory descriptors only; closing a file, stale or out-of-range descriptor, and listings that were read before the close remain host-selftested only (#341)",
    },
    # Common Memory Stick devctls have explicit outputs; unsupported devices or
    # command pairs remain visible per pair and are tracked under issue #341.
    "h_IoDevctl": {
        "status": "partial",
        "limitation": "Memory Stick devctl callback events unmodeled (#341)",
    },
    "h_IoDopen": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_umd_io_console_model",
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-dopen-0000..0005)",
        ],
        "limitation": "the measured cells establish results and close outcomes for six fixed UMD/disc path calls only; directory contents/order, MS0 paths and general error precedence remain unmeasured (#341)",
    },
    "h_IoLseek": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-lseek and io-append)",
        ],
        "limitation": "PSP-IO-001 measures 64-bit SEEK_SET 32, SEEK_CUR -16, SEEK_END 0 and the end position after a 32-byte append on one host0 file; large offsets, out-of-range clamping, invalid whence, raw devices and error precedence remain unmeasured (#341)",
    },
    "h_IoMkdir": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
            "src/rt/hle.c:h_IoMkdir",
        ],
    },
    "h_IoOpen": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-open-create, io-write, io-read-verify, io-append, io-errors)",
            "docs/HARDWARE_ORACLE.md (PSP-UMDIO-001: io-open-0000..0012)",
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
        ],
        "limitation": "measured coverage is one host0 file's create/write/read/append/missing-file path plus PSP-UMDIO-001's fixed open paths and flags; other flag combinations, general error precedence and raw UMD data operations remain unmeasured (#341)",
    },
    "h_IoRead": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-read-verify and io-errors)",
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
        ],
        "limitation": "PSP-IO-001 measures one 64-byte host0 read that returns 64 matching bytes, and that reading descriptor -1 returns a negative result (the exact code is not recorded in public evidence); short reads at end of file, zero-length reads, partial-read position advance, unreadable buffers, write-only descriptors, raw UMD data and standard-stream reads remain unmeasured (#341)",
    },
    "h_IoRemove": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
            "src/rt/hle.c:h_IoRemove",
        ],
    },
    "h_IoWrite": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-IO-001: io-write and io-append)",
            "src/rt/hle_thread_selftest.c:test_ms0_unified_namespace",
        ],
        "limitation": "PSP-IO-001 measures a 64-byte write that returns 64 on a created host0 file and a 32-byte write through an append-mode descriptor that returns 32; read-only descriptors, short writes, zero-length writes, unreadable source buffers, raw devices and standard-stream writes remain unmeasured (#341)",
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
        "limitation": "delete-while-waited is mailbox-measured on 2026-10-15 (PSP-3000, 6.61): the waiter returns WAIT_DELETE and its timeout word is rewritten to the remaining time; interrupt-context placement unmeasured (#339, #341)",
    },
    "h_SendMbx": {
        "status": "partial",
        "limitation": "invalid-message-pointer error class unmeasured; interrupt-context placement unmeasured (#339, #341)",
    },
    "h_ReceiveMbx": {
        "status": "partial",
        "limitation": "delete-while-waited is mailbox-measured on 2026-10-15 (WAIT_DELETE and timeout word rewritten to the remaining time); ILLEGAL_CONTEXT precedence for a blocking receive unmeasured; priority-mailbox circular linking not separately asserted beyond campaign facts (#339, #341)",
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
    # sceKernelReferSystemStatus: bytes written = min(size, 0x1c), return 0, and the size word reads back
    # 0x1c, all PSP-measured (PR #1003 head 3072bd7d, 2026-10-15).
    "h_ReferSystemStatus": {
        "status": "partial",
        "limitation": "idle-clock, comes-out-of-idle and thread-switch counters: written as zero; hardware reports non-zero, time-dependent values (not cited). VFPU-switch counter: written as zero, not cited. Status word 1: written as zero; hardware read 0 in all three records, uncited. NULL pointer: runtime returns ILLEGAL_ADDR as a safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001); no game relies on the output. Other unwritable non-NULL pointers are unmeasured.",
    },
    # VTimer lifecycle and query surface (PSP-HLE-VTIMER-001, 2026-10-15, PR #1003 record). Measured:
    # Create returns a valid UID; Start returns 0, also on a restart, and 1 while running (per
    # docs/HARDWARE_ORACLE.md); Stop returns 1 while running and 0 when not started; Delete of a
    # running timer returns 0; Refer and Delete on the deleted UID return 0x800201BE. Refer fills
    # active, base (non-zero while running), current, and reads schedule, handler and common as 0.
    # PSP-HLE-VTIMER-002 and -003 (2026-10-15, build b5d6fc5b) add: Start, Stop and GetVTimerTime
    # on a deleted UID return 0x800201BE and that GetVTimerTime writes nothing; Delete of a stopped
    # timer returns 0; 64 creates in a row succeed; GetVTimerBase equals Refer's base while running
    # and both read 0 once stopped. The microsecond unit and base = system time at Start are
    # consistent with that run (deltas agree within about 8%), not measured exactly. Interrupt-driven
    # behaviour and handlers depend on issue #325.
    "h_CreateVTimer": {
        "status": "partial",
        "limitation": "NULL option struct only (a non-NULL option is refused, layout unmeasured); 64 creates in a row are measured (PSP-HLE-VTIMER-003), so the table holds 64; the limit above 64 and the no-slot code 0x80020190 are unmeasured; the microsecond unit is consistent with PSP-HLE-VTIMER-002, not a precise rate measurement (#325)",
    },
    "h_StartVTimer": {
        "status": "partial",
        "limitation": "deleted-UID code 0x800201BE measured (PSP-HLE-VTIMER-003); already-running return 1 is repo-documented, not measured by PSP-HLE-VTIMER-001, -002 or -003; base = system time at Start and the microsecond unit are consistent with PSP-HLE-VTIMER-002, not measured exactly (#325)",
    },
    "h_StopVTimer": {
        "status": "partial",
        "limitation": "deleted-UID code 0x800201BE measured (PSP-HLE-VTIMER-003); the microsecond unit of the held time is consistent with PSP-HLE-VTIMER-002, not a precise rate measurement (#325)",
    },
    "h_DeleteVTimer": {
        "status": "partial",
        "limitation": "delete of a stopped timer returns 0 (PSP-HLE-VTIMER-002) and a second delete returns 0x800201BE (PSP-HLE-VTIMER-001, -003); interrupt-context placement unmeasured (#325)",
    },
    "h_GetVTimerTime": {
        "status": "partial",
        "limitation": "deleted-UID code 0x800201BE with the output untouched measured (PSP-HLE-VTIMER-003); time = system time - base and the microsecond unit are consistent with PSP-HLE-VTIMER-002 (deltas within about 8%), not a precise rate measurement; NULL out-pointer returns ILLEGAL_ADDR as a runtime safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), not as a measured VTimer result; no game relies on the output (#325)",
    },
    "h_GetVTimerBase": {
        "status": "partial",
        "limitation": "returns the same base as ReferVTimerStatus, measured equal while running and 0 once stopped (PSP-HLE-VTIMER-002); the value, guest time at the most recent Start, is consistent with that run's system-clock reads, not measured exactly; NULL out-pointer returns ILLEGAL_ADDR as a runtime safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), not as a measured VTimer result; deleted-UID code is unmeasured and reused by analogy; no game relies on the NULL output (#325)",
    },
    "h_ReferVTimerStatus": {
        "status": "partial",
        "limitation": "return 0, the deleted-UID code, and the active, current, schedule, handler and common states are measured (PR #1003); base equals GetVTimerBase while running and reads 0 once stopped (PSP-HLE-VTIMER-002), and its value (guest time at Start) and the microsecond unit are consistent with that run, not measured exactly; NULL pointer returns ILLEGAL_ADDR as a runtime safety choice after PSP-3000 hung on ReferSystemStatus(NULL) (PSP-HLE-NULL-OUT-001), not a measured VTimer result; the caller-size write rule for size 0 is the sema/evf/mbx rule applied without a VTimer cite (#325)",
    },
    # sceAudioSRC* (issue #339). Measured on PSP-3000 firmware 6.6.1, 2026-10-15
    # (PSP-HLE-AUDIO-SRC-001): the unreserved, reserve, second-reserve, release and
    # Output2-exclusion returns, the sample count returned by OutputBlocking, and the
    # accepted and refused reserve shapes listed in docs/HARDWARE_ORACLE.md.
    "h_AudioSRCChReserve": {
        "status": "partial",
        "limitation": "the returns for an unreserved slot, a held slot (0x80268002, including Output2 holding it), sample counts 0 (0x80000104), 65 and 4096 (accepted), 48000 Hz (accepted) and channel counts 1 (0x80000104) and 4 (0x80000003) are measured (PSP-HLE-AUDIO-SRC-001); the upper sample-count bound reuses the Output2 range 17..4111, channel counts other than 1, 2 and 4 take the code measured for 4, and rates other than 44100 and 48000 are refused with that same code and a message, all unmeasured (#339)",
    },
    "h_AudioSRCChRelease": {
        "status": "partial",
        "limitation": "release after a blocking output (0) and release with nothing reserved (0x80260008) are measured (PSP-HLE-AUDIO-SRC-001); whether a release succeeds while samples are still queued, as the Output2 busy rule does, and releasing a reservation made through the other API (refused here as not reserved) are unmeasured (#339)",
    },
    "h_AudioSRCOutputBlocking": {
        "status": "partial",
        "limitation": "returns 0x80260008 with nothing reserved and the reserved sample count after a reserve (0x400), both measured (PSP-HLE-AUDIO-SRC-001); the volume bound reuses the Output2 bound and the NULL or unreadable buffer result follows the regular output, both unmeasured; the host mixer runs at 44100 Hz, so a 48000 Hz block passes through a band-limited converter and keeps its source-rate pacing (#339)",
    },
    # Route-A graduation batch 1 (#341, 2026-10-19). Each status is bounded to
    # the named accepted hardware cells; audio's unmeasured argument/error paths
    # remain partial instead of being promoted as universal firmware behavior.
    "h_AudioChRelease": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-ch-release, audio-ch-query-released",
        ],
        "limitation": "the cells release channel 0 after the two valid blocking-output calls and query it after release; unreserved, out-of-range, and other channel-state release results are unmeasured (#341)",
    },
    "h_AudioChReserve": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-ch-reserve",
        ],
        "limitation": "audio-ch-reserve measures a successful reservation of channel 0 for 1024 stereo samples; automatic channel selection, invalid size or format, duplicate reservation, and exhaustion/error precedence are unmeasured (#341)",
    },
    "h_AudioRestLen": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-ch-query-before, audio-ch-query-after, audio-ch-query-released",
        ],
        "limitation": "the cells query channel 0 before and after blocking output and just after release; invalid-channel error behavior and other queue states are unmeasured (#341)",
    },
    "h_AudioOutput2Rest": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-out2-query-before, audio-out2-query-after",
        ],
        "limitation": "the cells query rest samples on one valid 1024-sample Output2 reservation before and after one blocking output; the not-reserved code and other queue states are unmeasured (#341)",
    },
    "h_AudioOutput2Blocking": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-out2-output-blocking",
        ],
        "limitation": "audio-out2-output-blocking measures one valid silent 1024-sample block at maximum volume and makes no acoustic claim; not-reserved, volume, buffer-error and audio_output error-propagation behavior are not covered (the handler discards audio_output's return) (#341)",
    },
    "h_AudioOutput2Release": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-out2-release",
            "docs/HARDWARE_ORACLE.md",
        ],
        "limitation": "audio-out2-release measures success after the queue drains; the documented #311 cell measures 0x80268002 while samples remain queued and preserves the reservation; releasing when not reserved is not measured (#341)",
    },
    "h_AudioOutput2Reserve": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cell: audio-out2-reserve",
        ],
        "limitation": "audio-out2-reserve measures a valid 1024-sample reservation; duplicate Output2 reservation behavior and invalid size bounds are unmeasured (#341)",
    },
    "h_AudioOutputBlocking": {
        "status": "partial",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-AUDIO-001",
            "fixtures/psp_oracle/probe.c:run_audio_query",
            "PSP-AUDIO-001 cells: audio-ch-output-blocking-0, audio-ch-output-blocking-1",
        ],
        "limitation": "the cells exercise two maximum-volume outputs with a valid silent stereo buffer and record timing and rest-length values, not audible output; mono, other volumes, invalid buffers/channels, and error precedence are unmeasured (#341)",
    },
    "h_GeEdramGetAddr": {
        "status": "complete",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-HLE-GE-EDRAM-001",
            "fixtures/psp_oracle/probe_hle_measure.c:hle_run",
            "src/rt/hle.c:h_GeEdramGetAddr",
            "PSP-HLE-GE-EDRAM-001 cell: edram-addr = 0x04000000",
        ],
    },
    "h_GeEdramGetSize": {
        "status": "complete",
        "evidence": [
            "tools/psp_oracle/manifest.json:PSP-HLE-GE-EDRAM-001",
            "fixtures/psp_oracle/probe_hle_measure.c:hle_run",
            "src/rt/hle.c:h_GeEdramGetSize",
            "PSP-HLE-GE-EDRAM-001 cell: edram-size = 0x00200000",
        ],
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
    "h_RtcGetTime64_t": {
        "status": "partial",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_psp_measured_rtc_time64_20261024",
            "hardware measurement 2026-10-24, PSP-3000 6.61 (valid dates convert; year 1 and pre-1970 dates return 0x800001fe; month 13, day 32, Feb 30 and hour 24 return 0 and normalise; NULL output returns 0; NULL date faults inside the module)",
        ],
        "limitation": "out-of-range fields normalise by calendar carry, whose exact seconds are this arithmetic rather than measured; year 0, month 0 and day 0 are refused with 0x800001fe and a refusal leaves the output unwritten, both unmeasured; a NULL or unreadable date and an unwritable non-NULL output take the HLE guest-fault stop",
    },
    "h_RtcGetWin32FileTime": {
        "status": "partial",
        "limitation": "cold-first-call error reporting may differ from firmware (PSPAutotests convert.c notes errors report properly only after a prior error and that the rules are hard to determine); component bounds beyond the measured year/epoch/day-carry cases stay fail-closed rather than measured (#341)",
    },
    # sceReg virtual system registry (src/rt/hle.c, sceReg block): the /CONFIG tree measured on
    # PSP-3000 6.6.1 (2026-10-09, read-only enumeration) with a per-user overlay persisted by the
    # flush calls. sceUtilityGetSystemParamInt reads the registry's shared integer keys back.
    "h_RegOpenRegistry": {
        "status": "partial",
        "limitation": "opens the one virtual system registry in mode 1 or 2 with 512 project-bounded handles (the console served at least 257 at once, measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration); RegParam regtype and name are not checked; the first open loads the measured /CONFIG tree and the per-user overlay; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegCloseRegistry": {
        "status": "partial",
        "limitation": "frees a registry handle slot; closed handles get the bad-handle code 0x8008272E, measured on sceRegGetKeysNum and applied here by analogy",
    },
    "h_RegFlushRegistry": {
        "status": "partial",
        "limitation": "persists the whole per-user overlay atomically when the registry changed (whether firmware persists less, or commits on close, is unmeasured); a failed write is EIO and the change stays in memory",
    },
    "h_RegOpenCategory": {
        "status": "partial",
        "limitation": "opens a category of the measured tree with 512 project-bounded handles; an open of a missing category creates it in either mode, cutting new names to 26 bytes (measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration for mode 1); the console's failure to reopen such a cut name is not modeled; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegCloseCategory": {
        "status": "partial",
        "limitation": "frees a category handle slot, including one made stale by sceRegRemoveCategory; closed handles get the bad-handle code 0x8008272E (measured on sceRegGetKeysNum, applied by analogy)",
    },
    "h_RegFlushCategory": {
        "status": "partial",
        "limitation": "persists the whole per-user overlay, not only the flushed category (firmware scope unmeasured); a failed write is EIO and the change stays in memory",
    },
    "h_RegRemoveCategory": {
        "status": "partial",
        "limitation": "removes an empty category a game created through a mode-2 registry handle; measured default categories are refused (EPERM) and non-empty ones too (ENOTEMPTY); write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegGetKeyInfo": {
        "status": "partial",
        "limitation": "reports type, size and a key handle for the measured /CONFIG entries (names, types, sizes and DIR size 0 measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration), created keys and child categories; an unknown key returns the measured 0x8008271D; values other than six measured settings are neutral placeholders",
    },
    "h_RegGetKeyInfoByName": {
        "status": "partial",
        "limitation": "the same lookup as sceRegGetKeyInfo without a key handle; the not-found code is applied by analogy",
    },
    "h_RegGetKeyValue": {
        "status": "partial",
        "limitation": "copies a value key with bounds checks (a buffer smaller than the value is EINVAL, a DIR entry EFTYPE; both unmeasured); stale or foreign key handles get the bad-handle code by analogy; reading a neutral placeholder logs once",
    },
    "h_RegGetKeyValueByName": {
        "status": "partial",
        "limitation": "the same copy as sceRegGetKeyValue, by name; error codes other than not-found are not hardware measured",
    },
    "h_RegSetKeyValue": {
        "status": "partial",
        "limitation": "writes an existing key through a mode-2 category handle (mode 1 is EACCES): INT exactly 4 bytes, a STR takes the size of its new value (string sizes follow their values, measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration), BIN up to its fixed size with the rest zero-filled; persists on a flush; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegCreateKey": {
        "status": "partial",
        "limitation": "creates an INT (4 bytes), STR or BIN key (up to a 4096-byte project bound) with zero-filled space through a mode-2 category handle; names over 26 bytes are cut to 26 (inferred from the measured category cut); DIR and unknown types are EINVAL, existing names EEXIST; write semantics and the errno-class codes are not hardware measured",
    },
    "h_RegGetKeysNum": {
        "status": "partial",
        "limitation": "counts a category's keys and child categories; a forged handle returns 0x8008272E and leaves the count untouched (measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration)",
    },
    "h_RegGetKeys": {
        "status": "partial",
        "limitation": "writes the first num entry names as NUL-padded 27-byte slots in the console's order for the measured tree (measured on PSP-3000 6.6.1, 2026-10-09, read-only enumeration) and creation order after it; truncation below the entry count is a project choice",
    },
    # Reviewed Route-A handlers: hardware evidence covers named cells, while
    # the remaining branches stay explicit instead of being promoted complete.
    "h_GeEdramSetAddrTranslation": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-GE-EDRAM-001: initial width, 512/1024/2048/4096 replacements, restore and final query)",
        ],
        "limitation": "replacement-width returns and restoration to boot width 1024 are measured; width 0 as an input, unsupported-width error 0x80000107 (a project choice), and applying the setting to actual GE address translation are not measured",
    },
    "h_TryLockLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 LwMutex cells)",
        ],
        "limitation": "normal acquisition, count-zero refusal, non-recursive self-contention, and another-owner contention are measured; negative counts, invalid-workarea error precedence, interrupt context, and several-waiter ordering are not measured",
    },
    "h_UnlockLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 LwMutex cells)",
        ],
        "limitation": "owner release, direct hand-off to one waiter, and non-owner/underflow behavior are measured; invalid workareas, negative or excessive unlock counts, and several-waiter ordering are not measured",
    },
    "h_AllocateFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 FPL cells)",
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001 FPL create, exhaustion, status, free and delete cells)",
        ],
        "limitation": "ordinary allocation, one finite timeout, one-waiter hand-off, delete wake, and measured block/status layout are covered; invalid output pointers, negative timeout handling, and multiple-waiter ordering are not measured",
    },
    "h_AllocateVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01 and PSP-B2-01 VPL cells)",
        ],
        "limitation": "ordinary allocation, one finite timeout, measured pool accounting, and one-waiter hand-off are covered; invalid output pointers, invalid sizes, and fragmented-pool behavior beyond the measured cases are not measured",
    },
    # Route-A batch 3: each citation was checked against the exact API path in
    # its hardware cell. These handlers remain partial where the cell does not
    # cover all branches or where the current implementation has a known gap.
    "h_CancelEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: event-cancel-before-deadline-late-dispatch returns WAIT_CANCEL to the waiter)",
        ],
        "limitation": "the cited cell measures a blocked waiter's WAIT_CANCEL outcome after event cancellation; it does not establish the cancel call's return, pattern assignment, waiter-count output, unknown-ID behavior, pointer errors, or multiple-waiter ordering",
    },
    "h_CancelSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B2-01: CancelSema(-1) restores the create-time initial count)",
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: sema-cancel-before-deadline-late-dispatch returns WAIT_CANCEL to the waiter)",
        ],
        "limitation": "the cited cells establish the -1 reset and a blocked waiter's WAIT_CANCEL outcome; other replacement counts, over-max refusal, optional waiter-count output, unknown IDs, pointer errors, and multiple-waiter ordering are not established by those citations",
    },
    "h_ClearEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: ClearEventFlag(mask) keeps only the bits in mask)",
        ],
        "limitation": "the measured mask behavior matches the normal-object path; an unknown event-flag UID currently returns generic 0x80020000 instead of the measured unknown-event-flag code 0x8002019A, and error precedence is not covered",
    },
    "h_CreateFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: fpl-create and valid pool status/allocation cells)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics: FPL blocks are blockSize apart)",
        ],
        "limitation": "the cited cells cover valid pool creation and block/status behavior; the handler converts zero block size/count to 16/1, a branch not established by those cited valid-pool cells, and allocation exhaustion plus partition, attribute, and option error precedence need separate evidence",
    },
    "h_CreateLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01 and PSP-B2-01: LwMutex creation workarea and one-waiter hand-off)",
        ],
        "limitation": "the cells cover recursive and non-recursive initialization, workarea fields, and one waiter hand-off; invalid workareas, unsupported attribute bits, negative counts, option data, and multi-waiter ordering are not established",
    },
    "h_CreateMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-refer-unlocked creation and observed status fields)",
        ],
        "limitation": "the measured cell covers ordinary unlocked and initially-held mutex status; name and attribute validation, recursive edge cases, invalid arguments, and allocation exhaustion are not covered",
    },
    "h_CreateSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: CreateSema accepts init greater than max and max zero, and rejects invalid attributes)",
            "docs/HARDWARE_ORACLE.md (PSP-B2-01: CancelSema restores the create-time initial count)",
        ],
        "limitation": "the cited cells cover the measured count and attribute rules plus initial-count retention; name-pointer behavior, object exhaustion, and invalid-argument precedence remain unmeasured",
    },
    "h_CreateVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01 and PSP-B2-01: valid VPL creation, accounting, allocation, and deletion)",
        ],
        "limitation": "valid pool creation and measured accounting are covered; the handler maps size zero to VPL_EXHAUSTED and ignores the partition argument, but the cited valid-pool cells do not establish those edge results, allocation exhaustion, or option behavior",
    },
    "h_DeleteFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: valid fpl-delete and refer-after-delete cells)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B2-01: deleting an FPL with a waiter)",
        ],
        "limitation": "valid deletion and the delete-wait outcome are covered; an invalid or already-deleted UID currently returns success in the handler, while the cited delete cells use a valid pool, and multi-waiter teardown ordering is not measured",
    },
    "h_DeleteLwMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: deleting a held LwMutex updates the workarea fields)",
        ],
        "limitation": "the cited cells cover valid deletion and the measured workarea fields; forged workareas, invalid-address precedence, stale UIDs, and multiple-waiter deletion behavior are not fully covered",
    },
    # Route-A batch 4: the cited cells cover concrete lifecycle/allocation
    # paths. The worker-only cells do not establish main-thread teardown.
    "h_DeleteSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01 and PSP-B2-01: unknown semaphore ID and deleting a waited-on semaphore)",
        ],
        "limitation": "a valid delete with one waiter and the unknown-semaphore code are covered; no-waiter behavior, multiple-waiter ordering, and interrupt-context precedence are not established",
    },
    "h_DeleteThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-D77, ED-D00, ED-DNEG, and ED-DERR post-exit delete/restart cells)",
        ],
        "limitation": "post-exit delete and restart outcomes are covered for four worker exit statuses; deleting a live or waiting thread, unknown IDs, and multi-joiner cleanup are not established",
    },
    "h_DeleteVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: valid VPL deletion while blocks are held)",
        ],
        "limitation": "valid deletion with allocated blocks is covered; deletion with blocked waiters, unknown IDs, arbitrary fragmentation, and invalid-free precedence are not established",
    },
    "h_ExitDeleteThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-D77, ED-D00, ED-DNEG, and ED-DERR)",
        ],
        "limitation": "worker ExitDeleteThread status, joiner result, and post-exit UID/delete behavior are covered for four values; main/launcher teardown is not covered by these cells and PSP-TEARDOWN-001 remains captured-only",
    },
    "h_ExitThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-THREAD-EXIT-001: ED-X77, ED-X00, ED-XNEG, and ED-XERR)",
        ],
        "limitation": "worker ExitThread status and WaitThreadEnd/ReferThreadStatus outcomes are covered for four values; main/launcher teardown, callbacks during exit, and other thread roles are not established",
    },
    "h_FreeFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: fpl-free-a, fpl-free-b, and status after free)",
        ],
        "limitation": "valid block frees and resulting free counts are covered; invalid, unaligned, repeated, or foreign-pool frees and waiter ordering are not established by the cited cells",
    },
    "h_FreeVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: VPL pool accounting, allocation placement, and free sequence)",
        ],
        "limitation": "valid frees under the measured pool accounting are covered; invalid IDs/addresses, misalignment, arbitrary fragmentation/coalescing, and zero-size allocation behavior are not established",
    },
    "h_ReleaseWaitThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B3-01: ReleaseWaitThread wakes a blocked target with WAIT_RELEASE)",
        ],
        "limitation": "the cited cell covers releasing one blocked target and the waiter's WAIT_RELEASE result; self/UID-zero targets, already-finished targets, and multiple-waiter ordering are not established",
    },
    "h_TryAllocateFpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: fpl-try-alloc-a, fpl-try-alloc-b, and fpl-try-alloc-exhausted)",
        ],
        "limitation": "two valid allocations, the measured exhaustion code, and pool status are covered; invalid output pointers, invalid UIDs, and other exhaustion/error precedence are not established",
    },
    "h_TryAllocateVpl": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: 1024-byte VPL accounting and high-down allocation placement)",
        ],
        "limitation": "valid allocation placement and measured pool accounting are covered; the handler maps a zero-size request to VPL_EXHAUSTED, whose exact hardware result is unmeasured, and invalid output pointers or fragmented-pool behavior are not established",
    },
    # Route-A batch 5: retain only the behaviors named by the existing cells.
    "h_LockMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-timeout-quanta, mutex-priority-inheritance, and accepted interrupt-context cell)",
        ],
        "limitation": "measured cases cover finite timeout, priority inheritance, and the listed interrupt-context refusals; full attribute/recursion ranges, negative counts, multi-waiter priority ordering, and invalid timeout-pointer precedence are not measured",
    },
    "h_NotifyCallback": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics: two callback notifications coalesce into one handler call with count 2)",
        ],
        "limitation": "the cited cell covers notification coalescing in one callback case; callback creation state, invalid UID behavior, dispatch context, and ordering with multiple callbacks are not established",
    },
    "h_PollEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01: unmet poll output and zero-bit refusal)",
        ],
        "limitation": "the cited cells cover an unmet poll's outBits and the zero-bit refusal; other wait modes, invalid output pointers, and error precedence are not established",
    },
    "h_ReceiveMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: blocked receiver is served directly by a sender)",
        ],
        "limitation": "the cited cell covers a blocked receiver served by a sender; empty/short receive result sizes, invalid pointers, timeout precedence, and multiple-receiver ordering are not established",
    },
    "h_ReferEventFlagStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-KERNEL-STATUS-001: event-size-zero, event-size-8, event-size-40, and event-size-full)",
        ],
        "limitation": "the cited cells establish bytes written and size-word behavior at four buffer sizes; full field values, unknown UID codes, invalid pointers, and error precedence are not established by that size row",
    },
    "h_ReferFplStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-KERNEL-STATUS-001: valid FPL layout, short/zero-size writes, free counts, and refer-after-delete)",
        ],
        "limitation": "valid pool layout and the cited short/zero-size behavior are covered; tick fields, invalid pointers, unknown IDs beyond the after-delete refer, and error precedence are not established",
    },
    "h_ReferMutexStatus": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-refer-unlocked status fields)",
        ],
        "limitation": "the cited cell covers an unlocked mutex and its measured status words; size variants, recursive ownership, invalid pointers, and unknown-UID fields are not established",
    },
    "h_SendMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: no-room, oversize, and blocked-receiver message-pipe cells)",
        ],
        "limitation": "the cited cells cover no-room refusal, oversize refusal, and a receiver served by a sender; result-size pointers, timeout behavior, multi-waiter ordering, and all mode/error-precedence combinations are not established",
    },
    "h_SetEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: event-signal-before-deadline-late-dispatch)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics: selected event-flag pattern and wait-mode cells)",
        ],
        "limitation": "the cited cells cover setting bits that satisfy a waiter and selected wait modes; invalid UIDs, all-bit masks, result precedence, and multiple-waiter ordering are not established",
    },
    "h_SignalSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-B1-01: semaphore signal count boundaries)",
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: sema-signal-before-deadline-late-dispatch)",
        ],
        "limitation": "the cited cells cover selected count boundaries and waking a waiter; invalid output pointers, negative-count ranges outside the tested values, and multi-waiter ordering are not established",
    },
    # Route-A batch 6: these remaining kernel wait/transfer rows have direct
    # cells, but their unmeasured argument and ordering branches stay partial.
    "h_SleepThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: SleepThread and wakeup sequence)",
        ],
        "limitation": "the cited cell covers a helper sleeping and resuming after a wakeup; no-current-thread, disabled-dispatch precedence, callbacks during sleep, and other wake races are not established",
    },
    "h_TryLockMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: accepted mutex-interrupt-context TryLockMutex result)",
        ],
        "limitation": "the accepted interrupt-context cell confirms ILLEGAL_CONTEXT for TryLockMutex; ordinary acquisition, recursive count boundaries, and owner-contention results are not covered by that cell",
    },
    "h_TryReceiveMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: message-pipe buffered and empty receive cells)",
        ],
        "limitation": "the cited message-pipe cells cover selected buffered and empty receives; result-size pointer handling, partial-mode edge cases, invalid pointers, and error precedence are not fully summarized by the public oracle row",
    },
    "h_TrySendMsgPipe": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: message-pipe no-room and oversize cells)",
        ],
        "limitation": "the cited cells cover no-room refusal, oversize refusal, and a partial-mode send; result-size pointer handling, invalid pointers, and all mode/error-precedence combinations are not established",
    },
    "h_UnlockMutex": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-MUTEX-001: mutex-priority-inheritance owner release and accepted interrupt-context cell)",
        ],
        "limitation": "the cited cases cover owner release in the priority-inheritance control and interrupt-context refusal; invalid UIDs, bad counts, and multi-waiter priority ordering are not established",
    },
    "h_WaitEventFlag": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: event signal/cancel before deadline and late dispatch)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01: event flag poll/wait modes and outputs)",
        ],
        "limitation": "the cited cells cover the named signal/cancel outcomes and selected wait modes; an unknown event-flag UID currently returns generic 0x80020000 instead of 0x8002019A, and other mode, pointer, and multi-waiter precedence is not established",
    },
    "h_WaitSema": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-WAIT-001: semaphore signal/cancel before deadline and late dispatch)",
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B1-01: semaphore count and wait results)",
        ],
        "limitation": "the cited cells cover selected count, signal/cancel, and timeout outcomes; invalid timeout pointers, negative counts, general multi-waiter ordering, and all error-precedence cases are not established",
    },
    "h_WakeupThread": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (Kernel-object semantics, PSP-B3-01: wakeups accumulate while the target is not sleeping)",
        ],
        "limitation": "the cited cells cover wakeup-count accumulation and a sleeping target; specific invalid-target errors, repeated wakeup order, and interactions with termination are not all established",
    },
    # Route-A batch 7: the measured system-clock delta supports a partial status,
    # while the probe's offset intervals do not establish a precise clock rate.
    "h_GetSystemTimeWide": {
        "status": "partial",
        "evidence": [
            "docs/HARDWARE_ORACLE.md (PSP-HLE-VTIMER-002: system-clock reads around the bounded delay)",
        ],
        "limitation": "the two system-clock reads advanced 220,608 around a bounded 20 ms delay, but host0 logging offset the intervals and the VTimer/system-clock deltas agree only within about 8%; this is consistent with the microsecond unit but does not establish a precise rate or absolute system-time values",
    },
    # Interrupt-enable predicate: returns 1 when the CPU interrupt state is enabled. The selftest
    # reads it before and after SuspendIntr/ResumeIntr in the production scheduler path.
    "h_CpuIsIntrEnable": {
        "status": "complete",
        "evidence": [
            "src/rt/hle_thread_selftest.c:test_interrupt_nid_semantics",
        ],
    },
    # System time: writes the 64-bit microsecond clock to the caller's SceKernelSysClock and returns 0.
    # The illegal-address code is the runtime's own; no PSP measurement names it.
    "h_GetSystemTime": {
        "status": "partial",
        "limitation": "writes the 64-bit modeled microsecond clock and returns 0; the illegal-address result is the runtime's own code (RTC_ILLEGAL_ADDR), not a PSP measurement, and the clock origin is not compared with hardware",
    },
    # System time, low word: returns the low 32 bits of the modeled microsecond clock.
    "h_GetSystemTimeLow": {
        "status": "partial",
        "limitation": "returns the low 32 bits of the modeled microsecond clock, which wraps; the clock origin and wrap timing are not compared with hardware",
    },
    # Guest memcpy: copies n bytes and returns dst. Overlapping spans use memmove semantics; the PSP's
    # overlap behaviour is unmeasured, and bad spans are refused through the runtime's out-of-range path.
    "h_Memcpy": {
        "status": "partial",
        "limitation": "copies with memmove semantics for overlapping spans, which the PSP's memcpy does not promise and whose overlap behaviour is unmeasured; bad guest spans are refused through the runtime's out-of-range path, not a PSP error code",
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
    # scePower Float getters: the default clock values are measured on the PSP-3000 6.61 oracle
    # 2026-10-15 (PSP-HLE-POWER-001); the float-return shaping is tracked by #86.
    0xEA382A27: {
        "name": "scePowerGetPllClockFrequencyFloat",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    },
    0xB1A52C83: {
        "name": "scePowerGetCpuClockFrequencyFloat",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    },
    0x9BADB3EB: {
        "name": "scePowerGetBusClockFrequencyFloat",
        "issue": "https://github.com/Jstar269/nakagawa-recomp/issues/86",
    },
}

# Handlers proven to write the float return into $f0 before returning.  A
# float-return NID must route to one of these; anything else (including the
# generic integer-return h_ok) is a float_return_handler_mismatch finding.
FLOAT_RETURN_HANDLERS = {
    "h_DisplayGetFramePerSec",
    "h_PowerGetPllClockFrequencyFloat",
    "h_PowerGetCpuClockFrequencyFloat",
    "h_PowerGetBusClockFrequencyFloat",
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
