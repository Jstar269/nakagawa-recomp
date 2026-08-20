# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Tests for the fail-closed HLE registration manifest (tools/hle_manifest.py).

Covers the extraction equivalence guarantees (every sr_hle_register occurrence
accounted for, no duplicates, handlers defined), the curated-metadata
cross-checks, the SetCompiledSdkVersion alias rule from issue #71's comment,
and reproducibility of the committed classification baseline.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import unittest

import hle_manifest
import hle_registry_meta as meta
import nid_name_proof
from hle_manifest import (
    DEFAULT_BASELINE,
    ManifestError,
    build_manifest,
    compute_findings,
    extract_registrations,
    manifest_to_baseline,
    unwaived_and_stale,
)

ROOT = Path(__file__).resolve().parents[1]

SYNTH_SOURCE = """
static uint32_t h_Alpha(CpuState *s) { (void)s; return 0; }
static uint32_t h_ok(CpuState *s) { (void)s; return 0; }
void sr_hle_register(uint32_t nid, const char *name, HleFn fn) { }
static void init(void) {
    sr_hle_register(0x00000010u, "synthAlpha", h_Alpha);
    sr_hle_register(0x00000020, "synthBeta", h_ok);
    static const uint32_t sas_ok[] = { 0x00000030, 0x00000040 };
    for (unsigned i = 0; i < 2; i++)
        sr_hle_register(sas_ok[i], "__sceSas_ok", h_ok);
}
"""


