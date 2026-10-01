# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_manifest
from psp_oracle.protocol import (
    ProtocolError,
    compare_texts,
    decode_psp_model_code,
    parse_output,
    provenance_issues,
    validate_dmac_size_matrix,
    validate_dmac_size_matrix_size,
    DMAC_SIZE_MATRIX_SIZES,
    DMAC_SIZE_MATRIX_TRIALS,
)
from psp_oracle.run_psplink import (
    _record_summary,
    _split_command,
    _validate_host0_capture,
    annotate_terminal_outcome,
    main as run_psplink_main,
)


META = (
    "NAKAGAWA_PSP_META schema=1 source={source} model={model} firmware={firmware} "
    "binary_sha256={binary} source_commit={commit}\n"
)

MEASURED_SHA = "a" * 64
MEASURED_COMMIT = "b" * 40
STAGED_PRX = b"synthetic PSP oracle probe"
STAGED_SHA = hashlib.sha256(STAGED_PRX).hexdigest()


def stream(source: str, result: str = "0x1") -> str:
    return META.format(
        source=source, model="synthetic", firmware="test", binary="0" * 64, commit="0" * 40
    ) + ("NAKAGAWA_PSP_TEST schema=1 test_id=SMOKE case_id=one status=PASS result=" + result + "\n")


def dmac_matrix_stream() -> str:
    lines = [META.format(
        source="psp", model="PSP-3000", firmware="6.61-ARK",
        binary=MEASURED_SHA, commit=MEASURED_COMMIT,
    )]
    for api, api_name in enumerate(("memcpy", "try")):
        for size in DMAC_SIZE_MATRIX_SIZES:
            allocation_bytes = (size + 0x2FFF) & ~0xFFF
            lines.append(
                "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
                f"case_id=size-matrix-{api_name}-0x{size:08x} status=PASS "
                f"result=0x0 out0=0x{size:x} out1=0x{size:x} out2=0x0 "
                f"out3=0x1 out4=0x10 out5=0x{api:x} "
                f"out6=0x{DMAC_SIZE_MATRIX_TRIALS:x} out7=0x0 out8=0x0 out9=0x0 "
                f"out10=0x0 out11=0x1000 out12=0x{allocation_bytes:x} "
                f"out13=0x2 out14=0x1000 out15=0x8801000 out16=0x8c01000 "
                f"out17=0x1 out18=0x2\n"
            )
    return "".join(lines)


def dmac_matrix_cell_stream(size: int) -> str:
    lines = [META.format(
        source="psp", model="PSP-3000", firmware="6.61-ARK",
        binary=MEASURED_SHA, commit=MEASURED_COMMIT,
    )]
    allocation_bytes = (size + 0x2FFF) & ~0xFFF
    for api, api_name in enumerate(("memcpy", "try")):
        lines.append(
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
            f"case_id=size-matrix-{api_name}-0x{size:08x} status=PASS "
            f"result=0x0 out0=0x{size:x} out1=0x{size:x} out2=0x0 "
            f"out3=0x1 out4=0x10 out5=0x{api:x} "
            f"out6=0x{DMAC_SIZE_MATRIX_TRIALS:x} out7=0x0 out8=0x0 out9=0x0 "
            f"out10=0x0 out11=0x1000 out12=0x{allocation_bytes:x} "
            f"out13=0x2 out14=0x1000 out15=0x8801000 out16=0x8c01000 "
            f"out17=0x1 out18=0x2\n"
        )
    return "".join(lines)


def measured_stream(source: str, result: str = "0x1") -> str:
    """A stream whose provenance fields are host-measured, not fixture defaults."""

    return META.format(
        source=source,
        model="PSP-2000",
        firmware="6.61-ME",
        binary=MEASURED_SHA,
        commit=MEASURED_COMMIT,
    ) + ("NAKAGAWA_PSP_TEST schema=1 test_id=SMOKE case_id=one status=PASS result=" + result + "\n")


def device_identity_stream(*, binary: str, commit: str | None) -> str:
    """A complete DMAC stream with a controlled device META identity."""

    text = dmac_matrix_stream()
    first_line, remainder = text.split("\n", 1)
    first_line = first_line.replace(f"binary_sha256={MEASURED_SHA}", f"binary_sha256={binary}")
    if commit is None:
        first_line = re.sub(r" source_commit=[^ ]+", "", first_line)
    else:
        first_line = first_line.replace(
            f"source_commit={MEASURED_COMMIT}", f"source_commit={commit}"
        )
    return first_line + "\n" + remainder


def identity_args(tmp: Path, **overrides: object) -> SimpleNamespace:
    binary = tmp / "probe.prx"
    binary.write_bytes(STAGED_PRX)
    args = SimpleNamespace(
        validate_dmac_size_matrix=True,
        binary=binary,
        source_commit=MEASURED_COMMIT,
        model="PSP-3000",
        firmware="6.61-ARK",
        model_code=None,
    )
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


class PspOracleProtocolTests(unittest.TestCase):
    def test_pspsdk_model_ordinal_three_is_psp3000_generation_04g(self) -> None:
        self.assertEqual(decode_psp_model_code(3), ("04g", "PSP-3000"))
        self.assertEqual(decode_psp_model_code(4), ("05g", "PSP-N1000"))

    def test_pspsdk_model_decoder_rejects_unknown_or_non_integer_values(self) -> None:
        for value in (-1, 8, True, "3"):
            with self.assertRaises(ValueError):
                decode_psp_model_code(value)  # type: ignore[arg-type]

    def test_parser_requires_metadata_and_orders_records(self) -> None:
        parsed = parse_output(stream("psp"))
        self.assertEqual(parsed.metadata_dict()["source"], "psp")
        self.assertEqual(parsed.results[0].key(), ("SMOKE", "one"))

    def test_duplicate_case_is_rejected(self) -> None:
        with self.assertRaises(ProtocolError):
            parse_output(stream("psp") + stream("psp"))

    def test_comparison_distinguishes_match_difference_and_only(self) -> None:
        match = compare_texts(stream("psp"), stream("nakagawa"))
        self.assertEqual(match["comparisons"][0]["comparison"], "MATCH")
        difference = compare_texts(stream("psp", "0x2"), stream("nakagawa"))
        self.assertEqual(difference["comparisons"][0]["comparison"], "DIFFERENCE")
        psp_only = compare_texts(stream("psp"), stream("nakagawa") +
                                 "NAKAGAWA_PSP_TEST schema=1 test_id=EXTRA case_id=one status=PASS result=0x2\n")
        self.assertIn(psp_only["comparisons"][1]["comparison"], {"NAKAGAWA_ONLY", "MATCH"})

    def test_malformed_hex_is_rejected(self) -> None:
        with self.assertRaises(ProtocolError):
            parse_output(stream("psp", "not-hex"))

    def test_source_commit_requires_a_full_40_or_64_digit_object_id(self) -> None:
        for length in (12, 41, 63):
            with self.subTest(length=length):
                text = measured_stream("psp").replace(
                    f"source_commit={MEASURED_COMMIT}",
                    f"source_commit={'b' * length}",
                    1,
                )
                with self.assertRaises(ProtocolError):
                    parse_output(text)


class GeCorpusGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.corpus = json.loads(
            (self.root / "fixtures" / "psp_oracle" / "ge_corpus.json").read_text(encoding="utf-8")
        )

    def _run_gate(self, document: dict | None = None) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(self.root / "tools" / "psp_oracle" / "run_psplink.py"), "--ge-corpus-gate"]
        if document is None:
            return subprocess.run(command, capture_output=True, text=True, check=False)
        fixture_dir = self.root / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="ge-corpus-gate-", dir=fixture_dir) as scratch:
            corpus_path = Path(scratch) / "corpus.json"
            corpus_path.write_text(json.dumps(document), encoding="utf-8")
            command.extend(["--ge-corpus", str(corpus_path)])
            return subprocess.run(command, capture_output=True, text=True, check=False)

    def _report(self, completed: subprocess.CompletedProcess[str]) -> dict:
        self.assertTrue(completed.stdout, completed.stderr)
        return json.loads(completed.stdout)

    def test_unmeasured_ge_case_reports_not_run(self) -> None:
        completed = self._run_gate()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = self._report(completed)
        self.assertEqual(report["semantic_boundary"], "GE_RASTER_PIXEL_CONFORMANCE")
        self.assertEqual(report["tracking_issue"], 343)
        self.assertEqual(report["cases"][0]["status"], "NOT_RUN")

    def test_software_record_labelled_psp_hardware_is_refused(self) -> None:
        document = json.loads(json.dumps(self.corpus))
        case = document["cases"][0]
        case["source_tier"] = "PSP_HARDWARE"
        case["framebuffer_sha256"] = "a" * 64
        for pixel in case["selected_pixels"]:
            pixel["pixel_value"] = 0xFF0000FF if (pixel["x"], pixel["y"]) == (4, 4) else 0
        raw_result = stream("nakagawa") + (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-GE-001 "
            f"case_id={case['case_id']} status=PASS framebuffer_sha256={'a' * 64} "
            "pixel_0_0=0x00000000 pixel_4_4=0xff0000ff pixel_12_12=0x00000000\n"
        )
        case["evidence_envelope"] = {
            "EVIDENCE_CLASS": "PSP_HARDWARE",
            "ACCEPTANCE_ELIGIBLE": True,
            "CASE_ID": case["case_id"],
            "CONSOLE_MODEL": "PSP-3000",
            "FW": "6.61-ARK",
            "SOURCE_COMMIT": MEASURED_COMMIT,
            "BINARY_SHA256": MEASURED_SHA,
            "RAW_RESULT": raw_result,
        }
        completed = self._run_gate(document)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = self._report(completed)
        result = report["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("source 'nakagawa'", result["reason"])

    def test_hardware_label_requires_acceptance_eligible_envelope(self) -> None:
        document = json.loads(json.dumps(self.corpus))
        case = document["cases"][0]
        case["source_tier"] = "PSP_HARDWARE"
        case["framebuffer_sha256"] = "a" * 64
        for pixel in case["selected_pixels"]:
            pixel["pixel_value"] = 0
        case["evidence_envelope"] = {
            "EVIDENCE_CLASS": "PSP_HARDWARE",
            "ACCEPTANCE_ELIGIBLE": False,
            "CASE_ID": case["case_id"],
            "CONSOLE_MODEL": "PSP-3000",
            "FW": "6.61-ARK",
            "SOURCE_COMMIT": MEASURED_COMMIT,
            "BINARY_SHA256": MEASURED_SHA,
            "RAW_RESULT": stream("psp"),
        }
        completed = self._run_gate(document)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        result = self._report(completed)["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("not acceptance-eligible", result["reason"])

    def test_schema_violation_is_refused_with_case_field_name(self) -> None:
        document = json.loads(json.dumps(self.corpus))
        document["cases"][0]["framebuffer"]["format"] = "888x"
        completed = self._run_gate(document)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        report = self._report(completed)
        result = report["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("framebuffer.format", result["reason"])


class PspDmacProtocolTests(unittest.TestCase):
    def test_size_matrix_validator_requires_all_sizes_and_trials(self) -> None:
        parsed = validate_dmac_size_matrix(dmac_matrix_stream())
        self.assertEqual(len(parsed.results), 16)

    def test_size_matrix_validator_rejects_missing_boundary_case(self) -> None:
        text = dmac_matrix_stream().replace(
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
            "case_id=size-matrix-try-0x0000c001", "# removed", 1
        )
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix(text)

    def test_size_matrix_validator_rejects_changed_post_request_guard(self) -> None:
        text = dmac_matrix_stream().replace("out9=0x0", "out9=0x1", 1)
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix(text)

    def test_size_matrix_single_size_validator_requires_both_api_cells(self) -> None:
        text = dmac_matrix_cell_stream(0xBFFF)
        parsed = validate_dmac_size_matrix_size(text, 0xBFFF)
        self.assertEqual(len(parsed.results), 2)
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix_size(text, 0xC000)
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix_size(text.split("NAKAGAWA_PSP_TEST", 2)[0], 0xBFFF)

    def test_size_matrix_validator_rejects_overlapping_or_unowned_spans(self) -> None:
        text = dmac_matrix_cell_stream(0xC000).replace("out16=0x8c01000", "out16=0x8801000", 1)
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix_size(text, 0xC000)
        text = dmac_matrix_cell_stream(0xC000).replace("out17=0x1", "out17=0x0", 1)
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix_size(text, 0xC000)

    def test_size_matrix_validator_requires_post_request_guard(self) -> None:
        text = dmac_matrix_stream().replace(" out9=0x0", "", 1)
        with self.assertRaises(ProtocolError):
            validate_dmac_size_matrix(text)


class PspOracleAcceptanceGateTests(unittest.TestCase):
    """A MATCH built from fixture placeholders must not read as acceptance evidence."""

    def test_placeholder_metadata_matches_but_is_not_acceptance_eligible(self) -> None:
        report = compare_texts(stream("psp"), stream("nakagawa"))
        self.assertEqual(report["classification"], "MATCH")
        self.assertFalse(report["acceptance_eligible"])
        blockers = " | ".join(report["acceptance_blockers"])
        self.assertIn("binary_sha256 is the all-zero fixture placeholder", blockers)
        self.assertIn("source_commit is the all-zero fixture placeholder", blockers)

    def test_measured_metadata_is_acceptance_eligible(self) -> None:
        report = compare_texts(measured_stream("psp"), measured_stream("nakagawa"))
        self.assertEqual(report["classification"], "MATCH")
        self.assertTrue(report["acceptance_eligible"])
        self.assertEqual(report["acceptance_blockers"], [])

    def test_difference_with_measured_provenance_is_still_acceptance_eligible(self) -> None:
        report = compare_texts(measured_stream("psp", "0x2"), measured_stream("nakagawa"))
        self.assertEqual(report["classification"], "DIFFERENCE")
        self.assertTrue(report["acceptance_eligible"])

    def test_unknown_model_or_firmware_blocks_acceptance(self) -> None:
        text = META.format(
            source="psp",
            model="unknown",
            firmware="unknown",
            binary=MEASURED_SHA,
            commit=MEASURED_COMMIT,
        ) + "NAKAGAWA_PSP_TEST schema=1 test_id=SMOKE case_id=one status=PASS result=0x1\n"
        report = compare_texts(text, measured_stream("nakagawa"))
        self.assertFalse(report["acceptance_eligible"])
        blockers = " | ".join(report["acceptance_blockers"])
        self.assertIn("psp: model is the fixture placeholder", blockers)
        self.assertIn("psp: firmware is the fixture placeholder", blockers)

    def test_swapped_streams_block_acceptance(self) -> None:
        report = compare_texts(measured_stream("nakagawa"), measured_stream("psp"))
        self.assertFalse(report["acceptance_eligible"])
        blockers = " | ".join(report["acceptance_blockers"])
        self.assertIn("psp: stream declares source='nakagawa'", blockers)
        self.assertIn("nakagawa: stream declares source='psp'", blockers)

    def test_unparseable_stream_is_inconclusive_and_not_eligible(self) -> None:
        report = compare_texts("garbage", measured_stream("nakagawa"))
        self.assertEqual(report["classification"], "INCONCLUSIVE")
        self.assertFalse(report["acceptance_eligible"])

    def test_emulator_capture_cannot_be_promoted_to_hardware_evidence(self) -> None:
        """A PPSSPP headless run is a smoke test, never a PSP oracle result."""

        ppsspp = META.format(
            source="ppsspp",
            model="PPSSPP",
            firmware="6.61",
            binary=MEASURED_SHA,
            commit=MEASURED_COMMIT,
        ) + "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SMOKE-001 case_id=sum-1-to-100 status=PASS result=0x13ba\n"
        report = compare_texts(ppsspp, measured_stream("nakagawa"))
        # Even with fully measured provenance, the source role must disqualify it.
        self.assertFalse(report["acceptance_eligible"])
        self.assertIn(
            "psp: stream declares source='ppsspp', not 'psp'",
            report["acceptance_blockers"],
        )

    def test_probe_emits_ppsspp_source_only_under_emulation(self) -> None:
        """The probe must label emulator output distinctly at the source."""

        probe = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "probe.c"
        source = probe.read_text(encoding="utf-8")
        self.assertIn('emulated ? "ppsspp" : "psp"', source)
        # The emulator sink is the PPSSPP devctl, not printf; both must be present.
        self.assertIn("EMULATOR_DEVCTL_SEND_OUTPUT", source)
        self.assertIn("EMULATOR_DEVCTL_IS_EMULATOR", source)

    def test_checked_in_fixture_metadata_is_reported_as_placeholder(self) -> None:
        probe = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "probe.c"
        source = probe.read_text(encoding="utf-8")
        self.assertIn("model=unknown firmware=unknown", source)
        issues = provenance_issues(
            {
                "model": "unknown",
                "firmware": "unknown",
                "binary_sha256": "0" * 64,
                "source_commit": "0" * 40,
            }
        )
        self.assertEqual(len(issues), 4)

    def test_priority_experiment_requires_a_started_peer_and_high_thread(self) -> None:
        probe = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "probe.c"
        source = probe.read_text(encoding="utf-8")
        start = source.index("static void dwp_run")
        end = source.index("static void run_display_wait_priority", start)
        body = source[start:end]
        self.assertIn("int low_started = 0;", body)
        self.assertRegex(
            body,
            r"const int low_start\s*=\s*sceKernelStartThread\(low, 0, NULL\)",
        )
        self.assertIn("if (!with_low || low_started)", body)
        self.assertIn("const int high_start = sceKernelStartThread(high, 0, NULL);", body)
        self.assertRegex(
            body,
            r"setup_ok\s*&&\s*high_started\s*&&\s*g_dwp\.iters\s*==\s*DWP_ITERS",
        )
        self.assertIn("if (!low_started)", body)


class PspOracleDeviceIdentityTests(unittest.TestCase):
    """The capture must bind the device's own identity, not a host rewrite."""

    def _report(
        self, tmp: Path, *, binary: str, commit: str | None
    ) -> dict[str, object]:
        return _validate_host0_capture(
            device_identity_stream(binary=binary, commit=commit), identity_args(tmp)
        )

    def test_matching_device_identity_is_eligible(self) -> None:
        with tempfile.TemporaryDirectory(prefix="identity-match-") as name:
            report = self._report(Path(name), binary=STAGED_SHA, commit=MEASURED_COMMIT)
        self.assertEqual(report["SOURCE_COMMIT_BINDING"], "MATCH")
        self.assertEqual(report["BINARY_SHA256_BINDING"], "MATCH")
        self.assertEqual(report["DEVICE_IDENTITY_STATUS"], "MATCH")
        self.assertTrue(report["acceptance_eligible"])

    def test_device_source_commit_mismatch_is_not_overwritten(self) -> None:
        mismatch = "d" * 40
        with tempfile.TemporaryDirectory(prefix="identity-mismatch-") as name:
            report = self._report(Path(name), binary=STAGED_SHA, commit=mismatch)
        self.assertEqual(report["SOURCE_COMMIT_BINDING"], "MISMATCH")
        self.assertEqual(report["metadata"]["source_commit"], mismatch)
        self.assertFalse(report["acceptance_eligible"])
        self.assertIn("IDENTITY_MISMATCH", " | ".join(report["DEVICE_IDENTITY_BLOCKERS"]))

    def test_device_binary_digest_mismatch_is_not_overwritten(self) -> None:
        mismatch = "d" * 64
        with tempfile.TemporaryDirectory(prefix="identity-digest-") as name:
            report = self._report(Path(name), binary=mismatch, commit=MEASURED_COMMIT)
        self.assertEqual(report["BINARY_SHA256_BINDING"], "MISMATCH")
        self.assertEqual(report["metadata"]["binary_sha256"], mismatch)
        self.assertFalse(report["acceptance_eligible"])
        self.assertIn("IDENTITY_MISMATCH", " | ".join(report["DEVICE_IDENTITY_BLOCKERS"]))

    def test_zero_or_absent_device_commit_is_not_bound(self) -> None:
        for commit, expected_status in (("0" * 40, "PLACEHOLDER"), (None, "NOT_REPORTED")):
            with self.subTest(commit=commit):
                with tempfile.TemporaryDirectory(prefix="identity-unbound-") as name:
                    report = self._report(Path(name), binary=STAGED_SHA, commit=commit)
                self.assertEqual(report["SOURCE_COMMIT_BINDING"], expected_status)
                self.assertFalse(report["acceptance_eligible"])
                self.assertIn(
                    "IDENTITY_NOT_BOUND",
                    " | ".join(report["DEVICE_IDENTITY_BLOCKERS"]),
                )

    def test_short_commit_prefix_never_satisfies_binding(self) -> None:
        with tempfile.TemporaryDirectory(prefix="identity-short-") as name:
            report = self._report(
                Path(name), binary=STAGED_SHA, commit=MEASURED_COMMIT[:12]
            )
        self.assertEqual(report["SOURCE_COMMIT_BINDING"], "NOT_BOUND")
        self.assertFalse(report["acceptance_eligible"])
        self.assertIn("short prefix never binds", " | ".join(report["DEVICE_IDENTITY_BLOCKERS"]))

    def test_full_64_digit_commit_is_supported(self) -> None:
        full_commit = "c" * 64
        with tempfile.TemporaryDirectory(prefix="identity-sha256-") as name:
            report = _validate_host0_capture(
                device_identity_stream(binary=STAGED_SHA, commit=full_commit),
                identity_args(Path(name), source_commit=full_commit),
            )
        self.assertEqual(report["SOURCE_COMMIT_BINDING"], "MATCH")
        self.assertTrue(report["acceptance_eligible"])

    def test_uppercase_full_device_commit_matches_case_insensitively(self) -> None:
        with tempfile.TemporaryDirectory(prefix="identity-uppercase-") as name:
            report = self._report(
                Path(name), binary=STAGED_SHA, commit=MEASURED_COMMIT.upper()
            )
        self.assertEqual(report["SOURCE_COMMIT_BINDING"], "MATCH")
        self.assertEqual(report["metadata"]["source_commit"], MEASURED_COMMIT)
        self.assertTrue(report["acceptance_eligible"])

    def test_partial_provenance_keeps_device_placeholders_visible(self) -> None:
        with tempfile.TemporaryDirectory(prefix="identity-partial-") as name:
            args = identity_args(
                Path(name), binary=None, source_commit=None, model="PSP-3000"
            )
            report = _validate_host0_capture(
                device_identity_stream(binary="0" * 64, commit="0" * 40), args
            )
        self.assertEqual(report["metadata"]["source_commit"], "0" * 40)
        self.assertEqual(report["metadata"]["binary_sha256"], "0" * 64)
        self.assertFalse(report["acceptance_eligible"])

    def test_makefile_rejects_unbound_or_dirty_probe_builds(self) -> None:
        makefile = (
            Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "Makefile"
        ).read_text(encoding="utf-8")
        self.assertIn("git rev-parse --verify HEAD", makefile)
        self.assertIn("git status --porcelain --untracked-files=normal", makefile)
        self.assertIn("PROBE_BUILD_COMMIT_FULL", makefile)
        self.assertIn("PROBE_BUILD_COMMIT must match the clean checkout HEAD", makefile)
        make = shutil.which("mingw32-make") or shutil.which("make")
        if make is None:
            self.skipTest("GNU Make is unavailable")
        makefile_path = (
            Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        )
        missing = subprocess.run(
            [make, "-C", str(makefile_path), "PROBE_BUILD_COMMIT_FULL="],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn(
            "PROBE_BUILD_COMMIT could not be determined",
            missing.stdout + missing.stderr,
        )
        # git status never lists an empty directory, so the scratch directory
        # needs a file for the checkout to read as dirty.
        with tempfile.TemporaryDirectory(
            prefix="probe-build-dirty-", dir=makefile_path
        ) as scratch:
            (Path(scratch) / "untracked.txt").write_text("dirty\n", encoding="utf-8")
            completed = subprocess.run(
                [make, "-C", str(makefile_path)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("source tree is dirty", completed.stdout + completed.stderr)

    def test_probe_emits_build_commit_macro_not_a_zero_placeholder(self) -> None:
        probe = (
            Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "probe.c"
        ).read_text(encoding="utf-8")
        self.assertIn('"source_commit=" PROBE_BUILD_COMMIT', probe)
        self.assertIn("#error \"PROBE_BUILD_COMMIT must be the full git object id", probe)
        self.assertNotIn("source_commit=0000000000000000000000000000000000000000", probe)


class PspOracleRunnerTests(unittest.TestCase):
    def test_dmac_host0_capture_uses_the_runner_validation_path(self) -> None:
        args = SimpleNamespace(
            validate_dmac_size_matrix=True,
            binary=None,
            source_commit=None,
            model=None,
            firmware=None,
            model_code=None,
        )
        report = _validate_host0_capture(dmac_matrix_stream(), args)
        self.assertEqual(report["classification"], "PASS")
        self.assertEqual(report["test_record_count"], 16)
        self.assertTrue(report["acceptance_eligible"])

    def test_capture_records_distinguish_result_skip_and_no_record(self) -> None:
        self.assertEqual(_record_summary("transport only\n"), ("NO_RECORD", 0))
        self.assertEqual(_record_summary(stream("psp")), ("RESULT_RECORDS", 1))
        skipped = stream("psp").replace("status=PASS", "status=SKIP")
        self.assertEqual(_record_summary(skipped), ("SKIP_RECORDS", 1))

    def test_hang_and_reset_require_human_annotation_of_no_record_capture(self) -> None:
        base = {"mode": "capture", "process_status": "TIMEOUT", "stdout_file": "capture.txt"}
        hang = annotate_terminal_outcome(base, b"transport only\n", "HANG")
        reset = annotate_terminal_outcome(base, b"transport only\n", "RESET")
        self.assertEqual(hang["terminal_outcome"], "HANG")
        self.assertEqual(reset["terminal_outcome"], "RESET")
        self.assertEqual(hang["terminal_outcome_source"], "human-observed")
        self.assertFalse(hang["acceptance_eligible"])
        with self.assertRaises(ValueError):
            annotate_terminal_outcome(base, stream("psp").encode(), "HANG")

    def test_windows_pspsh_payload_quotes_are_removed_once(self) -> None:
        command = (
            r'C:\PSPHacks\psplinkusb-windows\pspsh.exe '
            r'-e "ldstart host0:/nakagawa_psp_oracle.prx"'
        )
        self.assertEqual(
            _split_command(command),
            [
                r"C:\PSPHacks\psplinkusb-windows\pspsh.exe",
                "-e",
                "ldstart host0:/nakagawa_psp_oracle.prx",
            ],
        )

    def test_capture_uses_selected_scratch_results_directory(self) -> None:
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(
            prefix="run-psplink-", dir=root / "fixtures" / "psp_oracle"
        ) as scratch_name:
            scratch = Path(scratch_name)
            results = scratch / "results"
            report = scratch / "capture.json"
            with patch(
                "psp_oracle.run_psplink._run_command",
                return_value=(
                    0,
                    "NAKAGAWA_PSP_TEST schema=1 test_id=TEST case_id=1 status=PASS\n",
                    "",
                    "PROCESS_EXITED",
                ),
            ):
                self.assertEqual(
                    run_psplink_main(
                        [
                            "--command", "fake-pspsh",
                            "--results-directory", str(results),
                            "--out", str(report),
                        ]
                    ),
                    0,
                )
            payload = json.loads(report.read_text(encoding="utf-8"))
            captured = (root / payload["stdout_file"]).resolve()
            self.assertEqual(captured.parent, results.resolve())
            self.assertIn("status=PASS", captured.read_text(encoding="utf-8"))

    def test_model_code_is_derived_without_the_old_n1000_mapping(self) -> None:
        command = [
            sys.executable,
            str(Path(__file__).resolve().parent / "psp_oracle" / "run_psplink.py"),
            "--dry-run",
            "--model-code",
            "3",
        ]
        # Dry-run still validates/derives the model before reporting the plan.
        completed = subprocess.run(
            command, capture_output=True, text=True, check=True
        )
        plan = json.loads(completed.stdout)
        self.assertFalse(plan["provenance_supplied"])
        self.assertEqual(plan["model"], "PSP-3000-04g")
        self.assertEqual(plan["model_code"], 3)

    def test_nakagawa_mode_reuses_production_selftest_and_derives_records(self) -> None:
        root = Path(__file__).resolve().parents[1]
        source = (root / "src" / "rt" / "hle_thread_selftest.c").read_text(encoding="utf-8")
        hle_source = (root / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        makefile = (root / "Makefile").read_text(encoding="utf-8")
        self.assertIn("sr_syscall", source)
        self.assertIn("oracle_sha256_file", source)
        self.assertIn("GetModuleFileNameA", source)
        self.assertNotIn("oracle_sha256_file(args->artifact", source)
        self.assertIn(
            '"status=%s result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x',
            source,
        )
        self.assertNotIn("status=PASS result=", source)
        self.assertIn("psp-oracle-nakagawa: hle-thread-selftest-build", makefile)
        self.assertIn("tools/psp_oracle/run_nakagawa.py", makefile)
        self.assertIn("src/rt/hle.c", makefile)
        self.assertIn("sched_delete_thread", hle_source)
        self.assertIn("-Wl,--no-insert-timestamp", makefile)
        self.assertIn("PSP-SMOKE-001", source)
        self.assertIn("sr_psp_oracle_smoke_sum", source)
        self.assertNotIn("sum_u32(uint32_t", source)
        self.assertIn("tools/psp_oracle/build_nakagawa_smoke.py", makefile)
        self.assertIn("psp-oracle-nakagawa-smoke", makefile)

    def test_smoke_builder_is_source_owned_and_does_not_emit_records(self) -> None:
        builder = Path(__file__).resolve().parent / "psp_oracle" / "build_nakagawa_smoke.py"
        text = builder.read_text(encoding="utf-8")
        self.assertIn('TOOLS / "codegen.py"', text)
        self.assertIn("nakagawa_psp_oracle_sum_u32", text)
        self.assertNotIn("NAKAGAWA_PSP_TEST", text)

    def test_oracle_capture_launcher_preserves_child_stdout(self) -> None:
        launcher = Path(__file__).resolve().parent / "psp_oracle" / "run_nakagawa.py"
        self.assertTrue(launcher.is_file())
        self.assertIn("stdout=subprocess.PIPE", launcher.read_text(encoding="utf-8"))


class PspOracleBuildRouteTests(unittest.TestCase):
    """Keep the merged CASE/Makefile/import contract structurally explicit."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )
        self.fixture = self.root / "fixtures" / "psp_oracle"

    def test_supported_case_names_map_to_unique_case_ids(self) -> None:
        routes = re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        )
        self.assertEqual(len(routes), 56)
        names = [name for name, _ in routes]
        ids = [int(case_id) for _, case_id in routes]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(set(ids), set(range(1, 57)))
        self.assertNotIn("psp_b1_imports.S", self.makefile)
        self.assertNotIn("psp_b2_imports.S", self.makefile)
        self.assertNotIn("psp_b3_imports.S", self.makefile)

    def test_mutex_import_block_is_limited_to_mutex_cases(self) -> None:
        self.assertIn("OBJS = $(BUILD_DIR)/probe.o\n", self.makefile)
        self.assertIn("MUTEX_CASES = mutex-refer-unlocked mutex-timeout-quanta", self.makefile)
        self.assertIn("mutex-priority-inheritance mutex-interrupt-context", self.makefile)
        self.assertIn("ifneq ($(filter $(MUTEX_CASES),$(CASE)),)\n", self.makefile)
        self.assertIn("OBJS += $(BUILD_DIR)/mutex_imports.o\n", self.makefile)
        self.assertNotIn(
            "OBJS = $(BUILD_DIR)/probe.o $(BUILD_DIR)/mutex_imports.o",
            self.makefile,
        )

    def test_kernel_object_routes_use_shared_threadman_import_block(self) -> None:
        for case, source, macro in (("kobj-b1", "psp_b1.c", "B1"), ("wait-b2", "psp_b2.c", "B2"), ("kernel-b3", "psp_b3.c", "B3")):
            start = self.makefile.rfind(f"else ifeq ($(CASE),{case})")
            self.assertGreaterEqual(start, 0)
            end = self.makefile.find("else ifeq", start + 1)
            if end < 0:
                end = self.makefile.find("endif", start + 1)
            body = self.makefile[start:end]
            self.assertIn(f"TARGET = $(BUILD_DIR)/{source[:-2]}", body)
            self.assertIn(f"OBJS = $(BUILD_DIR)/{source[:-2]}.o $(BUILD_DIR)/threadman_user_imports.o", body)
            self.assertIn(f"CFLAGS += -D{macro}_BUILD_COMMIT=", body)
        shared = (self.fixture / "threadman_user_imports.S").read_text(encoding="utf-8")
        for symbol in (
            "sceKernelCreateCallback", "sceKernelCreateEventFlag", "sceKernelCreateFpl",
            "sceKernelCreateMsgPipe", "sceKernelCreateMbx", "sceKernelWaitEventFlag",
            "sceKernelTerminateDeleteThread", "sceKernelGetThreadExitStatus",
        ):
            self.assertIn(symbol, shared)

    def test_b_kernel_object_recipes_are_unique_and_import_sources_are_reachable(self) -> None:
        for stem in ("psp_b1", "psp_b2", "psp_b3"):
            self.assertEqual(len(re.findall(rf"^\$\(BUILD_DIR\)/{stem}\.o:", self.makefile, re.MULTILINE)), 1)
            self.assertEqual(len(re.findall(rf"^\$\(BUILD_DIR\)/{stem}_imports\.o:", self.makefile, re.MULTILINE)), 0)
            self.assertFalse((self.fixture / f"{stem}_imports.S").exists())
        self.assertEqual(len(re.findall(r"^\$\(BUILD_DIR\)/threadman_user_imports\.o:", self.makefile, re.MULTILINE)), 1)
        self.assertIn("threadman_user_imports.S", self.makefile)


class PspDmacProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.probe = (self.root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        self.makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )

    def assert_probe_contains(self, needle: str) -> None:
        self.assertTrue(needle in self.probe, f"probe is missing required source: {needle}")

    def test_dmac_cases_are_individually_buildable_and_use_the_real_imports(self) -> None:
        for case in (
            "dma-concurrency",
            "dma-invalid-tail-memcpy-dst",
            "dma-invalid-tail-memcpy-src",
            "dma-invalid-tail-try-dst",
            "dma-invalid-tail-try-src",
            "dma-size-matrix",
            "dma-size-matrix-cell",
        ):
            self.assertIn(f"else ifeq ($(CASE),{case})", self.makefile)
        self.assertIn("LIBS = -lpspdmac", self.makefile)
        self.assertIn("sceDmacMemcpy(dst, src, size)", self.probe)
        self.assertIn("sceDmacTryMemcpy(dst, src, size)", self.probe)

    def test_concurrency_probe_separates_a_caller_window_from_busy(self) -> None:
        self.assertIn("#define DMAC_CONCURRENCY_TRIALS 64u", self.probe)
        self.assertIn("start_window_count", self.probe)
        self.assertIn("timeline_overlap_count", self.probe)
        self.assertIn("second_busy_count", self.probe)
        self.assertIn("busy_while_first_pending_count", self.probe)
        self.assertIn("busy_after_first_return_count", self.probe)
        self.assertIn('"concurrent-memcpy-try"', self.probe)
        self.assertIn('"concurrent-try-try"', self.probe)
        self.assertIn('"concurrent-try-memcpy"', self.probe)

    def test_invalid_tail_probe_fails_closed_before_the_call(self) -> None:
        self.assertIn("PSP_LARGE_MEMORY = 0", self.makefile)
        self.assertIn("sceKernelAllocPartitionMemory", self.probe)
        self.assertIn("PSP_SMEM_High", self.probe)
        self.assertIn("DMAC_BOUNDARY_BLOCK_BYTES", self.probe)
        self.assertIn("candidate_tail", self.probe)
        self.assertNotIn("DMAC_BASELINE_USER_END", self.probe)
        self.assertNotIn("DMAC_BOUNDARY_BLOCK_BASE", self.probe)
        self.assertIn('emit_dmac_invalid_setup(emulated, "SKIP"', self.probe)
        self.assertIn("DMAC_INVALID_REQUEST (DMAC_MEASURED_PREFIX + 1u)", self.probe)
        self.assertNotIn("boundary_prefix[DMAC_MEASURED_PREFIX]", self.probe)

    def test_invalid_tail_probe_never_dma_accesses_unowned_memory(self) -> None:
        marker = "static void run_dmac_invalid_tail(int emulated) {"
        self.assertIn(marker, self.probe)
        body = self.probe.split(marker, 1)[1].split("\n}\n#endif", 1)[0]
        self.assertNotIn("dmac_call(", body)
        self.assertIn('emit_dmac_invalid_setup(emulated, "SKIP"', body)

    def test_size_matrix_covers_boundaries_repeats_and_cache_guards(self) -> None:
        self.assert_probe_contains("DMAC_SIZE_TRIALS 3u")
        self.assert_probe_contains("DMAC_SIZE_ALIGNMENT 0x1000u")
        self.assert_probe_contains("DMAC_SIZE_REDZONE_BYTES 0x1000u")
        for size in ("0x0000bfffu", "0x0000c000u", "0x0000c001u", "0x0000d000u",
                     "0x0000f000u", "0x0000ffffu", "0x00010000u", "0x00100000u"):
            self.assert_probe_contains(size)
        for needle in (
            "DMAC_SIZE_REQUEST",
            "sceKernelDcacheWritebackRange",
            "sceKernelDcacheInvalidateRange",
            "dmac_size_guard_mutations",
            "source_block_uid",
            "destination_block_uid",
            'PROBE_HOST0_LOG "host0:/dmac_size_matrix_log.txt"',
            '"size-matrix-%s-0x%08x"',
        ):
            self.assert_probe_contains(needle)

    def test_size_matrix_allocates_its_data_and_guard_spans(self) -> None:
        body = self.probe.split("static void run_dmac_size_matrix", 1)[1].split(
            "\n}\n#endif", 1
        )[0]
        matrix_source = self.probe.split("#define DMAC_SIZE_PARTITION", 1)[1].split(
            "#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_DMAC_CONCURRENCY", 1
        )[0]
        allocation_helpers = self.probe.split("struct dmac_size_buffer {", 1)[1].split(
            "static void dmac_size_reset_source", 1
        )[0]
        for needle in (
            "sceKernelAllocPartitionMemory",
            "PSP_SMEM_High",
            "sceKernelGetBlockHeadAddr",
            "DMAC_SIZE_PARTITION",
            "DMAC_SIZE_REDZONE_BYTES",
        ):
            self.assertTrue(
                needle in allocation_helpers,
                f"matrix allocation helper does not establish ownership: {needle}",
            )
        self.assertTrue("dmac_size_spans_overlap" in body, "allocated spans are not checked")
        self.assertFalse(
            "0x04000000u" in matrix_source,
            "matrix still uses fixed VRAM destination",
        )
        self.assertFalse("0x04100000u" in matrix_source, "matrix still uses fixed VRAM source")

    def test_size_matrix_syncs_both_owned_allocations_for_each_transfer(self) -> None:
        body = self.probe.split("static void run_dmac_size_matrix", 1)[1].split(
            "\n}\n#endif", 1
        )[0]
        transfer = body.index("dmac_call(")
        trial_loop = body.rindex("for (uint32_t trial", 0, transfer)
        before_transfer = body[trial_loop:transfer]
        self.assertGreaterEqual(before_transfer.count("dmac_size_cache_sync("), 2)
        self.assertTrue("source_buffer" in before_transfer, "source is not synced per transfer")
        self.assertTrue(
            "destination_buffer" in before_transfer,
            "destination is not synced per transfer",
        )
        after_transfer = body[transfer:]
        self.assertTrue(
            "sceKernelDcacheInvalidateRange(source_buffer.head" in after_transfer,
            "source is not invalidated after transfer",
        )
        self.assertTrue(
            "sceKernelDcacheInvalidateRange(destination_buffer.head" in after_transfer,
            "destination is not invalidated after transfer",
        )

    def test_size_matrix_build_can_isolate_one_size_per_session(self) -> None:
        self.assertTrue("DMAC_SIZE_REQUEST" in self.makefile, "Makefile cannot select one size")
        self.assertTrue("-DDMAC_SIZE_REQUEST=" in self.makefile, "selector is not passed to PSP C")
        self.assert_probe_contains("dmac_size_matrix_cell_log.txt")

    def test_size_matrix_records_are_closed_before_the_next_cell(self) -> None:
        body = self.probe.split("static void run_dmac_size_matrix", 1)[1].split(
            "\n}\n#endif", 1
        )[0]
        self.assertRegex(
            body,
            re.compile(
                r"for \(uint32_t i = 0;.*?for \(uint32_t api = 0;.*?"
                r"for \(uint32_t trial = 0;.*?emit_record_extended\(.*?"
                r"dmac_size_release_buffer",
                re.DOTALL,
            ),
        )
        writer = self.probe.split("static void emit_record_extended", 1)[1].split(
            "\n}\n#endif", 1
        )[0]
        self.assertLess(writer.index("sceIoWrite(fd"), writer.index("sceIoClose(fd)"))

    def test_model_profile_uses_the_user_bridge_and_raw_firmware_word(self) -> None:
        self.assertIn("PSP_ORACLE_CASE_MODEL_PROFILE", self.probe)
        self.assertIn("kuKernelGetModel()", self.probe)
        self.assertIn("sceKernelDevkitVersion()", self.probe)
        self.assertIn('PROBE_HOST0_LOG "host0:/model_profile_log.txt"', self.probe)
        self.assertIn("-lpspkubridge", self.makefile)

    def test_hardware_records_have_host0_mirrors_when_stdout_route_is_absent(self) -> None:
        # A one-shot PSPLink capture can contain only the load reply when
        # pluser stdout is unavailable. Keep transport and #303 DMAC results
        # on host0 so capture does not depend on that route.
        stdout_capture = "Load/Start host0:/transport-write.prx UID: 0x12345678\n"
        self.assertEqual(_record_summary(stdout_capture), ("NO_RECORD", 0))
        for case, filename in (
            ("PSP_ORACLE_CASE_TRANSPORT_WRITE", "transport_write_log.txt"),
            ("PSP_ORACLE_CASE_DMAC_SURVEY", "dmac_survey_log.txt"),
            ("PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_DST", "dmac_invalid_tail_memcpy_dst_log.txt"),
            ("PSP_ORACLE_CASE_DMAC_INVALID_TAIL_MEMCPY_SRC", "dmac_invalid_tail_memcpy_src_log.txt"),
            ("PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_DST", "dmac_invalid_tail_try_dst_log.txt"),
            ("PSP_ORACLE_CASE_DMAC_INVALID_TAIL_TRY_SRC", "dmac_invalid_tail_try_src_log.txt"),
            ("PSP_ORACLE_CASE_DMAC_SIZE_MATRIX_CELL", "dmac_size_matrix_cell_log.txt"),
        ):
            self.assertIn(
                f'#elif PSP_ORACLE_CASE == {case}\n#define PROBE_HOST0_LOG "host0:/{filename}"',
                self.probe,
            )
        self.assertIn("sceIoOpen(PROBE_HOST0_LOG", self.probe)

    def test_probe_preprocessor_guards_are_balanced(self) -> None:
        open_guards: list[int] = []
        for line_number, line in enumerate(self.probe.splitlines(), 1):
            directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|endif)\b", line)
            if not directive:
                continue
            if directive.group(1) == "endif":
                self.assertTrue(open_guards, f"extra #endif at line {line_number}")
                open_guards.pop()
            else:
                open_guards.append(line_number)
        self.assertEqual(open_guards, [], f"unclosed #if at lines {open_guards}")

    def _oracle_manifest(self) -> dict:
        return json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )

    def test_system_manifest_exposes_the_model_profile_case(self) -> None:
        system = next(
            entry for entry in self._oracle_manifest()["tests"] if entry["id"] == "PSP-SYSTEM-001"
        )
        self.assertEqual(system["status"], "implemented")
        self.assertEqual(system["hardware_evidence"], "CAPTURED")
        self.assertEqual(system["case_ids"], ["model-profile"])

    def test_implemented_status_alone_is_not_accepted_hardware_evidence(self) -> None:
        """`status: implemented` is probe-source prose, never hardware evidence."""
        manifest = self._oracle_manifest()
        oracle_apis = hle_manifest.oracle_exercised_apis(manifest)
        for test_id in ("PSP-TRANSPORT-001", "PSP-SYSTEM-001"):
            entry = next(item for item in manifest["tests"] if item["id"] == test_id)
            self.assertEqual(entry["status"], "implemented")
            self.assertEqual(entry["hardware_evidence"], "CAPTURED")
            for api in entry["apis"]:
                self.assertNotIn(
                    api,
                    oracle_apis,
                    f"{api} reaches HARDWARE_MEASURED from a probe source, not a run",
                )

    def test_fpu_and_cache_probe_families_are_named_as_not_run(self) -> None:
        manifest = self._oracle_manifest()
        by_id = {entry["id"]: entry for entry in manifest["tests"]}
        for test_id in ("PSP-FPU-001", "PSP-CACHE-001"):
            self.assertEqual(by_id[test_id]["status"], "planned")
            self.assertEqual(by_id[test_id]["hardware_evidence"], "NOT_RUN")
        self.assertEqual(len(by_id), len(manifest["tests"]))

    def test_manifest_hardware_evidence_uses_closed_vocabulary(self) -> None:
        manifest = self._oracle_manifest()
        vocabulary = {"NOT_RUN", "CAPTURED", "MEASURED"}
        for entry in manifest["tests"]:
            with self.subTest(test_id=entry["id"]):
                self.assertIn("hardware_evidence", entry)
                self.assertIsInstance(entry["hardware_evidence"], str)
                self.assertIn(entry["hardware_evidence"], vocabulary)

    def test_only_documented_measured_groups_claim_hardware_evidence(self) -> None:
        manifest = self._oracle_manifest()
        evidence = {entry["id"]: entry["hardware_evidence"] for entry in manifest["tests"]}
        self.assertEqual(
            {test_id for test_id, value in evidence.items() if value == "MEASURED"},
            {"PSP-DMAC-001", "PSP-DISPLAY-001", "PSP-EXCEPTION-001"},
        )
        self.assertEqual(
            {test_id for test_id, value in evidence.items() if value == "CAPTURED"},
            {"PSP-KERNEL-001", "PSP-TRANSPORT-001", "PSP-SYSTEM-001"},
        )

    def test_measured_rows_cite_a_public_document_and_the_cases_it_measures(self) -> None:
        """A MEASURED row must name publication-eligible evidence and its own cases."""
        include_paths = set(
            json.loads(
                (self.root / "assets" / "public_source_profile.json").read_text(
                    encoding="utf-8"
                )
            )["include_paths"]
        )
        measured = [
            entry
            for entry in self._oracle_manifest()["tests"]
            if entry["hardware_evidence"] == "MEASURED"
        ]
        self.assertTrue(measured)
        for entry in measured:
            with self.subTest(test_id=entry["id"]):
                match = re.fullmatch(
                    r"(docs/[A-Za-z0-9_./-]+\.md)#([a-z0-9-]+)", entry["evidence_ref"]
                )
                self.assertIsNotNone(match, "evidence_ref must be 'docs/....md#section'")
                self.assertIn(match.group(1), include_paths)
                cases = entry["evidence_cases"]
                self.assertTrue(cases)
                self.assertTrue(
                    set(cases) <= set(entry.get("case_ids", [])) | set(entry["apis"])
                )
                self.assertTrue(entry["measurement_note"].strip())

    def test_rows_without_measured_evidence_say_why(self) -> None:
        for entry in self._oracle_manifest()["tests"]:
            if entry["hardware_evidence"] == "MEASURED":
                continue
            with self.subTest(test_id=entry["id"]):
                self.assertTrue(
                    entry.get("evidence_note", "").strip(),
                    "an unmeasured row must state why it is not measured",
                )

    def test_exception_campaign_is_named_with_its_buildable_cases(self) -> None:
        entry = next(
            item
            for item in self._oracle_manifest()["tests"]
            if item["id"] == "PSP-EXCEPTION-001"
        )
        self.assertEqual(entry["hardware_evidence"], "MEASURED")
        makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )
        for case_id in entry["case_ids"]:
            with self.subTest(case_id=case_id):
                self.assertIn(f"else ifeq ($(CASE),{case_id})", makefile)
        # exception-a2 is measured as run PSP-A2-01, but the cited section names
        # its fixture only inside the range `exception-a1`..`exception-a3`.
        self.assertIn("exception-a2", entry["case_ids"])
        self.assertNotIn("exception-a2", entry["evidence_cases"])

    def test_manifest_routes_issue_23_to_dedicated_scalar_probe(self) -> None:
        manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        dmac = next(entry for entry in manifest["tests"] if entry["id"] == "PSP-DMAC-001")
        self.assertEqual(dmac["issues"], [23])
        self.assertEqual(dmac["hardware_evidence"], "MEASURED")
        self.assertEqual(len(dmac["case_ids"]), 23)
        self.assertIn("size-matrix-memcpy-0x0000bfff", dmac["case_ids"])
        self.assertIn("size-matrix-memcpy-0x0000c001", dmac["case_ids"])
        self.assertIn("missing record is never PASS", dmac["reset"])
        self.assertEqual(
            set(dmac["outcome_contract"]),
            {"result", "skip", "hang", "reset", "inconclusive"},
        )

    def test_manifest_routes_mailbox_measurement_to_339_and_341(self) -> None:
        manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        kernel = next(
            entry for entry in manifest["tests"] if entry["id"] == "PSP-KERNEL-001"
        )
        self.assertIn("mbx-delete-wait", kernel["diagnostic_case_ids"])
        self.assertIn("mbx-timeout-control", kernel["diagnostic_case_ids"])
        self.assertIn(339, kernel["issues"])
        self.assertIn(341, kernel["issues"])
        for api in (
            "sceKernelCreateMbx",
            "sceKernelDeleteMbx",
            "sceKernelReceiveMbx",
            "sceKernelReferMbxStatus",
            "sceKernelReferThreadStatus",
            "sceKernelWaitThreadEnd",
        ):
            self.assertIn(api, kernel["apis"])


class PspMailboxCleanupTests(unittest.TestCase):
    def test_probe_cleans_one_mailbox_and_joins_or_terminates_receiver(self) -> None:
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is unavailable for the synthetic mailbox cleanup harness")

        probe = (root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        start = probe.rindex(
            "#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_MBX_DELETE_WAIT"
        )
        end = probe.index("#endif", start) + len("#endif")
        mailbox_probe = probe[start:end]
        harness = r'''#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PSP_ORACLE_CASE_MBX_DELETE_WAIT 56
#define PSP_ORACLE_CASE PSP_ORACLE_CASE_MBX_DELETE_WAIT
#define PSP_THREAD_WAITING 4u
#define THREAD_ATTR_USER 0

typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef int32_t SceUID;
typedef int (*ThreadEntry)(SceSize, void *);
typedef struct {
    int size;
    uint32_t status;
    uint32_t waitType;
    SceUID waitId;
} SceKernelThreadInfo;
typedef struct {
    int size;
    uint32_t numWaitThreads;
} SceKernelMbxInfo;

SceUID sceKernelCreateMbx(const char *, int, void *);
SceUID sceKernelCreateThread(const char *, ThreadEntry, int, int, int, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelReferThreadStatus(SceUID, SceKernelThreadInfo *);
int sceKernelDeleteMbx(SceUID);
int sceKernelWaitThreadEnd(SceUID, SceUInt *);
int sceKernelDeleteThread(SceUID);
int sceKernelTerminateDeleteThread(SceUID);
int sceKernelReferMbxStatus(SceUID, SceKernelMbxInfo *);
int sceKernelReceiveMbx(SceUID, void **, SceUInt *);
uint32_t sceKernelGetSystemTimeLow(void);
void emit_record_extended(int, const char *, const char *, const char *,
                          uint32_t, const uint32_t *, size_t);

/* MAILBOX PROBE SNIPPET */

static int live_mailboxes;
static int live_threads;
static int mailbox_active[32];
static int mailbox_delete_calls[32];
static int mailbox_creates;
static int thread_status_queries;
static int clock_queries;
static int timeout_join;
static int terminate_delete_calls;

static void reset_state(int should_timeout_join) {
    memset(mailbox_active, 0, sizeof(mailbox_active));
    memset(mailbox_delete_calls, 0, sizeof(mailbox_delete_calls));
    live_mailboxes = 0;
    live_threads = 0;
    mailbox_creates = 0;
    thread_status_queries = 0;
    clock_queries = 0;
    timeout_join = should_timeout_join;
    terminate_delete_calls = 0;
}

SceUID sceKernelCreateMbx(const char *name, int attr, void *options) {
    (void)name;
    (void)attr;
    (void)options;
    const SceUID uid = mailbox_creates++ == 0 ? 10 : 20;
    mailbox_active[uid] = 1;
    live_mailboxes++;
    return uid;
}

SceUID sceKernelCreateThread(const char *name, ThreadEntry entry, int priority,
                             int stack_size, int attr, void *options) {
    (void)name;
    (void)entry;
    (void)priority;
    (void)stack_size;
    (void)attr;
    (void)options;
    live_threads++;
    return 30;
}

int sceKernelStartThread(SceUID thread, SceSize args, void *argp) {
    (void)thread;
    (void)args;
    (void)argp;
    s_mbx_receive_entry_us = 100;
    return 0;
}

int sceKernelReferThreadStatus(SceUID thread, SceKernelThreadInfo *status) {
    (void)thread;
    if (thread_status_queries++ == 0) {
        status->status = PSP_THREAD_WAITING;
        status->waitType = 5;
        status->waitId = 10;
    }
    return 0;
}

int sceKernelDeleteMbx(SceUID uid) {
    mailbox_delete_calls[uid]++;
    if (!mailbox_active[uid]) {
        return -1;
    }
    mailbox_active[uid] = 0;
    live_mailboxes--;
    return 0;
}

int sceKernelWaitThreadEnd(SceUID thread, SceUInt *timeout) {
    (void)thread;
    *timeout = 0;
    if (timeout_join) {
        return -1;
    }
    s_mbx_receive_rc = (int)0x800201b5u;
    s_mbx_receive_message = &s_mbx_message_sentinel;
    s_mbx_receive_remaining_us = 0;
    s_mbx_receive_return_us = 250;
    s_mbx_receive_completion_count = 1;
    return 0;
}

int sceKernelDeleteThread(SceUID thread) {
    (void)thread;
    live_threads--;
    return 0;
}

int sceKernelTerminateDeleteThread(SceUID thread) {
    (void)thread;
    terminate_delete_calls++;
    live_threads--;
    return 0;
}

int sceKernelReferMbxStatus(SceUID uid, SceKernelMbxInfo *status) {
    (void)uid;
    status->numWaitThreads = 0;
    return 0;
}

int sceKernelReceiveMbx(SceUID uid, void **message, SceUInt *timeout) {
    (void)uid;
    (void)message;
    *timeout = 0;
    return -1;
}

uint32_t sceKernelGetSystemTimeLow(void) {
    static const uint32_t values[] = {200, 300, 50300};
    const size_t index = (size_t)clock_queries++;
    return values[index < sizeof(values) / sizeof(values[0]) ? index : 2];
}

void emit_record_extended(int emulated, const char *test_id, const char *case_id,
                          const char *status, uint32_t result,
                          const uint32_t *out, size_t count) {
    (void)emulated;
    (void)test_id;
    (void)case_id;
    (void)status;
    (void)result;
    (void)out;
    (void)count;
}

int main(void) {
    reset_state(0);
    if (!run_mbx_delete_wait(0)) {
        return 1;
    }
    if (mailbox_delete_calls[10] != 1) {
        return 2; /* redundant delete after the primary mailbox was removed */
    }
    if (mailbox_delete_calls[20] != 1 || live_mailboxes != 0 || live_threads != 0) {
        return 3;
    }

    reset_state(1);
    if (run_mbx_delete_wait(0)) {
        return 4;
    }
    if (mailbox_delete_calls[10] != 1) {
        return 5;
    }
    if (mailbox_delete_calls[20] != 1 || terminate_delete_calls != 1 ||
        live_mailboxes != 0 || live_threads != 0) {
        return 6;
    }
    return 0;
}
'''.replace("/* MAILBOX PROBE SNIPPET */", mailbox_probe)

        fixture = root / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="mbx-cleanup-host-", dir=fixture) as scratch:
            source_path = Path(scratch) / "mbx_cleanup_host.c"
            binary_path = Path(scratch) / "mbx_cleanup_host.exe"
            source_path.write_text(harness, encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source_path), "-o", str(binary_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            executed = subprocess.run(
                [str(binary_path)], capture_output=True, text=True, check=False
            )
            self.assertEqual(executed.returncode, 0, executed.stderr)


class PspMutexProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.probe = (self.root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        self.makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )
        self.imports = (self.root / "fixtures" / "psp_oracle" / "mutex_imports.S").read_text(
            encoding="utf-8"
        )

    def test_mutex_cases_are_individually_buildable(self) -> None:
        for case in (
            "mutex-refer-unlocked",
            "mutex-timeout-quanta",
            "mutex-priority-inheritance",
            "mutex-interrupt-context",
        ):
            self.assertIn(f"else ifeq ($(CASE),{case})", self.makefile)

    def test_mutex_imports_assembly_declares_all_plain_mutex_nids(self) -> None:
        for symbol in (
            "sceKernelCreateMutex",
            "sceKernelDeleteMutex",
            "sceKernelLockMutex",
            "sceKernelLockMutexCB",
            "sceKernelTryLockMutex",
            "sceKernelUnlockMutex",
            "sceKernelCancelMutex",
            "sceKernelReferMutexStatus",
        ):
            self.assertIn(symbol, self.imports)

    def test_mutex_imports_declares_contiguous_threadman_user_stubs(self) -> None:
        # Mutex cases link custom ThreadManForUser NIDs; declaring IMPORT_START
        # emits __stub_module_ThreadManForUser which suppresses the SDK library header.
        # All probe and CRT ThreadManForUser functions must be declared here to keep
        # the stub section contiguous in .sceStub.text (issue #400).
        for symbol in (
            "sceKernelCreateThread",
            "sceKernelStartThread",
            "sceKernelExitThread",
            "sceKernelDelayThread",
            "sceKernelWaitThreadEnd",
            "sceKernelCreateSema",
            "sceKernelWaitSema",
            "sceKernelSignalSema",
            "sceKernelCheckCallback",
            "sceKernelCreateCallback",
        ):
            self.assertIn(symbol, self.imports)

    def test_makefile_fails_on_fixup_imports_warning(self) -> None:
        # Issue #400: psp-fixup-imports warns 'stubs out of order' on disjoint
        # stub runs while exiting 0. The Makefile post-link hook must treat this as fatal.
        self.assertIn("FIXUP =", self.makefile)
        self.assertIn("psp-fixup-imports", self.makefile)
        self.assertIn("stubs out of order", self.makefile)
        self.assertIn("exit 1", self.makefile)

    def test_phaseb_makefile_fails_on_fixup_imports_warning(self) -> None:
        phaseb_makefile = (
            self.root / "fixtures" / "psp_phaseb" / "Makefile"
        ).read_text(encoding="utf-8")
        self.assertIn("FIXUP =", phaseb_makefile)
        self.assertIn("psp-fixup-imports", phaseb_makefile)
        self.assertIn("stubs out of order", phaseb_makefile)
        self.assertIn("exit 1", phaseb_makefile)

    def test_probe_implements_all_four_unresolved_mutex_cases(self) -> None:
        self.assertIn("run_mutex_refer_unlocked_case", self.probe)
        self.assertIn("run_mutex_timeout_quanta_case", self.probe)
        self.assertIn("run_mutex_priority_inheritance_case", self.probe)
        self.assertIn("run_mutex_interrupt_context_case", self.probe)

    def test_manifest_registers_psp_mutex_001(self) -> None:
        manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        mutex = next(entry for entry in manifest["tests"] if entry["id"] == "PSP-MUTEX-001")
        self.assertEqual(mutex["group"], "mutex")
        self.assertEqual(mutex["issues"], [2])
        self.assertEqual(
            mutex["case_ids"],
            [
                "mutex-refer-unlocked",
                "mutex-timeout-quanta",
                "mutex-priority-inheritance",
                "mutex-interrupt-context",
            ],
        )


if __name__ == "__main__":
    unittest.main()
