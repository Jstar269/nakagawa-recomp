# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

from psp_oracle.protocol import (
    ProtocolError,
    compare_texts,
    decode_psp_model_code,
    parse_output,
    provenance_issues,
    validate_dmac_size_matrix,
    DMAC_SIZE_MATRIX_SIZES,
    DMAC_SIZE_MATRIX_TRIALS,
)
from psp_oracle.run_psplink import (
    _record_summary,
    _split_command,
    _validate_host0_capture,
    annotate_terminal_outcome,
)


META = (
    "NAKAGAWA_PSP_META schema=1 source={source} model={model} firmware={firmware} "
    "binary_sha256={binary} source_commit={commit}\n"
)

MEASURED_SHA = "a" * 64
MEASURED_COMMIT = "b" * 40


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
            lines.append(
                "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
                f"case_id=size-matrix-{api_name}-0x{size:08x} status=PASS "
                f"result=0x0 out0=0x{size:x} out1=0x{size:x} out2=0x0 "
                f"out3=0x1 out4=0x10 out5=0x{api:x} "
                f"out6=0x{DMAC_SIZE_MATRIX_TRIALS:x} out7=0x0 out8=0x0\n"
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


class PspDmacProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.probe = (self.root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        self.makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )

    def test_dmac_cases_are_individually_buildable_and_use_the_real_imports(self) -> None:
        for case in (
            "dma-concurrency",
            "dma-invalid-tail-memcpy-dst",
            "dma-invalid-tail-memcpy-src",
            "dma-invalid-tail-try-dst",
            "dma-invalid-tail-try-src",
            "dma-size-matrix",
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

    def test_size_matrix_covers_boundaries_repeats_and_cache_guards(self) -> None:
        self.assertIn("DMAC_SIZE_BYTES 0x00100000u", self.probe)
        self.assertIn("DMAC_SIZE_TRIALS 3u", self.probe)
        for size in ("0x0000bfffu", "0x0000c000u", "0x0000c001u", "0x0000d000u",
                     "0x0000f000u", "0x0000ffffu", "0x00010000u", "0x00100000u"):
            self.assertIn(size, self.probe)
        self.assertIn("sceKernelDcacheWritebackRange", self.probe)
        self.assertIn("sceKernelDcacheInvalidateRange", self.probe)
        self.assertIn("dmac_size_source_mutations", self.probe)
        self.assertIn('PROBE_HOST0_LOG "host0:/dmac_size_matrix_log.txt"', self.probe)
        self.assertIn('"size-matrix-%s-0x%08x"', self.probe)

    def test_model_profile_uses_the_user_bridge_and_raw_firmware_word(self) -> None:
        self.assertIn("PSP_ORACLE_CASE_MODEL_PROFILE", self.probe)
        self.assertIn("kuKernelGetModel()", self.probe)
        self.assertIn("sceKernelDevkitVersion()", self.probe)
        self.assertIn('PROBE_HOST0_LOG "host0:/model_profile_log.txt"', self.probe)
        self.assertIn("-lpspkubridge", self.makefile)

    def test_system_manifest_exposes_the_model_profile_case(self) -> None:
        manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        system = next(entry for entry in manifest["tests"] if entry["id"] == "PSP-SYSTEM-001")
        self.assertEqual(system["status"], "implemented")
        self.assertEqual(system["case_ids"], ["model-profile"])

    def test_manifest_routes_issue_23_to_dedicated_scalar_probe(self) -> None:
        manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"
            )
        )
        dmac = next(entry for entry in manifest["tests"] if entry["id"] == "PSP-DMAC-001")
        self.assertEqual(dmac["issues"], [23])
        self.assertEqual(len(dmac["case_ids"]), 23)
        self.assertIn("size-matrix-memcpy-0x0000bfff", dmac["case_ids"])
        self.assertIn("size-matrix-memcpy-0x0000c001", dmac["case_ids"])
        self.assertIn("missing record is never PASS", dmac["reset"])
        self.assertEqual(
            set(dmac["outcome_contract"]),
            {"result", "skip", "hang", "reset", "inconclusive"},
        )


if __name__ == "__main__":
    unittest.main()