class ExtractionTests(unittest.TestCase):
    def test_synthetic_source_extracts_all_forms(self) -> None:
        regs = extract_registrations(SYNTH_SOURCE)
        self.assertEqual(
            [(r["nid"], r["name"], r["handler"], r["origin"]) for r in regs],
            [
                (0x10, "synthAlpha", "h_Alpha", "static"),
                (0x20, "synthBeta", "h_ok", "static"),
                (0x30, "__sceSas_ok", "h_ok", "sas_ok_loop"),
                (0x40, "__sceSas_ok", "h_ok", "sas_ok_loop"),
            ],
        )

    def test_unknown_registration_form_fails_closed(self) -> None:
        src = SYNTH_SOURCE + "\nstatic void late(void) { sr_hle_register(dynamic_nid, \"x\", h_Alpha); }\n"
        with self.assertRaises(ManifestError) as ctx:
            extract_registrations(src)
        self.assertIn("cannot account for", str(ctx.exception))

    def test_duplicate_nid_fails(self) -> None:
        src = SYNTH_SOURCE.replace('0x00000020, "synthBeta"', '0x00000010, "synthBeta"')
        with self.assertRaises(ManifestError) as ctx:
            extract_registrations(src)
        self.assertIn("duplicate NID", str(ctx.exception))

    def test_undefined_handler_fails(self) -> None:
        src = SYNTH_SOURCE + '\nstatic void extra(void) { sr_hle_register(0x00000050, "synthGamma", h_Ghost); }\n'
        with self.assertRaises(ManifestError) as ctx:
            extract_registrations(src)
        self.assertIn("h_Ghost", str(ctx.exception))

    def test_string_literal_occurrences_are_ignored(self) -> None:
        src = SYNTH_SOURCE + '\nstatic const char *hint = "add sr_hle_register(0x%08xu, ...);";\n'
        regs = extract_registrations(src)
        self.assertEqual(len(regs), 4)

    def test_line_commented_registration_is_not_extracted(self) -> None:
        src = SYNTH_SOURCE + '\n// sr_hle_register(0x00000060, "synthDead", h_Alpha);\n'
        self.assertEqual(len(extract_registrations(src)), 4)

    def test_block_commented_registration_is_not_extracted(self) -> None:
        src = SYNTH_SOURCE + '\n/* retired:\n   sr_hle_register(0x00000061, "synthDead", h_Alpha);\n*/\n'
        self.assertEqual(len(extract_registrations(src)), 4)

    def test_if0_registration_is_not_extracted(self) -> None:
        src = SYNTH_SOURCE + (
            "\n#if 0\n"
            'static void dead(void) { sr_hle_register(0x00000062, "synthDead", h_Alpha); }\n'
            "#endif\n"
        )
        regs = extract_registrations(src)
        self.assertEqual([r["nid"] for r in regs], [0x10, 0x20, 0x30, 0x40])

    def test_else_branch_of_if0_is_live(self) -> None:
        src = SYNTH_SOURCE + (
            "\n#if 0\n"
            'static void dead(void) { sr_hle_register(0x00000063, "synthDead", h_Alpha); }\n'
            "#else\n"
            'static void live(void) { sr_hle_register(0x00000064, "synthLive", h_Alpha); }\n'
            "#endif\n"
        )
        regs = extract_registrations(src)
        self.assertIn(0x64, [r["nid"] for r in regs])
        self.assertNotIn(0x63, [r["nid"] for r in regs])

    def test_elif_branch_of_if0_is_live(self) -> None:
        src = SYNTH_SOURCE + (
            "\n#if 0\n"
            'static void dead(void) { sr_hle_register(0x00000066, "synthDead", h_Alpha); }\n'
            "#elif 1\n"
            'static void live(void) { sr_hle_register(0x00000067, "synthLive", h_Alpha); }\n'
            "#endif\n"
        )
        regs = extract_registrations(src)
        nids = [r["nid"] for r in regs]
        self.assertIn(0x67, nids)
        self.assertNotIn(0x66, nids)

    def test_elif_inside_nested_if0_stays_dead(self) -> None:
        src = SYNTH_SOURCE + (
            "\n#if 0\n"
            "#if SOMETHING\n"
            "#elif 1\n"
            'static void dead(void) { sr_hle_register(0x00000068, "synthDead", h_Alpha); }\n'
            "#endif\n"
            "#endif\n"
        )
        self.assertEqual(len(extract_registrations(src)), 4)

    def test_nested_conditionals_inside_if0_stay_dead(self) -> None:
        src = SYNTH_SOURCE + (
            "\n#if 0\n"
            "#ifdef ANYTHING\n"
            "#endif\n"
            'static void dead(void) { sr_hle_register(0x00000065, "synthDead", h_Alpha); }\n'
            "#endif\n"
        )
        self.assertEqual(len(extract_registrations(src)), 4)

    def test_comment_inside_sas_array_cannot_leak_nids(self) -> None:
        src = SYNTH_SOURCE.replace(
            "{ 0x00000030, 0x00000040 }",
            "{ 0x00000030, /* retired 0x00000099 */ 0x00000040 // old 0x000000aa\n }",
        )
        regs = extract_registrations(src)
        self.assertEqual(
            [r["nid"] for r in regs if r["origin"] == "sas_ok_loop"], [0x30, 0x40]
        )

    def test_comment_opener_inside_string_does_not_eat_code(self) -> None:
        src = SYNTH_SOURCE.replace(
            '"synthAlpha"', '"synth/*Alpha"'
        )
        regs = extract_registrations(src)
        self.assertEqual(regs[0]["name"], "synth/*Alpha")
        self.assertEqual(len(regs), 4)

    def test_char_literal_quote_does_not_derail_scanner(self) -> None:
        src = SYNTH_SOURCE.replace(
            "static void init(void) {",
            "static char q = '\"';\nstatic void init(void) {",
        )
        self.assertEqual(len(extract_registrations(src)), 4)


class LiveManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = build_manifest()
        cls.regs = {int(r["nid"], 16): r for r in cls.manifest["registrations"]}

    def test_extraction_scale_matches_hle_c(self) -> None:
        """Every live registration is now an individually named static call.

        hle.c once registered the SAS ADSR/reverb/output setters through an
        anonymous ``sas_ok[]`` NID array whose entries all shared the generic
        ``__sceSas_ok`` label. Issue #75 replaced that loop with one named
        registration per NID so the manifest is truthful per NID, so no live
        registration carries the ``sas_ok_loop`` origin any more. The extractor
        still supports the form -- ``ExtractionTests`` covers it against
        synthetic sources -- because nothing guarantees it will not reappear.
        """
        self.assertGreaterEqual(len(self.regs), 300)
        origins = {r["origin"] for r in self.manifest["registrations"]}
        self.assertEqual(origins, {"static"})

    #: Registrations whose *name* the generated table assigns to a *different*
    #: NID. Each entry is live semantic debt: nid -> (name, canonical nid,
    #: tracking issue).
    #:
    #: Unlike a mislabelled NID -- wrong name, right NID -- this class is
    #: invisible to the nid_names.h cross-check, which looks names up *by NID*
    #: and so simply misses a NID that is not in the table at all.
    #:
    #: sceAtracGetChannel (0x31668bba -> 0x31668baa) was a member and was
    #: corrected: a single-nibble slip whose canonical value the table already
    #: carried, so the fix was mechanical. The two below are not near-misses of
    #: their canonical values and route real dedicated handlers, so correcting
    #: them is a routing change that needs evidence, not a transcription fix.
    #: Absence from the table is not itself proof a NID is fabricated -- the
    #: table has ~1615 entries and the PSP exports more than that -- so what
    #: these registrations actually reach is an open question, not a finding.
    REVERSE_NID_DEBT = {
        0x1579A30A: ("sceUtilityUnloadModule", 0xE49BFE92, 324),
        0x2A6117A5: ("sceUtilityLoadModule", 0x2A2B3DE0, 324),
    }

    def test_no_new_registration_contradicts_the_canonical_table_by_name(self) -> None:
        """Fail closed on registrations the canonical table assigns elsewhere.

        Balanced exactly, in the same spirit as ``meta.WAIVERS``: a newly
        introduced contradiction fails, and so does an entry that no longer
        reproduces, so a fix must retire its record in the same change.
        """
        table_path = ROOT / "src" / "rt" / "nid_names.h"
        table: dict[int, str] = {}
        for m in re.finditer(
            r"\{0x([0-9a-fA-F]{8})u,\s*\"([^\"]+)\"\}",
            table_path.read_text(encoding="utf-8"),
        ):
            table[int(m.group(1), 16)] = m.group(2)
        # A parser that silently yields nothing would make every check below
        # vacuously pass -- the exact fail-open this guard exists to prevent.
        self.assertGreater(len(table), 1000, "nid_names.h parsed implausibly few entries")

        nids_by_name: dict[str, set[int]] = {}
        for nid, name in table.items():
            nids_by_name.setdefault(name, set()).add(nid)

        observed: dict[int, str] = {}
        for r in self.manifest["registrations"]:
            nid = int(r["nid"], 16)
            canonical_nids = nids_by_name.get(r["name"])
            # A name absent from the table carries no claim either way; only a
            # name the table *does* know, under some other NID, is a finding.
            if canonical_nids and nid not in canonical_nids:
                observed[nid] = r["name"]

        self.assertEqual(
            observed,
            {nid: name for nid, (name, _, _) in self.REVERSE_NID_DEBT.items()},
            "a registration's name is assigned to a different NID by "
            "src/rt/nid_names.h; correct the NID, or record it in "
            "REVERSE_NID_DEBT with its tracking issue",
        )
        for nid, (name, canonical_nid, _issue) in self.REVERSE_NID_DEBT.items():
            self.assertEqual(nids_by_name[name], {canonical_nid})
            self.assertNotIn(nid, table)

    def test_sce_atrac_get_channel_uses_the_canonical_nid(self) -> None:
        """Regression for the 0x31668bba -> 0x31668baa transcription fix."""
        self.assertNotIn(0x31668BBA, self.regs)
        self.assertEqual(self.regs[0x31668BAA]["name"], "sceAtracGetChannel")

    def test_sas_registry_routes_canonical_nids_to_distinct_handlers(self) -> None:
        """The SAS NID table must preserve each public signature's handler shape."""
        expected = {
            0x019B25EB: "h_SasSetADSR",
            0x07F58C24: "h_SasGetAllEnvelopeHeights",
            0x267A6DD2: "h_SasRevParam",
            0x2C8E6AB3: "h_SasGetPauseFlag",
            0x33D4AB37: "h_SasRevType",
            0x42778A9F: "h_SasInit",
            0x440CA7D8: "h_SasSetVolume",
            0x4AA9EAD6: "h_SasUnsupportedVoice",
            0x50A14DFC: "h_SasCoreWithMix",
            0x5F9529F6: "h_SasSetSL",
            0x68A46B95: "h_SasGetEndFlag",
            0x7497EA85: "h_SasUnsupportedVoice",
            0x74AE582A: "h_SasGetEnvelopeHeight",
            0x76F01ACA: "h_SasSetKeyOn",
            0x787D04D5: "h_SasSetPause",
            0x99944089: "h_SasSetVoice",
            0x9EC3676A: "h_SasSetADSRmode",
            0xA0CF2FA4: "h_SasSetKeyOff",
            0xA232CBE6: "h_SasUnsupportedVoice",
            0xA3589D81: "h_SasCore",
            0xAD84D37F: "h_SasSetPitch",
            0xB7660A23: "h_SasSetNoise",
            0xBD11B7C2: "h_SasGetGrain",
            0xCBCD4F79: "h_SasSetSimpleADSR",
            0xD1E0A01E: "h_SasSetGrain",
            0xD5A229C9: "h_SasRevEVOL",
            0xD5EBBBCD: "h_SasUnsupportedVoice",
            0xE175EF66: "h_SasGetOutputmode",
            0xE1CD9561: "h_SasSetVoicePCM",
            0xE855BF76: "h_SasSetOutputmode",
            0xF6107F00: "h_SasUnsupportedVoice",
            0xF983B186: "h_SasRevVON",
        }
        sas = {
            nid: r for nid, r in self.regs.items() if r["name"].startswith("__sceSas")
        }
        self.assertEqual(set(sas), set(expected))
        self.assertEqual({nid: r["handler"] for nid, r in sas.items()}, expected)
        self.assertNotIn(0xD5EBBCDC, sas)
        self.assertEqual(sas[0x33D4AB37]["name"], "__sceSasRevType")
        self.assertEqual(sas[0x9EC3676A]["name"], "__sceSasSetADSRmode")
        self.assertNotEqual(sas[0x33D4AB37]["handler"], sas[0xB7660A23]["handler"])
        self.assertNotEqual(sas[0x9EC3676A]["handler"], sas[0xCBCD4F79]["handler"])
        self.assertTrue(all(r["handler"] != "h_ok" for r in sas.values()))

    def test_all_classifications_present(self) -> None:
        classes = {r["classification"] for r in self.manifest["registrations"]}
        self.assertEqual(classes, {"dedicated", "fake_success", "controlled_unsupported"})

    def test_sdk_version_variants_share_the_stateful_handler(self) -> None:
        """Issue #71 comment: every SetCompiledSdkVersion variant must retain state.

        All registered SetCompiledSdkVersion firmware variants -- including
        0x1b4217bc (sceKernelSetCompiledSdkVersion603_605), once a waived
        h_ok defect -- must route to h_SetCompiledSdkVersion so g_sdk_version
        is updated, and no waiver for that NID may remain.
        """
        sdk_variants = [
            r for r in self.manifest["registrations"]
            if r["name"].startswith("sceKernelSetCompiledSdkVersion")
        ]
        self.assertGreaterEqual(len(sdk_variants), 3)
        for r in sdk_variants:
            self.assertEqual(r["handler"], "h_SetCompiledSdkVersion", r["name"])
            self.assertEqual(r["classification"], "dedicated", r["name"])
        for nid in (0x7591C7DB, 0x35669D4C, 0x1B4217BC):
            self.assertEqual(self.regs[nid]["handler"], "h_SetCompiledSdkVersion")
        self.assertEqual(
            self.regs[0x1B4217BC]["name"], "sceKernelSetCompiledSdkVersion603_605"
        )
        findings = {(int(f["nid"], 16), f["finding"]) for f in self.manifest["findings"]}
        self.assertNotIn((0x1B4217BC, "alias_mismatch"), findings)
        self.assertNotIn((0x1B4217BC, "mislabeled_nid"), findings)
        self.assertEqual(
            [w for w in meta.WAIVERS if w["nid"] == 0x1B4217BC],
            [],
            "the 603_605 fix must leave no waiver behind",
        )

    def test_findings_and_waivers_are_in_exact_balance(self) -> None:
        findings = [{**f, "nid": int(f["nid"], 16)} for f in self.manifest["findings"]]
        unwaived, stale = unwaived_and_stale(findings)
        self.assertEqual(unwaived, [], "new finding needs a fix or a waiver with an issue link")
        self.assertEqual(stale, [], "fixed defect must retire its waiver")

    def test_every_waiver_links_an_issue(self) -> None:
        for w in meta.WAIVERS:
            self.assertIn("github.com/Jstar269/nakagawa-recomp/issues", w["issue"])

    def test_curated_statuses_are_valid_and_live(self) -> None:
        handlers = {r["handler"] for r in self.manifest["registrations"]}
        for handler, status in meta.HANDLER_STATUS.items():
            self.assertIn(status, meta.HANDLER_STATUSES)
            self.assertIn(handler, handlers)

    def test_psmf_getters_are_controlled_unsupported(self) -> None:
        self.assertEqual(self.regs[0x46F61F8B]["classification"], "controlled_unsupported")
        self.assertEqual(self.regs[0xB9848A74]["classification"], "controlled_unsupported")

    def test_psmf_controlled_unsupported_cites_no_stale_public_tracker(self) -> None:
        # Public #31 is a closed dependency-bump PR unrelated to the PSMF demux
        # surface; the metadata must describe the refusal contract instead of
        # pointing at a live-but-wrong tracker object.
        source = Path(meta.__file__).read_text(encoding="utf-8")
        self.assertNotIn("issue #31", source)

    def test_committed_baseline_is_current_and_reproducible(self) -> None:
        baseline = json.loads(DEFAULT_BASELINE.read_text(encoding="ascii"))
        self.assertEqual(baseline, manifest_to_baseline(self.manifest))

    def test_manifest_build_is_deterministic(self) -> None:
        a = json.dumps(build_manifest(), sort_keys=True)
        b = json.dumps(build_manifest(), sort_keys=True)
        self.assertEqual(a, b)


