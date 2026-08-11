# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Curated classification metadata for the HLE registration manifest.

Plain Python data module on purpose, matching tools/compat_overrides.py: this
file is meant to be read and reviewed by humans in diffs, without a
serialization round-trip. tools/hle_manifest.py cross-checks every
entry here against the registrations actually extracted from src/rt/hle.c, so
stale handler names or NIDs fail the gate instead of rotting silently.

Classification model (per registered NID):
  fake_success           -- routed to a generic always-success handler (h_ok
                            family) or a handler curated as `stub`. The call
                            reports success but performs none of the API's
                            contract. These are the silent-corruption risks
                            issue #71 exists to surface.
  controlled_unsupported -- a dedicated handler that deliberately refuses the
                            operation with the API's own documented error
                            (e.g. the PSMF getters returning PSMF_ERR_NO_DATA
                            until issue #31 lands). Unsupported, but honest.
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

# handler name -> status. Every handler named here must exist in hle.c's
# extracted registrations (tools/test_hle_manifest.py enforces it).
HANDLER_STATUS = {
    # Stores g_sdk_version for SDK-dependent paths; the retained-state
    # contract for the variants routed to it is implemented.
    "h_SetCompiledSdkVersion": "complete",
    # Documented controlled-error policy: PSMF video/audio getters return
    # PSMF_ERR_NO_DATA until the real demux is connected (AGENTS.md; the
    # attract movie stays black by design). Tracked by issue #31.
    "h_PsmfGetVideo": "controlled_unsupported",
    "h_PsmfGetAudio": "controlled_unsupported",
    # SAS waveform/ATRAC3 entry points whose source codecs are not implemented
    # by this runtime. They validate the core/voice identity and return the
    # documented invalid-state error instead of fabricating success.
    "h_SasUnsupportedVoice": "controlled_unsupported",
    # sceDmacMemcpy / sceDmacTryMemcpy. The measured contract is implemented and
    # regression-tested through production dispatch: the illegal-size and
    # illegal-address classes, whole-span validation with overflow-safe
    # arithmetic, failure atomicity (no byte written, no GPU dirty),
    # memmove-correct same-pointer and overlapping copies, and the measured
    # 0xC000 effective prefix ceiling. The handlers remain partial because
    # concurrent-DMA BUSY behavior and the precedence of validation for an
    # invalid truncated tail are not established by the available evidence.
    "h_DmacMemcpy": "partial",
    "h_DmacTryMemcpy": "partial",
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
