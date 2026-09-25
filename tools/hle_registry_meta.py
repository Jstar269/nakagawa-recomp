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
        "limitation": "Init accepts only the documented 4:2:0 allocation shape and makes guest bytes deterministic; the firmware-owned header and cache contract are not measured (#302)",
    },
    "h_MpegAvcDecodeYCbCr": {
        "status": "partial",
        "limitation": "Decode produces only a backend-delivered picture and reports delayed/no-data states; the hardware EOS and cache-coherency observations are still required (#302)",
    },
    "h_MpegAvcDecodeStopYCbCr": {
        "status": "partial",
        "limitation": "Stop clears tracked state, but the firmware buffered-picture status is explicitly unmeasured rather than reported as a guessed frame count (#302)",
    },
    "h_MpegAvcCopyYCbCr": {
        "status": "partial",
        "limitation": "Copy handles matching initialized allocations and rejects overlap; the firmware layout and overlap result still need a physical PSP oracle (#302)",
    },
    "h_MpegAvcCsc": {
        "status": "partial",
        "limitation": "CSC preflights dynamic source/range/stride geometry and pixel formats; clipping, range conversion, and destination coherency remain hardware-oracle work (#302)",
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
    # memmove-correct same-pointer and overlapping copies, and the measured
    # 0xC000 effective prefix ceiling. The handlers remain partial because
    # concurrent-DMA BUSY behavior and the precedence of validation for an
    # invalid truncated tail are not established by the available evidence.
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
        "limitation": "returns no error under virtual ISO drive; other drive-error states unmodeled (#281, #341)",
    },
    # Common Memory Stick devctls have explicit outputs; unsupported devices or
    # command pairs remain visible per pair and are summarized under issue #281.
    "h_IoDevctl": {
        "status": "partial",
        "limitation": "Memory Stick devctl callback events unmodeled (#281, #341)",
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
        "limitation": "run clocks use host scheduler time and preemption/release counters are modeled rather than PSP-measured (#309)",
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