class FindingRuleTests(unittest.TestCase):
    def test_alias_rule_flags_prefix_with_wrong_handler(self) -> None:
        regs = [
            {"nid": 0x1, "name": "sceKernelSetCompiledSdkVersion999", "handler": "h_ok", "origin": "static"},
            {"nid": 0x2, "name": "sceKernelSetCompiledSdkVersion", "handler": "h_SetCompiledSdkVersion",
             "origin": "static"},
        ]
        findings = compute_findings(regs)
        self.assertEqual(
            [(f["nid"], f["finding"]) for f in findings], [(0x1, "alias_mismatch")]
        )

    def test_mislabel_rule_uses_canonical_names(self) -> None:
        regs = [{"nid": 0x1B4217BC, "name": "sceKernelSetCompiledSdkVersion603_605",
                 "handler": "h_SetCompiledSdkVersion", "origin": "static"}]
        findings = compute_findings(regs)
        self.assertEqual([f["finding"] for f in findings], [])

    def test_generic_success_handler_forces_fake_success(self) -> None:
        self.assertEqual(hle_manifest.classify("h_ok"), ("fake_success", "stub"))
        self.assertEqual(hle_manifest.classify("h_Anything"), ("dedicated", "unreviewed"))
        self.assertEqual(
            hle_manifest.classify("h_PsmfGetVideo"),
            ("controlled_unsupported", "controlled_unsupported"),
        )

    def test_float_return_nid_rejects_integer_stub(self) -> None:
        regs = [
            {"nid": 0xDBA6C4C4, "name": "sceDisplayGetFramePerSec", "handler": "h_ok",
             "origin": "static"},
        ]
        findings = compute_findings(regs)
        self.assertEqual(
            [(f["nid"], f["finding"], f["handler"]) for f in findings],
            [(0xDBA6C4C4, "float_return_handler_mismatch", "h_ok")],
        )

    def test_float_return_nid_accepts_float_handler(self) -> None:
        regs = [
            {"nid": 0xDBA6C4C4, "name": "sceDisplayGetFramePerSec", "handler": "h_DisplayGetFramePerSec",
             "origin": "static"},
        ]
        findings = compute_findings(regs)
        self.assertEqual(findings, [])

    def test_float_return_metadata_is_live_and_dedicated(self) -> None:
        manifest = build_manifest()
        regs = {r["nid"]: r for r in manifest["registrations"]}
        entry = regs["0xdba6c4c4"]
        self.assertEqual(entry["name"], "sceDisplayGetFramePerSec")
        self.assertNotEqual(entry["handler"], "h_ok")
        self.assertNotEqual(entry["classification"], "fake_success")
        self.assertEqual(entry["status"], "complete")
        self.assertIn(entry["handler"], meta.FLOAT_RETURN_HANDLERS)


class MpegDirtyNotificationContractTests(unittest.TestCase):
    def test_mpeg_avc_decode_dirty_notification_invariants(self) -> None:
        mpeg_src = (ROOT / "src" / "rt" / "mpeg.c").read_text(encoding="utf-8")
        self.assertIn("gotFrame = sr_h264_frame(ctx->h264, eos, buffer,", mpeg_src,
                      "mpeg_avc_decode must pass guest buffer address to sr_h264_frame")
        self.assertIn("if (gotFrame <= 0 && !ctx->h264Frames) {", mpeg_src,
                      "mpeg_avc_decode must scope full-buffer dirty notification strictly to clear_video_buffer")

    def test_h264_mf_convert_frame_dirty_notification_invariants(self) -> None:
        h264_src = (ROOT / "src" / "rt" / "h264_mf.c").read_text(encoding="utf-8")
        self.assertIn("uint64_t fullBytes = (uint64_t)(uint32_t)h * pitchBytes64;", h264_src,
                      "convert_frame must compute full-buffer dirty bytes without signed overflow")
        self.assertIn("if (w == frameWidth && fullBytes <= UINT32_MAX)", h264_src,
                      "convert_frame must emit a single dirty call only for a representable contiguous span")
        self.assertIn("sr_gpu_vram_dirty(buffer + (uint32_t)y * pitchBytes, rowBytes);", h264_src,
                      "convert_frame must emit exact per-row dirty calls when w < frameWidth")



CHAIN_SOURCE = """
static uint32_t h_Prod(CpuState *s) { (void)s; return 0; }
static uint32_t h_Shared(CpuState *s) { (void)s; return 0; }
static uint32_t h_ok(CpuState *s) { (void)s; return 0; }
void sr_hle_register(uint32_t nid, const char *name, HleFn fn) { }

static void register_shared(void) {
    sr_hle_register(0x00000050u, "synthShared", h_Shared);
}

void sr_hle_init(void) {
#ifdef SR_HLE_THREAD_SELFTEST
    register_shared();
    sr_hle_register(0x00000060u, "synthTestOnly", h_ok);
#else
    register_shared();
    sr_hle_register(0x00000070u, "synthProdOnly", h_Prod);
#endif
    sr_hle_register(0x00000080u, "synthAlways", h_Prod);
}
"""


class RegistrationScopeTests(unittest.TestCase):
    """Production reachability is what makes a registration production evidence."""

    def setUp(self) -> None:
        self.scopes = hle_manifest.registration_scopes(CHAIN_SOURCE)

    def test_helper_called_from_both_branches_is_reachable_from_production(self) -> None:
        entry = self.scopes[0x50]
        self.assertEqual(entry["declared_in"], "register_shared")
        self.assertEqual(entry["reachable_from"], ["production", "selftest"])

    def test_registration_only_under_the_selftest_macro_is_not_production(self) -> None:
        self.assertEqual(self.scopes[0x60]["reachable_from"], ["selftest"])

    def test_registration_in_the_else_branch_is_production_only(self) -> None:
        self.assertEqual(self.scopes[0x70]["reachable_from"], ["production"])

    def test_registration_outside_any_conditional_is_reachable_from_both(self) -> None:
        self.assertEqual(self.scopes[0x80]["reachable_from"], ["production", "selftest"])

    def test_unrelated_conditionals_do_not_narrow_production_reachability(self) -> None:
        source = CHAIN_SOURCE.replace(
            'sr_hle_register(0x00000080u, "synthAlways", h_Prod);',
            "#ifdef SR_SOME_OTHER_FEATURE\n"
            '    sr_hle_register(0x00000080u, "synthAlways", h_Prod);\n'
            "#endif",
        )
        self.assertEqual(
            hle_manifest.registration_scopes(source)[0x80]["reachable_from"],
            ["production", "selftest"],
            "a registration guarded by an unrelated #ifdef still ships; treating it "
            "as selftest-only would understate production reachability",
        )


class ConformanceJoinTests(unittest.TestCase):
    def test_probe_rows_group_by_dispatched_nid(self) -> None:
        header = (
            "static const IcProbe kIcMatrix[] = {\n"
            '{ "sceKernelWaitSema", 0x4e3a1105u, "Bad sema", ICG_SEMA, 0,\n'
            "  {0, 1, 2, 3}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW}},\n"
            '{ "sceKernelWaitSema", 0x4e3a1105u, "Valid sema", ICG_SEMA, 2,\n'
            "  {0, 4, 5, 6}, {ICU, CNW, ILCTX, CNW}, {ICB, CNW, IC_NOTRUN, CNW}},\n"
            "};\n"
        )
        cells = hle_manifest.conformance_cells(header)
        self.assertEqual(
            cells[0x4E3A1105],
            [
                {"api": "sceKernelWaitSema", "scenario": "Bad sema"},
                {"api": "sceKernelWaitSema", "scenario": "Valid sema"},
            ],
        )

    def test_text_before_the_matrix_is_not_scanned(self) -> None:
        self.assertEqual(hle_manifest.conformance_cells('{ "notAProbe", 0x1u, "" }'), {})

    def test_selftest_dispatch_needs_the_define_to_be_used(self) -> None:
        used = "#define NID_USED 0x11111111u\n" "  x = sr_syscall(&cpu, NID_USED);\n"
        unused = "#define NID_UNUSED 0x22222222u\n"
        found = hle_manifest.selftest_dispatched_nids(used + unused)
        self.assertIn(0x11111111, found)
        self.assertNotIn(
            0x22222222, found, "an unreferenced NID define is not an exercise"
        )

    def test_selftest_dispatch_sees_a_bare_literal(self) -> None:
        self.assertIn(
            0x05572A5F,
            hle_manifest.selftest_dispatched_nids("(void)sr_syscall(&cpu, 0x05572a5fu);"),
        )


class EvidenceTierTests(unittest.TestCase):
    @staticmethod
    def _entry(classification="dedicated", cells=(), selftest=False, oracle=(),
               reach=("production", "selftest")):
        return {
            "registration": {"classification": classification, "reachable_from": list(reach)},
            "exercised": {
                "conformance_cells": list(cells),
                "hle_selftest_dispatch": selftest,
                "psp_oracle_probes": list(oracle),
            },
        }

    def test_a_hardware_probe_outranks_host_coverage(self) -> None:
        tier, _ = hle_manifest.evidence_tier(self._entry(oracle=["PSP-SMOKE-001"]))
        self.assertEqual(tier, "HARDWARE_MEASURED")

    def test_executable_coverage_of_a_dedicated_handler_is_host_tested(self) -> None:
        tier, _ = hle_manifest.evidence_tier(self._entry(selftest=True))
        self.assertEqual(tier, "HOST_TESTED")

    def test_exercising_a_generic_stub_is_not_evidence(self) -> None:
        tier, why = hle_manifest.evidence_tier(
            self._entry(classification="fake_success", cells=[{"api": "x", "scenario": ""}])
        )
        self.assertEqual(
            tier, "NOT_EVIDENCE",
            "entering h_ok proves the registry resolves, not that the API behaves",
        )
        self.assertIn("stub", why)

    def test_a_dedicated_handler_with_no_coverage_is_only_static(self) -> None:
        tier, _ = hle_manifest.evidence_tier(self._entry())
        self.assertEqual(tier, "STATICALLY_SUPPORTED")

    def test_every_tier_is_in_the_declared_vocabulary(self) -> None:
        for entry in (
            self._entry(oracle=["p"]), self._entry(selftest=True),
            self._entry(classification="fake_success"), self._entry(),
        ):
            self.assertIn(hle_manifest.evidence_tier(entry)[0], hle_manifest.EVIDENCE_TIERS)


class LiveEvidenceChainTests(unittest.TestCase):
    """The chain over the real tree, checked for shape rather than for a count."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.chain = hle_manifest.build_evidence_chain()
        cls.by_nid = {e["nid"]: e for e in cls.chain["entries"]}

    def test_every_registration_appears_exactly_once(self) -> None:
        self.assertEqual(len(self.chain["entries"]), self.chain["registrations"])
        self.assertEqual(len(self.by_nid), self.chain["registrations"])

    def test_every_registration_is_reachable_from_the_production_runtime(self) -> None:
        self.assertEqual(
            self.chain["summary"]["not_reachable_from_production"], [],
            "a registration the shipping runtime cannot reach is either dead or a "
            "test-only registration masquerading as production evidence",
        )

    def test_tier_counts_cover_every_entry(self) -> None:
        self.assertEqual(
            sum(self.chain["summary"]["by_tier"].values()), len(self.chain["entries"])
        )

    def test_a_label_whose_nid_is_not_reproducible_carries_a_shape(self) -> None:
        for entry in self.chain["entries"]:
            if entry["nid_derivation"]["independently_derived"]:
                self.assertIsNone(entry["nid_derivation"]["shape"])
            else:
                shape = entry["nid_derivation"]["shape"]
                self.assertIsNotNone(shape, entry["nid"])
                self.assertIn(shape["classification"], nid_name_proof.CLASSIFICATIONS)

    def test_the_imported_link_is_unknown_without_an_import_manifest(self) -> None:
        sample = next(iter(self.chain["entries"]))
        self.assertEqual(
            sample["imported"], "unknown: no import manifest supplied",
            "the retail import manifest is a private input; its absence must never "
            "be reported as 'this NID is not imported'",
        )

    def test_exercised_stubs_are_reported_rather_than_counted_as_coverage(self) -> None:
        for stub in self.chain["summary"]["exercised_stubs"]:
            entry = self.by_nid[stub["nid"]]
            self.assertTrue(entry["exercised_stub"])
            self.assertEqual(entry["evidence_tier"], "NOT_EVIDENCE")

    def test_a_hardware_probe_api_reaches_its_registration(self) -> None:
        manifest = json.loads(hle_manifest.PSP_ORACLE_MANIFEST.read_text(encoding="utf-8"))
        apis = hle_manifest.oracle_exercised_apis(manifest)
        registered = {e["canonical_name"] for e in self.chain["entries"]}
        for api, probes in apis.items():
            if api in registered:
                entry = next(e for e in self.chain["entries"] if e["canonical_name"] == api)
                self.assertEqual(entry["exercised"]["psp_oracle_probes"], probes)


if __name__ == "__main__":
    unittest.main()
