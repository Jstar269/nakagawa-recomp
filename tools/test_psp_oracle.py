# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
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
from import_fixtures import (
    INTERLEAVED_NIDS,
    INTERLEAVED_SHAPE,
    build_import_elf,
    build_interleaved_import_elf,
)
from psp_import_table import UNATTRIBUTED_LIBRARY
from psp_oracle import user_mode_imports
from psp_oracle.protocol import (
    ProtocolError,
    ge_corpus_report,
    PSP_MODEL_INTERPRETATION_RULE,
    compare_texts,
    decode_psp_model_code,
    model_identity_fields,
    parse_output,
    parse_progress,
    provenance_issues,
    validate_dmac_size_matrix,
    validate_dmac_size_matrix_size,
    DMAC_SIZE_MATRIX_SIZES,
    DMAC_SIZE_MATRIX_TRIALS,
)
from psp_oracle.run_psplink import (
    CAMPAIGN_QUEUE_CASES,
    PsplinkCampaignRunner,
    PsplinkSnapshot,
    _campaign_completeness_contract,
    _campaign_host0_log_path,
    _campaign_stream_complete,
    _parse_campaign_records,
    _record_summary,
    _split_command,
    _validate_host0_capture,
    annotate_terminal_outcome,
    evaluate_teardown_snapshots,
    main as run_psplink_main,
    parse_psplink_meminfo,
    parse_psplink_module_list,
    parse_psplink_module_threads,
    parse_psplink_thread_snapshot,
    parse_probe_completion_sentinel,
    _validate_campaign_contract,
)
from psp_oracle.parse_golden import (
    AUDIO_OUT_COUNTS,
    AUDIO_SPEC,
    CAMPAIGN_PROBE_CASES,
    DELAY_ZERO_OUT_COUNTS,
    DELAY_ZERO_SPEC,
    DMAC_CELL_OUT_COUNTS,
    DMAC_CELL_SPEC,
    DMAC_INVALID_CASES,
    GE_NAN_OUT_COUNTS,
    GE_NAN_SPEC,
    GE_NAN_WORDS,
    parse_audio_query_output,
    parse_campaign_probe_output,
    parse_delay_zero_output,
    parse_dmac_cells_output,
    parse_dmac_invalid_tail_output,
    parse_ge_nan_output,
    parse_registry_readonly_output,
    validate_hle_edram_restore,
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

    def test_model_identity_rule_separates_raw_family_and_agreement(self) -> None:
        fields = model_identity_fields("psp-3000-series", "0x03")
        self.assertEqual(fields["PHYSICAL_MODEL_LABEL"], "psp-3000-series")
        self.assertEqual(fields["SOFTWARE_MODEL_RAW_VALUE"], "0x03")
        self.assertEqual(fields["INTERPRETED_MODEL_FAMILY"], "PSP-3000")
        self.assertEqual(fields["MODEL_INTERPRETATION_RULE"], PSP_MODEL_INTERPRETATION_RULE)
        self.assertEqual(fields["MODEL_IDENTITY_AGREEMENT"], "AGREES")

    def test_unknown_model_rule_leaves_the_raw_value_untouched(self) -> None:
        stored_envelope = {
            "PHYSICAL_MODEL_LABEL": "PSP-3000",
            "SOFTWARE_MODEL_RAW_VALUE": "0x03",
        }
        fields = model_identity_fields(
            stored_envelope["PHYSICAL_MODEL_LABEL"],
            stored_envelope["SOFTWARE_MODEL_RAW_VALUE"],
            rule_id="PSPSDK_PMODEL_ORDINAL_V2",
        )
        self.assertEqual(fields["SOFTWARE_MODEL_RAW_VALUE"], "0x03")
        self.assertEqual(stored_envelope["SOFTWARE_MODEL_RAW_VALUE"], "0x03")
        self.assertEqual(fields["INTERPRETED_MODEL_FAMILY"], "UNKNOWN")
        self.assertEqual(fields["MODEL_IDENTITY_AGREEMENT"], "UNKNOWN")

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


class NewProbeResultParserTests(unittest.TestCase):
    def _row(self, test_id: str, case_id: str, count: int, *,
             status: str = "PASS", values: dict[int, int] | None = None) -> str:
        outputs = values or {}
        fields = " ".join(
            f"out{i}=0x{outputs.get(i, 0):08x}" for i in range(count)
        )
        return (
            f"NAKAGAWA_PSP_TEST schema=1 test_id={test_id} case_id={case_id} "
            f"status={status} result=0x00000000 {fields}\n"
        )

    def _stream(self, rows: list[str]) -> str:
        return META.format(
            source="psp", model="PSP-3000", firmware="6.61-ARK",
            binary=MEASURED_SHA, commit=MEASURED_COMMIT,
        ) + "".join(rows)

    def _invalid_tail_named_row(self, case_id: str, *, api: str,
                                endpoint: str, tier: str,
                                delta: int = 0, status: str = "PASS",
                                executed: int = 1, prefix: int = 0,
                                matches: int = 0, guards_outside: int = 0,
                                post_guard: int = 0, overflow_band: int = 0,
                                source_intact: int = 1,
                                payload_mutations: int = 0,
                                setup_mask: int = 0xF,
                                source_addr: int = 0x08811000,
                                destination_addr: int = 0x08822000) -> str:
        rc = 0
        fields = {
            "result": rc,
            "rc": rc,
            "P": prefix,
            "matches": matches,
            "guards_outside": guards_outside,
            "post_guard": post_guard,
            "overflow_band": overflow_band,
            "source_intact": source_intact,
            "setup_mask": setup_mask,
            "K": 0xC000,
            "delta": delta,
            "api": api,
            "endpoint": endpoint,
            "cache_discipline": 1 if executed else 0,
            "tier": tier,
            "executed": executed,
            "payload_mutations": payload_mutations,
            "source_addr": source_addr,
            "destination_addr": destination_addr,
        }
        encoded = " ".join(
            f"{name}=0x{value:08x}" if isinstance(value, int) else f"{name}={value}"
            for name, value in fields.items()
        )
        return (
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-DMAC-001 "
            f"case_id={case_id} status={status} {encoded}\n"
        )

    def test_invalid_tail_tier_s0_record_stream_is_strict(self) -> None:
        rows = []
        shapes = (("a", "dst"), ("b", "src"), ("c", "both"), ("d", "dst"))
        for api in ("memcpy", "try"):
            for cell, endpoint in shapes:
                rows.append(self._invalid_tail_named_row(
                    f"invalid-tail-s0-{cell}-{api}", api=api,
                    endpoint=endpoint, tier="S",
                    source_addr=0 if cell == "b" else 0x08811000,
                    destination_addr=(
                        0 if cell == "a" else 0xFFFFFFFF if cell == "d" else 0x08822000
                    ),
                ))
        complete = self._stream(rows)
        self.assertEqual(
            len(parse_dmac_invalid_tail_output(complete, "dma-invalid-tail-s0").results),
            8,
        )
        malformed = complete.replace(" P=0x00000000", " P=0x00000001", 1)
        with self.assertRaises(ProtocolError):
            parse_dmac_invalid_tail_output(malformed, "dma-invalid-tail-s0")

        def stream_with_pointer_shape(api: str, cell: str,
                                      overrides: dict[str, int]) -> str:
            changed_rows = list(rows)
            row_index = next(
                index for index, row in enumerate(changed_rows)
                if f"case_id=invalid-tail-s0-{cell}-{api}" in row
            )
            for field, value in overrides.items():
                row = changed_rows[row_index]
                old_token = next(
                    token for token in row.split() if token.startswith(f"{field}=")
                )
                changed_rows[row_index] = row.replace(
                    old_token, f"{field}=0x{value:08x}", 1
                )
            return self._stream(changed_rows)

        malformed_shapes = (
            ("a", {"source_addr": 0}),
            ("a", {"destination_addr": 0x08822000}),
            ("b", {"destination_addr": 0}),
            ("b", {"source_addr": 0x08811000}),
            ("c", {"source_addr": 0, "destination_addr": 0}),
            ("c", {"source_addr": 0}),
            ("c", {"destination_addr": 0}),
            ("d", {"source_addr": 0}),
            ("d", {"destination_addr": 0x08822000}),
        )
        for api in ("memcpy", "try"):
            for cell, pointer_overrides in malformed_shapes:
                with self.subTest(api=api, cell=cell,
                                  pointer_overrides=pointer_overrides):
                    malformed_shape = stream_with_pointer_shape(
                        api, cell, pointer_overrides
                    )
                    with self.assertRaises(ProtocolError):
                        parse_dmac_invalid_tail_output(
                            malformed_shape, "dma-invalid-tail-s0"
                        )
                    with self.assertRaises(ProtocolError):
                        _parse_campaign_records(
                            malformed_shape, "dma-invalid-tail-s0"
                        )

        oversized_address = stream_with_pointer_shape(
            "memcpy", "c", {"destination_addr": 0x100000000}
        )
        with self.assertRaises(ProtocolError):
            parse_dmac_invalid_tail_output(
                oversized_address, "dma-invalid-tail-s0"
            )

        negative_address = complete.replace(
            "source_addr=0x08811000", "source_addr=0x-000001", 1
        )
        with self.assertRaises(ProtocolError):
            parse_dmac_invalid_tail_output(
                negative_address, "dma-invalid-tail-s0"
            )

    def test_invalid_tail_tier_b_spans_fit_source_owned_scratch(self) -> None:
        probe = (Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" /
                 "probe.c").read_text(encoding="utf-8")
        flattened = re.sub(r"\\\r?\n\s*", " ", probe)
        constant_names = (
            "DMAC_INVALID_PRE_GUARD_BYTES",
            "DMAC_INVALID_PAYLOAD_BYTES",
            "DMAC_INVALID_POST_GUARD_BYTES",
            "DMAC_INVALID_OVERFLOW_BAND_BYTES",
            "DMAC_INVALID_TAIL_GUARD_BYTES",
            "DMAC_INVALID_MAX_DELTA",
        )
        constants = {
            name: int(re.search(
                rf"(?m)^#define\s+{name}\s+(0x[0-9a-fA-F]+|[0-9]+)[uU]?\s*$",
                probe,
            ).group(1), 0)
            for name in constant_names
        }

        def macro_terms(name: str) -> tuple[str, ...]:
            match = re.search(rf"(?m)^#define\s+{name}\s+\(([^)]*)\)", flattened)
            return tuple(re.findall(r"DMAC_INVALID_[A-Z_]+", match.group(1)))

        self.assertCountEqual(
            macro_terms("DMAC_INVALID_MAX_REQUEST"),
            ("DMAC_INVALID_PAYLOAD_BYTES", "DMAC_INVALID_MAX_DELTA"),
        )
        scratch_terms = macro_terms("DMAC_INVALID_SCRATCH_BYTES")
        self.assertCountEqual(scratch_terms, constant_names[:-1])
        max_request = sum(constants[name] for name in macro_terms(
            "DMAC_INVALID_MAX_REQUEST"
        ))
        scratch_bytes = sum(constants[name] for name in scratch_terms)

        self.assertIn(
            "#define DMAC_INVALID_PAYLOAD_OFFSET DMAC_INVALID_PRE_GUARD_BYTES",
            probe,
        )
        self.assertIn(
            "static uint8_t s_dmac_invalid_io[DMAC_INVALID_MAX_REQUEST]", probe
        )
        self.assertRegex(
            probe,
            r"\*block_uid\s*=\s*sceKernelAllocPartitionMemory\(\s*"
            r"DMAC_INVALID_PARTITION,\s*\"oracle-dmac-scratch\",\s*"
            r"PSP_SMEM_High,\s*DMAC_INVALID_SCRATCH_BYTES,\s*NULL\);",
        )
        self.assertIn(
            "*block_head = (uint8_t *)sceKernelGetBlockHeadAddr(*block_uid);",
            probe,
        )
        tier_b = probe.split("static void run_dmac_invalid_tier_b", 1)[1].split(
            "static void run_dmac_invalid_tail", 1
        )[0]
        self.assertIn(
            "uint8_t *const payload = block_head + DMAC_INVALID_PAYLOAD_OFFSET;",
            tier_b,
        )
        self.assertIn(
            "const uint32_t requested = DMAC_INVALID_PAYLOAD_BYTES + delta;",
            tier_b,
        )
        for endpoint in (
            "dst = payload;", "src = payload;",
            "src = s_dmac_invalid_io;", "dst = s_dmac_invalid_io;",
        ):
            self.assertIn(endpoint, tier_b)
        delta_match = re.search(
            r"static const uint32_t deltas\[\]\s*=\s*\{([^}]*)\};", tier_b
        )
        self.assertIsNotNone(delta_match, "missing Tier-B request deltas")
        deltas = tuple(
            int(value.strip().rstrip("uU"), 0)
            for value in delta_match.group(1).split(",") if value.strip()
        )
        self.assertCountEqual(deltas, (1, 4, 0x1000, 0x2000))
        payload_offset = constants["DMAC_INVALID_PRE_GUARD_BYTES"]
        for delta in deltas:
            requested = constants["DMAC_INVALID_PAYLOAD_BYTES"] + delta
            span_end = payload_offset + requested
            self.assertLessEqual(
                span_end,
                scratch_bytes,
                f"Tier-B delta {delta:#x} span ends at {span_end:#x}, "
                f"beyond the {scratch_bytes:#x}-byte owned scratch block",
            )
            self.assertLessEqual(delta, constants["DMAC_INVALID_MAX_DELTA"])
            self.assertLessEqual(requested, max_request)

    def test_audio_query_complete_and_malformed_scalar_fixtures(self) -> None:
        rows = []
        for case_id in AUDIO_SPEC.ordered_cases:
            count = AUDIO_OUT_COUNTS[case_id]
            values = {0: AUDIO_SPEC.terminal_count} if case_id == "audio-done" else None
            rows.append(self._row("PSP-AUDIO-001", case_id, count, values=values))
        complete = self._stream(rows)
        self.assertTrue(parse_audio_query_output(complete).complete)
        with self.assertRaises(ProtocolError):
            parse_audio_query_output(complete.replace(" out6=0x00000000", "", 1))
        with self.assertRaises(ProtocolError):
            parse_audio_query_output(self._stream(rows[:-1]))

    def test_ge_nan_complete_and_wrong_raw_bits_are_rejected(self) -> None:
        rows = []
        for case_id in GE_NAN_SPEC.ordered_cases:
            values = {0: GE_NAN_SPEC.terminal_count}
            if case_id != "ge-nan-done":
                sample = case_id.rsplit("-", 1)[1]
                values[0] = GE_NAN_WORDS[sample]
            rows.append(self._row(
                "PSP-GE-001", case_id, GE_NAN_OUT_COUNTS[case_id], values=values
            ))
        complete = self._stream(rows)
        self.assertTrue(parse_ge_nan_output(complete).complete)
        malformed = complete.replace("out0=0x7fc00000", "out0=0x7fc00001", 1)
        with self.assertRaises(ProtocolError):
            parse_ge_nan_output(malformed)

    def test_dma_owned_alignment_and_overlap_cells_validate_exact_addresses(self) -> None:
        rows = []
        for case_id in DMAC_CELL_SPEC.ordered_cases:
            if case_id == "dmac-cells-done":
                rows.append(self._row(
                    "PSP-DMAC-001", case_id, 1,
                    values={0: DMAC_CELL_SPEC.terminal_count},
                ))
                continue
            if case_id.startswith("align-"):
                _, api_name, side, offset_text = case_id.split("-")
                offset = int(offset_text, 16)
                src_offset = offset if side in {"src", "both"} else 0
                dst_offset = offset if side in {"dst", "both"} else 0
                values = {
                    0: 0 if api_name == "memcpy" else 1,
                    1: src_offset, 2: dst_offset, 3: 64,
                    4: 64, 5: 64, 6: 0, 7: 1,
                    8: 0x08801000 + src_offset,
                    9: 0x08802000 + dst_offset,
                }
            else:
                _, api_name, direction, delta_text = case_id.split("-")
                delta = int(delta_text, 16)
                source_offset = delta if direction == "backward" else 0
                destination_offset = delta if direction == "forward" else 0
                values = {
                    0: 0 if api_name == "memcpy" else 1,
                    1: 0 if direction == "forward" else 1,
                    2: delta, 3: 64, 4: 64, 5: 64, 6: 0, 7: 1,
                    8: 0x08801000 + source_offset,
                    9: 0x08801000 + destination_offset,
                }
            rows.append(self._row(
                "PSP-DMAC-001", case_id, DMAC_CELL_OUT_COUNTS[case_id],
                values=values,
            ))
        complete = self._stream(rows)
        self.assertTrue(parse_dmac_cells_output(complete).complete)
        malformed = complete.replace("out9=0x08802001", "out9=0x08802000", 1)
        with self.assertRaises(ProtocolError):
            parse_dmac_cells_output(malformed)

    def test_delay_zero_ready_equal_priority_fixture_and_malformed_pass(self) -> None:
        rows = []
        for case_id in DELAY_ZERO_SPEC.ordered_cases:
            if case_id == "delay-zero-done":
                rows.append(self._row(
                    "PSP-KERNEL-002", case_id, 1,
                    values={0: DELAY_ZERO_SPEC.terminal_count},
                ))
            else:
                rows.append(self._row(
                    "PSP-KERNEL-002", case_id, DELAY_ZERO_OUT_COUNTS[case_id],
                    values={0: 0, 1: 1, 2: 0, 3: 0, 4: 1, 5: 4,
                            6: 32, 7: 32, 8: 2, 9: 0},
                ))
        complete = self._stream(rows)
        self.assertTrue(parse_delay_zero_output(complete).complete)
        malformed = complete.replace("out6=0x00000020", "out6=0x00000021", 1)
        with self.assertRaises(ProtocolError):
            parse_delay_zero_output(malformed)

    def test_invalid_tail_tier_b_records_classify_bounded_overruns(self) -> None:
        deltas = (1, 4, 0x1000, 0x2000)
        for campaign_case, launch in DMAC_INVALID_CASES.items():
            if launch.tier != "B":
                continue
            rows = []
            for delta in deltas:
                requested = 0xC000 + delta
                if launch.endpoint == "dst":
                    post_guard = min(delta, 0x1000)
                    overflow_band = max(delta - 0x1000, 0)
                else:
                    post_guard = 0
                    overflow_band = 0
                rows.append(self._invalid_tail_named_row(
                    f"invalid-tail-{launch.cell}-delta-{delta:04x}",
                    api=launch.api or "", endpoint=launch.endpoint or "",
                    tier="B", delta=delta, prefix=requested, matches=requested,
                    post_guard=post_guard, overflow_band=overflow_band,
                ))
            complete = self._stream(rows)
            with self.subTest(campaign_case=campaign_case):
                self.assertEqual(
                    len(parse_dmac_invalid_tail_output(complete, campaign_case).results),
                    4,
                )
                outside_write = complete.replace(
                    "guards_outside=0x00000000",
                    "guards_outside=0x00000001",
                    1,
                )
                with self.assertRaises(ProtocolError):
                    parse_dmac_invalid_tail_output(outside_write, campaign_case)
                bad_classification = complete.replace(
                    "overflow_band=0x00000000",
                    "overflow_band=0x00000001",
                    1,
                )
                with self.assertRaises(ProtocolError):
                    parse_dmac_invalid_tail_output(bad_classification, campaign_case)
                payload_mutation = complete.replace(
                    "payload_mutations=0x00000000",
                    "payload_mutations=0x00000001",
                    1,
                )
                with self.assertRaises(ProtocolError):
                    parse_dmac_invalid_tail_output(payload_mutation, campaign_case)

    def test_invalid_tail_setup_skip_contains_no_transfer(self) -> None:
        campaign_case = "dma-invalid-tail-s0"
        rows = [
            self._invalid_tail_named_row(
                f"invalid-tail-s0-{cell}-{api}", api=api, endpoint=endpoint,
                tier="S", status="SKIP", executed=0, setup_mask=0x7,
                source_intact=0,
                source_addr=0, destination_addr=0,
            )
            for api in ("memcpy", "try")
            for cell, endpoint in (("a", "dst"), ("b", "src"),
                                   ("c", "both"), ("d", "dst"))
        ]
        complete = self._stream(rows)
        self.assertEqual(
            len(parse_dmac_invalid_tail_output(complete, campaign_case).results), 8
        )
        transferred = complete.replace(" executed=0x00000000", " executed=0x00000001", 1)
        with self.assertRaises(ProtocolError):
            parse_dmac_invalid_tail_output(transferred, campaign_case)
        claimed_intact = complete.replace(
            "source_intact=0x00000000", "source_intact=0x00000001", 1
        )
        with self.assertRaises(ProtocolError):
            parse_dmac_invalid_tail_output(claimed_intact, campaign_case)


    def test_campaign_probe_parsers_accept_complete_synthetic_records(self) -> None:
        for campaign_case, (spec, out_counts) in CAMPAIGN_PROBE_CASES.items():
            rows = [
                self._row(
                    spec.test_id, case_id, out_counts[case_id],
                    values={0: spec.terminal_count} if case_id == spec.terminal_case else None,
                )
                for case_id in spec.ordered_cases
            ]
            with self.subTest(campaign_case=campaign_case):
                parsed = parse_campaign_probe_output(self._stream(rows), campaign_case)
                self.assertTrue(parsed.complete)
                self.assertEqual(parsed.record_count, len(spec.ordered_cases))

    def test_campaign_probe_parser_rejects_incomplete_and_malformed_records(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES["kernel-alarm"]
        complete = self._stream([
            self._row(
                spec.test_id, case_id, counts[case_id],
                values={0: spec.terminal_count} if case_id == spec.terminal_case else None,
            )
            for case_id in spec.ordered_cases
        ])
        incomplete = "\n".join(complete.splitlines()[:-1]) + "\n"
        self.assertFalse(
            parse_campaign_probe_output(
                incomplete, "kernel-alarm", require_complete=False
            ).complete
        )
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(incomplete, "kernel-alarm")
        malformed = complete.replace("out0=0x00000000", "out9=0x00000000", 1)
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(malformed, "kernel-alarm")

    def test_registry_parser_never_accepts_unmodeled_key_values(self) -> None:
        rows = [
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-open status=PASS result=0x0 out0=0x1 out1=0x1\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-errors status=PASS result=0x0 out0=0x1 out1=0x2 "
            "out2=0x3 out3=0x4 out4=0x5 out5=0x6 out6=0x7\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-category-0000 status=PASS result=0x0 out0=0x2 out1=0x0 "
            "detail=CONFIG\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-key-0000 status=PASS result=0x0 out0=0x2 out1=0x4 "
            "out2=0x1 out3=0x0 detail=CONFIG/language value_hex=01000000\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-key-0001 status=PASS result=0x0 out0=0x3 out1=0x8 "
            "out2=0x0 out3=0xffffffff detail=CONFIG/nickname\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-bad-handle status=PASS result=0x80082715 "
            "out0=0x80082715 out1=0xffffffff\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-done status=PASS result=0x0 out0=0x1 out1=0x2 out2=0x6\n",
        ]
        report = parse_registry_readonly_output(self._stream(rows))
        self.assertTrue(report.complete)
        self.assertEqual(report.categories, ("CONFIG",))
        self.assertEqual(
            report.keys,
            ("CONFIG/language", "CONFIG/nickname"),
        )
        leaked = rows[4].replace("out2=0x0", "out2=0x1").replace(
            "detail=CONFIG/nickname", "detail=CONFIG/nickname "
            "value_hex=6e69636b6e616d65"
        )
        with self.assertRaises(ProtocolError):
            parse_registry_readonly_output(self._stream(rows[:4] + [leaked] + rows[5:]))

    def test_registry_bad_handle_record_is_last_before_done(self) -> None:
        rows = [
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-open status=PASS result=0x0 out0=0x1 out1=0x1\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-errors status=PASS result=0x0 out0=0x1 out1=0x2 "
            "out2=0x3 out3=0x0 out4=0x100 out5=0x100 out6=0x0\n",
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=walk-config\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-category-0000 status=PASS result=0x0 out0=0x0 out1=0x0 "
            "detail=CONFIG\n",
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=bad-handle\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-bad-handle status=PASS result=0x1 out0=0x1 out1=0x0\n",
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-REGISTRY-001 "
            "case_id=registry-done status=PASS result=0x0 out0=0x1 out1=0x0 out2=0x4\n",
        ]
        report = parse_registry_readonly_output(self._stream(rows))
        self.assertTrue(report.complete)
        self.assertEqual(report.parsed.last_step.step, "bad-handle")
        # A launch that hung in the forged-handle call keeps every earlier
        # measurement and names the step it stopped in.
        hung = parse_registry_readonly_output(self._stream(rows[:5]), require_complete=False)
        self.assertFalse(hung.complete)
        self.assertEqual(hung.parsed.last_step.step, "bad-handle")
        with self.assertRaises(ProtocolError):
            parse_registry_readonly_output(self._stream(rows[:5] + rows[6:]))
        moved = rows[:2] + rows[4:6] + rows[2:4] + rows[6:]
        with self.assertRaises(ProtocolError):
            parse_registry_readonly_output(self._stream(moved))


class GeCorpusGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.corpus = json.loads(
            (self.root / "fixtures" / "psp_oracle" / "ge_corpus.json").read_text(encoding="utf-8")
        )

    def _run_gate(self, document: dict | None = None,
                  results: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(self.root / "tools" / "psp_oracle" / "run_psplink.py"), "--ge-corpus-gate"]
        if document is None:
            return subprocess.run(command, capture_output=True, text=True, check=False)
        fixture_dir = self.root / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="ge-corpus-gate-", dir=fixture_dir) as scratch:
            results_dir = Path(scratch) / "results"
            results_dir.mkdir()
            for relative, text in (results or {}).items():
                target = results_dir / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8", newline="")
            command.extend(["--results-directory", str(results_dir)])
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

    def _schema(self) -> dict:
        return json.loads((self.root / "assets" / "ge_corpus.schema.json").read_text(encoding="utf-8"))

    def test_a_substituted_schema_is_refused(self) -> None:
        permissive = json.loads((self.root / "assets" / "public_source_profile.json").read_text(encoding="utf-8"))
        report = ge_corpus_report(self.corpus, permissive)
        self.assertEqual(report["status"], "REFUSED")
        self.assertIn("not the GE pixel corpus contract", report["cases"][0]["reason"])
        self.assertNotEqual(ge_corpus_report(self.corpus, self._schema())["status"], "REFUSED")

    def test_schema_keywords_the_validator_would_ignore_are_refused(self) -> None:
        schema = self._schema()
        schema["$defs"]["case"]["properties"]["framebuffer"]["properties"]["width"]["minium"] = 1
        report = ge_corpus_report(self.corpus, schema)
        self.assertEqual(report["status"], "REFUSED")
        self.assertIn("minium", report["cases"][0]["reason"])

    def test_packed_pixel_values_must_fit_the_declared_format(self) -> None:
        document = json.loads(json.dumps(self.corpus))
        case = document["cases"][0]
        case["framebuffer"]["format"] = "5650"
        case["framebuffer"]["initial_pixel"] = 0xFF00FF00
        case["selected_pixels"][0]["pixel_value"] = 0xFF00FF00
        result = ge_corpus_report(document, self._schema())["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("does not fit the 5650 pixel format", result["reason"])
        case["framebuffer"]["initial_pixel"] = 0
        case["selected_pixels"][0]["pixel_value"] = 0xFFFF
        case["selected_pixels"][1]["pixel_value"] = 0
        case["selected_pixels"][2]["pixel_value"] = 0
        self.assertNotIn("does not fit", ge_corpus_report(document, self._schema())["cases"][0]["reason"])

    def test_prim_vertex_count_must_agree_with_the_vertex_words(self) -> None:
        document = json.loads(json.dumps(self.corpus))
        case = document["cases"][0]
        case["vertex_words"] = case["vertex_words"][:-1]
        result = ge_corpus_report(document, self._schema())["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("PRIM vertex count 3 does not divide", result["reason"])

    def test_successive_prims_consume_vertex_words_cumulatively(self) -> None:
        document = json.loads(json.dumps(self.corpus))
        case = document["cases"][0]
        commands = case["command_words"]
        prim = next(i for i, word in enumerate(commands) if word[2:4] == "04")
        count = int(commands[prim], 16) & 0xFFFF
        per_vertex = len(case["vertex_words"]) // count
        quad = f"0x{(int(commands[prim], 16) & 0xFFFF0000) | 4:08x}"
        commands.insert(prim + 1, quad)
        for relocation in case.get("relocations", []):
            if relocation["command_index"] > prim:
                relocation["command_index"] += 1
        case["vertex_words"] = case["vertex_words"] + case["vertex_words"][:per_vertex] * 4
        result = ge_corpus_report(document, self._schema())["cases"][0]
        self.assertNotEqual(result["status"], "REFUSED", result["reason"])
        case["vertex_words"] = case["vertex_words"][:-1]
        result = ge_corpus_report(document, self._schema())["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn(f"PRIM vertex counts {count}, 4 (total {count + 4})", result["reason"])

    def test_the_public_fixture_carries_no_hardware_envelope(self) -> None:
        """Raw hardware text must not enter this synthetic-classified public fixture.

        A measured envelope inlines the console model, firmware, binary digest and
        source commit; admitting one is a maintainer provenance decision, so the
        committed corpus stays envelope-free until that decision is made.
        """
        for case in self.corpus["cases"]:
            self.assertIsNone(case["evidence_envelope"])
            self.assertIsNone(case["source_tier"])

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
            "RESULT_RECORD": {
                "path": "ge/software.txt",
                "sha256": hashlib.sha256(raw_result.encode("utf-8")).hexdigest(),
            },
        }
        completed = self._run_gate(document, {"ge/software.txt": raw_result})
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
            "RESULT_RECORD": {"path": "ge/psp.txt", "sha256": "0" * 64},
        }
        completed = self._run_gate(document)
        self.assertEqual(completed.returncode, 2, completed.stderr)
        result = self._report(completed)["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("not acceptance-eligible", result["reason"])

    def _hardware_case(self, raw_result: str, record_sha: str | None = None) -> dict:
        document = json.loads(json.dumps(self.corpus))
        case = document["cases"][0]
        case["source_tier"] = "PSP_HARDWARE"
        case["framebuffer_sha256"] = "a" * 64
        for pixel in case["selected_pixels"]:
            pixel["pixel_value"] = 0
        case["evidence_envelope"] = {
            "EVIDENCE_CLASS": "PSP_HARDWARE",
            "ACCEPTANCE_ELIGIBLE": True,
            "CASE_ID": case["case_id"],
            "CONSOLE_MODEL": "PSP-3000",
            "FW": "6.61-ARK",
            "SOURCE_COMMIT": MEASURED_COMMIT,
            "BINARY_SHA256": MEASURED_SHA,
            "RESULT_RECORD": {
                "path": "ge/run.txt",
                "sha256": record_sha or hashlib.sha256(raw_result.encode("utf-8")).hexdigest(),
            },
        }
        return document

    def test_hardware_claim_without_a_results_directory_is_not_run(self) -> None:
        document = self._hardware_case(stream("psp"))
        result = ge_corpus_report(document, self._schema(), None)["cases"][0]
        self.assertEqual(result["status"], "NOT_RUN")
        self.assertIn("private result record", result["reason"])

    def test_result_record_digest_must_match_the_stored_result(self) -> None:
        document = self._hardware_case(stream("psp"), record_sha="b" * 64)
        completed = self._run_gate(document, {"ge/run.txt": stream("psp")})
        self.assertEqual(completed.returncode, 2, completed.stderr)
        result = self._report(completed)["cases"][0]
        self.assertEqual(result["status"], "REFUSED")
        self.assertIn("sha256 does not match", result["reason"])

    def test_missing_result_record_is_refused(self) -> None:
        completed = self._run_gate(self._hardware_case(stream("psp")), {})
        self.assertEqual(completed.returncode, 2, completed.stderr)
        self.assertIn("not present", self._report(completed)["cases"][0]["reason"])

    def test_schema_requires_a_record_reference_not_inline_raw_text(self) -> None:
        envelope = self._schema()["$defs"]["case"]["properties"]["evidence_envelope"]
        self.assertIn("RESULT_RECORD", envelope["required"])
        self.assertNotIn("RAW_RESULT", envelope["properties"])
        document = self._hardware_case(stream("psp"))
        document["cases"][0]["evidence_envelope"]["RESULT_RECORD"]["path"] = "../escape.txt"
        result = ge_corpus_report(document, self._schema(), self.root)["cases"][0]
        self.assertEqual(result["status"], "REFUSED")

    def test_result_record_loader_refuses_escape_and_oversize(self) -> None:
        from psp_oracle.protocol import GE_RESULT_RECORD_MAX_BYTES, _load_result_record
        with tempfile.TemporaryDirectory() as scratch:
            base = Path(scratch) / "results"
            base.mkdir()
            outside = Path(scratch) / "outside.txt"
            outside.write_text("x", encoding="utf-8")
            text, reason = _load_result_record({"path": "../outside.txt", "sha256": "0" * 64}, base)
            self.assertIsNone(text)
            self.assertIn("leaves the results directory", reason)
            big = base / "big.txt"
            big.write_bytes(b"a" * (GE_RESULT_RECORD_MAX_BYTES + 1))
            text, reason = _load_result_record({"path": "big.txt", "sha256": "0" * 64}, base)
            self.assertIsNone(text)
            self.assertIn("at most", reason)

    def test_non_object_identity_entries_are_refused_not_raised(self) -> None:
        for key, value in (("semantic_boundary", "GE_RASTER_PIXEL_CONFORMANCE"), ("tracking_issue", 343)):
            with self.subTest(key=key):
                schema = self._schema()
                schema["properties"][key] = value
                report = ge_corpus_report(self.corpus, schema)
                self.assertEqual(report["status"], "REFUSED")
                self.assertIn("not the GE pixel corpus contract", report["cases"][0]["reason"])

    def test_unimplemented_keyword_forms_are_refused(self) -> None:
        schema = self._schema()
        schema["$defs"]["case"]["additionalProperties"] = {"type": "string"}
        self.assertIn("boolean form", ge_corpus_report(self.corpus, schema)["cases"][0]["reason"])
        schema = self._schema()
        schema["$defs"]["case"]["properties"]["vertex_words"]["items"] = [{"type": "string"}]
        self.assertIn("single-schema object form", ge_corpus_report(self.corpus, schema)["cases"][0]["reason"])

    def test_cli_accepts_only_the_tracked_schema_file(self) -> None:
        sibling = self.root / "assets" / "public_source_profile.json"
        completed = subprocess.run(
            [sys.executable, str(self.root / "tools" / "psp_oracle" / "run_psplink.py"),
             "--ge-corpus-gate", "--ge-corpus-schema", str(sibling)],
            capture_output=True, text=True, check=False)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("must be assets/ge_corpus.schema.json", completed.stderr)

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

    def test_hang_annotation_names_the_last_complete_step_of_a_cut_capture(self) -> None:
        base = {"mode": "capture", "process_status": "TIMEOUT", "stdout_file": "capture.txt"}
        capture = (
            "Load/Start UID: 0x1\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=walk-config\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=bad-handle\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=registry-readonly step=bad-ha"
        )
        hang = annotate_terminal_outcome(base, capture.encode(), "HANG")
        self.assertEqual(
            hang["last_probe_step"], {"case_id": "registry-readonly", "step": "bad-handle"}
        )
        self.assertIsNone(
            annotate_terminal_outcome(base, b"transport only\n", "HANG")["last_probe_step"]
        )

    def test_parse_progress_collects_complete_steps_from_a_cut_stream(self) -> None:
        self.assertIsNone(parse_progress("").last_step)
        self.assertIsNone(parse_progress("transport only\n").last_step)
        progress = parse_progress(
            "console noise\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=x step=a\n"
            "NAKAGAWA_PSP_TEST schema=1 test_id=T case_id=x status=PASS\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=x step=b\n"
            "NAKAGAWA_PSP_STEP schema=1 case_id=x step=c"
        )
        # The final line has no newline, so it may stop mid-marker and is not counted.
        self.assertEqual(progress.last_step.step, "b")
        self.assertEqual(progress.results, ())
        self.assertEqual(progress.metadata, ())
        with self.assertRaises(ProtocolError):
            parse_progress("NAKAGAWA_PSP_STEP schema=1 case_id=x step=has space\n")

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
            ), patch(
                "psp_oracle.run_psplink._read_hardware_lock",
                return_value=(True, "HELD_AND_CONFIRMED"),
            ):
                self.assertEqual(
                    run_psplink_main(
                        [
                            "--command", "fake-pspsh",
                            "--session-id", "synthetic-session",
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

    def test_model_code_is_not_used_as_an_operator_declared_physical_label(self) -> None:
        command = [
            sys.executable,
            str(Path(__file__).resolve().parent / "psp_oracle" / "run_psplink.py"),
            "--dry-run",
            "--model-code",
            "3",
        ]
        # A raw code alone does not create an operator-declared physical label.
        completed = subprocess.run(
            command, capture_output=True, text=True, check=True
        )
        plan = json.loads(completed.stdout)
        self.assertFalse(plan["provenance_supplied"])
        self.assertIsNone(plan["model"])
        self.assertEqual(plan["model_code"], 3)

        with_model = subprocess.run(
            command + [
                "--model", "psp-3000-series",
                "--binary", "probe.prx",
                "--source-commit", "a" * 40,
                "--firmware", "6.61",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        paired_plan = json.loads(with_model.stdout)
        self.assertEqual(paired_plan["model"], "psp-3000-series")
        self.assertEqual(paired_plan["model_code"], 3)

    def test_terminal_annotation_preserves_legacy_model_envelope_fields(self) -> None:
        legacy = {
            "CONSOLE_MODEL": "PSP-3000-04g",
            "MODEL_SOURCE": "operator-recorded label; no serial or MAC stored",
            "PHYSICAL_MODEL_LABEL": "psp-3000-series",
            "SOFTWARE_MODEL_RAW_VALUE": "3",
            "MODEL_CONTRADICTION": "RECORDED",
        }
        annotated = annotate_terminal_outcome(legacy, b"transport only\n", "RESET")
        self.assertEqual(
            {key: annotated[key] for key in legacy},
            legacy,
        )
        self.assertEqual(legacy["MODEL_CONTRADICTION"], "RECORDED")

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


class PspLinkTeardownSnapshotTests(unittest.TestCase):
    THREADS = (
        "<Thread List (2 entries)>\n"
        "UID: 0x00000001 - Name: PspLink\n"
        "UID: 0x00000002 - Name: USBThread\n"
    )
    MEMORY = (
        "Memory Partitions:\n"
        "N  |    BASE    |   SIZE   | TOTALFREE |  MAXFREE  | ATTR |\n"
        "---|------------|----------|-----------|-----------|------|\n"
        "1  | 0x08800000 | 33554432 |  20971520 |  16777216 | 000F |\n"
        "2  | 0x88000000 | 53687091 |  33554432 |  25165824 | 000F |\n"
    )
    MODULES = (
        "<Module List (1 modules)>\n"
        "UID: 0x00000003 Attr: 0000 - Name: PspLink\n"
    )

    def snapshot(self, *, threads=None, memory=None, modules=None) -> PsplinkSnapshot:
        return PsplinkSnapshot(
            threads=frozenset(threads if threads is not None else {
                ("0x00000001", "PspLink"), ("0x00000002", "USBThread")
            }),
            memory_free_bytes=tuple(sorted((memory if memory is not None else {
                1: (20971520, 16777216), 2: (33554432, 25165824)
            }).items())),
            modules=frozenset(modules if modules is not None else {
                ("0x00000003", "PspLink")
            }),
        )

    def clean_triplet(self):
        before = self.snapshot()
        module_main = ("0x00000009", "user_main")
        after_probe = self.snapshot(
            threads=set(before.threads) | {module_main},
            modules=set(before.modules) | {("0x00000008", "NAKAGAWA_PSP_ORACLE")},
        )
        return before, after_probe, before, module_main

    def test_campaign_stream_gate_excludes_only_one_final_teardown_marker(self) -> None:
        stream = (
            "NAKAGAWA_PSP_META schema=1 source=psp model=fixture firmware=test "
            + "binary_sha256=" + "0" * 64 + " source_commit=" + "1" * 40 + "\n"
            "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-SYSTEM-001 case_id=model-profile "
            "status=PASS result=0x3 out0=0x3 out1=0x06060110 out2=0xde\n"
        )
        passing = stream + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
        failing = stream + "NAKAGAWA_PSP_COMPLETE schema=1 status=FAIL\n"
        skipped = stream + "NAKAGAWA_PSP_COMPLETE schema=1 status=NOT_RUN\n"

        self.assertTrue(_campaign_stream_complete(passing, "model-profile"))
        self.assertTrue(_campaign_stream_complete(failing, "model-profile"))
        self.assertTrue(_campaign_stream_complete(skipped, "model-profile"))
        self.assertEqual(parse_probe_completion_sentinel(passing), "PASS")
        self.assertEqual(parse_probe_completion_sentinel(failing), "FAIL")
        self.assertEqual(parse_probe_completion_sentinel(skipped), "NOT_RUN")
        self.assertIsNone(parse_probe_completion_sentinel(
            passing + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
        ))
        self.assertFalse(
            _campaign_stream_complete(
                passing + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n",
                "model-profile",
            )
        )
        self.assertFalse(
            _campaign_stream_complete(
                passing + "unexpected trailing output\n", "model-profile"
            )
        )

    def test_thlist_parser_requires_complete_uid_and_name_set(self) -> None:
        self.assertEqual(parse_psplink_thread_snapshot(self.THREADS), {
            ("0x00000001", "PspLink"), ("0x00000002", "USBThread")
        })
        with self.assertRaises(ValueError):
            parse_psplink_thread_snapshot(
                "<Thread List (2 entries)>\nUID: 0x1 - Name: PspLink\n"
            )

    def test_meminfo_parser_compares_total_and_largest_free_bytes_per_partition(self) -> None:
        self.assertEqual(parse_psplink_meminfo(self.MEMORY), {
            1: (20971520, 16777216), 2: (33554432, 25165824)
        })
        with self.assertRaises(ValueError):
            parse_psplink_meminfo(
                "Memory Partitions:\nN | BASE | SIZE | TOTALFREE | MAXFREE | ATTR\n"
            )

    def test_modlist_and_module_thread_parsers_return_uid_name_sets(self) -> None:
        self.assertEqual(parse_psplink_module_list(self.MODULES), {
            ("0x00000003", "PspLink")
        })
        output = (
            "UID: 0x00000008 Attr: 0000 - Name: NAKAGAWA_PSP_ORACLE\n"
            "Module Thread (1)\nUID: 0x00000009 - Name: user_main\n"
        )
        self.assertEqual(parse_psplink_module_threads(output, "0x00000008"), {
            ("0x00000009", "user_main")
        })

    def test_snapshot_uses_psplink_module_inventory_command(self) -> None:
        runner = PsplinkCampaignRunner(
            SimpleNamespace(), console_model="synthetic", source_commit="0" * 40
        )
        outputs = [
            (0, self.THREADS, "", "PROCESS_EXITED"),
            (0, self.MEMORY, "", "PROCESS_EXITED"),
            (0, self.MODULES, "", "PROCESS_EXITED"),
        ]
        with patch.object(runner, "_request", side_effect=outputs) as request:
            snapshot, problem = runner._take_snapshot()
        self.assertIsNone(problem)
        self.assertIsNotNone(snapshot)
        self.assertEqual(
            [entry.args[0] for entry in request.call_args_list],
            ["thlist", "meminfo", "modlist"],
        )

    def test_three_snapshot_check_passes_clean_unload(self) -> None:
        before, after_probe, after_unload, module_main = self.clean_triplet()
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True,
            host0_roundtrip=True,
        )
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["issues"], [])

    def test_unqualified_exprint_response_is_diagnostic_not_a_teardown_gate(self) -> None:
        before, after_probe, after_unload, module_main = self.clean_triplet()
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True, host0_roundtrip=True,
            exprint_command_status="PROCESS_EXITED",
        )
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["exprint_status"], "NOT_RUN")
        self.assertEqual(report["exprint_command_status"], "PROCESS_EXITED")

    def test_three_snapshot_check_reports_allocator_drift_without_failing_teardown(self) -> None:
        before, after_probe, _after_unload, module_main = self.clean_triplet()
        after_probe = self.snapshot(
            threads=after_probe.threads,
            memory={1: (20967424, 16773120), 2: (33550336, 25161216)},
            modules=after_probe.modules,
        )
        after_unload = self.snapshot(
            memory={1: (20970496, 16770000), 2: (33550336, 25161728)},
        )

        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True,
            host0_roundtrip=True,
        )

        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["issues"], [])
        self.assertTrue(report["memory_free_deltas_diagnostic_only"])
        self.assertEqual(report["memory_free_deltas_bytes"], {
            1: {
                "s1": {"total_free": -4096, "largest_block": -4096},
                "s2": {"total_free": -1024, "largest_block": -7216},
            },
            2: {
                "s1": {"total_free": -4096, "largest_block": -4608},
                "s2": {"total_free": -4096, "largest_block": -4096},
            },
        })

    def test_three_snapshot_check_fails_on_thread_and_module_residue(self) -> None:
        before, after_probe, _after_unload, module_main = self.clean_triplet()
        after_probe = self.snapshot(
            threads=set(after_probe.threads) | {("0x0000000a", "oracle-thread")},
            modules=set(after_probe.modules) | {("0x00000008", "NAKAGAWA_PSP_ORACLE")},
        )
        after_unload = self.snapshot(
            memory={1: (20971520, 16777216), 2: (33550336, 25161728)},
            modules=set(before.modules) | {("0x00000008", "NAKAGAWA_PSP_ORACLE")},
        )
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True,
            host0_roundtrip=True,
        )
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any("child thread" in issue for issue in report["issues"]))
        self.assertTrue(any("module set" in issue for issue in report["issues"]))
        self.assertNotIn(
            "post-unload per-partition free memory differs from S0", report["issues"]
        )

    def test_post_unload_survival_of_the_probe_main_thread_is_named(self) -> None:
        before, after_probe, _after_unload, module_main = self.clean_triplet()
        after_unload = self.snapshot(threads=set(before.threads) | {module_main})
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True,
            host0_roundtrip=True,
        )
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(report["recovery_eligible"])
        self.assertIn("post-unload thread set differs from S0", report["issues"])
        self.assertIn(
            "probe main thread survived module stop/unload; "
            "the probe's module_stop did not end and delete it",
            report["issues"],
        )
        self.assertEqual(report["s2_leftover_threads"], [list(module_main)])
        self.assertEqual(report["s2_missing_threads"], [])

    def test_unrelated_post_unload_thread_is_not_blamed_on_module_stop(self) -> None:
        before, after_probe, _after_unload, module_main = self.clean_triplet()
        stranger = ("0x0000007f", "SceUnrelatedThread")
        after_unload = self.snapshot(threads=set(before.threads) | {stranger})
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True,
            host0_roundtrip=True,
        )
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("post-unload thread set differs from S0", report["issues"])
        self.assertFalse(any("module_stop" in issue for issue in report["issues"]))
        self.assertEqual(report["s2_leftover_threads"], [list(stranger)])

    def test_three_snapshot_check_fails_when_modstun_handshake_is_unconfirmed(self) -> None:
        before, after_probe, after_unload, module_main = self.clean_triplet()
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=False, sentinel_status="PASS",
            shell_qualified=True,
            host0_roundtrip=True,
        )
        self.assertEqual(report["status"], "FAIL")
        self.assertTrue(any("modstun" in issue for issue in report["issues"]))

    def test_teardown_failure_enters_the_bounded_recovery_ladder(self) -> None:
        runner = PsplinkCampaignRunner(
            SimpleNamespace(), console_model="synthetic", source_commit="0" * 40
        )
        report = {
            "status": "FAIL", "issues": ["thread set changed"],
            "recovery_eligible": True,
        }
        with patch.object(runner, "_recover", return_value=True) as recover:
            self.assertTrue(runner._enforce_teardown_check(
                report, module_uid="0x00000008", probe_succeeded=True
            ))
        recover.assert_called_once_with(
            "0x00000008", "probe teardown check failed: thread set changed"
        )

    def test_three_snapshot_check_keeps_missing_snapshots_unavailable(self) -> None:
        before, _after_probe, _after_unload, module_main = self.clean_triplet()
        report = evaluate_teardown_snapshots(
            before, None, None, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True, host0_roundtrip=True,
        )
        self.assertEqual(report["status"], "BLOCKED")
        self.assertIn("S1 snapshot unavailable", report["issues"])
        self.assertIn("S2 snapshot unavailable", report["issues"])
        self.assertFalse(report["recovery_eligible"])
        self.assertIsNone(report["s1_thread_count"])
        self.assertIsNone(report["s2_thread_count"])
        self.assertIsNone(report["s2_module_count"])
        for deltas in report["memory_free_deltas_bytes"].values():
            self.assertIsNone(deltas["s1"])
            self.assertIsNone(deltas["s2"])

    def test_module_thread_query_failure_blocks_without_constructing_a_set(self) -> None:
        before, after_probe, after_unload, _module_main = self.clean_triplet()
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", None,
            unload_confirmed=True, sentinel_status="PASS",
            shell_qualified=True, host0_roundtrip=True,
            module_threads_available=False,
        )
        self.assertEqual(report["status"], "BLOCKED")
        self.assertIn("module thread query unavailable", report["issues"])
        self.assertFalse(report["recovery_eligible"])

    def test_blocked_or_unsuccessful_probe_does_not_enter_recovery(self) -> None:
        runner = PsplinkCampaignRunner(
            SimpleNamespace(), console_model="synthetic", source_commit="0" * 40
        )
        cases = (
            ({"status": "BLOCKED", "issues": ["S2 snapshot unavailable"]}, True),
            ({
                "status": "FAIL", "issues": ["confirmed unload failure"],
                "recovery_eligible": True,
            }, False),
        )
        with patch.object(runner, "_recover", return_value=True) as recover:
            for report, probe_succeeded in cases:
                self.assertFalse(runner._enforce_teardown_check(
                    report, module_uid="0x00000008",
                    probe_succeeded=probe_succeeded,
                ))
                self.assertEqual(report["recovery_status"], "NOT_RUN")
        recover.assert_not_called()

    def test_not_run_completion_marker_blocks_teardown_without_claiming_pass(self) -> None:
        before, after_probe, after_unload, module_main = self.clean_triplet()
        report = evaluate_teardown_snapshots(
            before, after_probe, after_unload, "0x00000008", {module_main},
            unload_confirmed=True, sentinel_status="NOT_RUN",
            shell_qualified=True, host0_roundtrip=None,
        )
        self.assertEqual(report["status"], "BLOCKED")
        self.assertIn("probe completion sentinel reports NOT_RUN", report["issues"])
        self.assertFalse(report["recovery_eligible"])

    def test_emulated_probe_teardown_marks_skipped_host0_stage_not_run(self) -> None:
        probe = (Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" /
                 "probe.c").read_text(encoding="utf-8")
        host0 = probe.split("static int probe_teardown_host0(", 1)[1].split("\n}", 1)[0]
        teardown = probe.split("static void probe_teardown(", 1)[1].split("\n}", 1)[0]
        self.assertRegex(host0, r"if\s*\(emulated\)\s*return\s+PROBE_TEARDOWN_SKIPPED;")
        self.assertRegex(
            teardown,
            r'!success\s*\?\s*"FAIL"\s*:\s*host0_result\s*==\s*'
            r'PROBE_TEARDOWN_SKIPPED\s*\?\s*"NOT_RUN"\s*:\s*"PASS"',
        )

    def test_interrupt_resume_failure_and_child_termination_reach_teardown_verdict(self) -> None:
        root = Path(__file__).resolve().parents[1]
        probe = (root / "fixtures" / "psp_oracle" /
                 "probe.c").read_text(encoding="utf-8")
        resume = probe.split("int probe_resume_intr(", 1)[1].split("\n}", 1)[0]
        self.assertIn("sceKernelCpuResumeIntr(token)", resume)
        self.assertIn("probe_untrack_intr_token(token)", resume)
        self.assertIn("return -1", resume)
        self.assertIn("s_teardown_tracking_failed = 1", resume)

        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is unavailable for the interrupt-resume harness")
        untrack_start = probe.index("static int probe_untrack_intr_token(int token) {")
        untrack_end = probe.index("\n}\n", untrack_start) + 2
        untrack = probe[untrack_start:untrack_end]
        resume_start = probe.index("int probe_resume_intr(int token) {")
        resume_end = probe.index("\n}\n", resume_start) + 2
        resume_function = probe[resume_start:resume_end]
        harness = r'''#include <stddef.h>
#include <string.h>

#define PROBE_TEARDOWN_CAPACITY 4u
static int s_teardown_tracking_failed;
static int s_pending_intr_tokens[PROBE_TEARDOWN_CAPACITY];
static int s_pending_intr_active[PROBE_TEARDOWN_CAPACITY];
static int s_live_interrupts_enabled;
static int s_resume_calls;

void sceKernelCpuResumeIntr(int token) {
    (void)token;
    ++s_resume_calls;
}

/* PSP contract used by the fixture: token 0 means the saved state was
   suspended, while a nonzero token means the saved state was enabled. */
int sceKernelIsCpuIntrSuspended(unsigned int token) {
    return token == 0u;
}

int sceKernelIsCpuIntrEnable(void) {
    return s_live_interrupts_enabled;
}

'''+untrack+"\n"+resume_function+r'''

static void setup(int token, int tracked, int live_enabled) {
    memset(s_pending_intr_tokens, 0, sizeof(s_pending_intr_tokens));
    memset(s_pending_intr_active, 0, sizeof(s_pending_intr_active));
    s_pending_intr_tokens[0] = token;
    s_pending_intr_active[0] = tracked;
    s_teardown_tracking_failed = 0;
    s_live_interrupts_enabled = live_enabled;
    s_resume_calls = 0;
}

int main(void) {
    setup(0, 1, 0);
    if (probe_resume_intr(0) != 0 || s_pending_intr_active[0] ||
        s_teardown_tracking_failed || s_resume_calls != 1) return 1;

    setup(1, 1, 1);
    if (probe_resume_intr(1) != 0 || s_pending_intr_active[0] ||
        s_teardown_tracking_failed || s_resume_calls != 1) return 2;

    /* A live post-resume mismatch is a teardown failure and retains the
       token so the caller cannot silently lose the pending cleanup. */
    setup(0, 1, 1);
    if (probe_resume_intr(0) >= 0 || !s_pending_intr_active[0] ||
        !s_teardown_tracking_failed) return 3;

    /* A matching live state still fails when the token was never tracked. */
    setup(1, 0, 1);
    if (probe_resume_intr(1) >= 0 || s_pending_intr_active[0] ||
        !s_teardown_tracking_failed) return 4;
    return 0;
}
'''
        fixture = root / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="interrupt-resume-host-", dir=fixture) as scratch:
            source_path = Path(scratch) / "interrupt_resume_host.c"
            binary_path = Path(scratch) / "interrupt_resume_host.exe"
            source_path.write_text(harness, encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source_path), "-o", str(binary_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            executed = subprocess.run(
                [str(binary_path)], capture_output=True, text=True, check=False
            )
            self.assertEqual(executed.returncode, 0, executed.stderr)

        teardown_state = probe.split("static int probe_teardown_state(", 1)[1].split("\n}", 1)[0]
        self.assertRegex(
            teardown_state,
            r"if\s*\(probe_resume_intr\(s_pending_intr_tokens\[i\]\)\s*<\s*0\)\s*"
            r"success\s*=\s*0\s*;",
        )
        children = probe.split("static int probe_teardown_children(", 1)[1].split("\n}", 1)[0]
        self.assertIn("probe_terminate_delete_thread(s_owned_threads[i].uid)", children)
        self.assertNotIn("sceKernelTerminateThread", children)
        self.assertNotIn("probe_delete_thread(s_owned_threads[i].uid)", children)

    def test_child_teardown_uses_atomic_terminate_delete_and_keeps_failures_tracked(self) -> None:
        """Compile and execute the teardown helper against failing PSP calls."""
        root = Path(__file__).resolve().parents[1]
        compiler = shutil.which("gcc")
        if compiler is None:
            self.skipTest("gcc is unavailable for the child teardown harness")

        probe = (root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        helper_start = probe.index(
            "int probe_terminate_delete_thread(SceUID uid) {"
        )
        helper_end = probe.index("\n}\n", helper_start) + 2
        helper = probe[helper_start:helper_end]
        children_start = probe.index("static int probe_teardown_children(void) {")
        children_end = probe.index("\n}\n", children_start) + 2
        children = probe[children_start:children_end]
        harness = r'''#include <stddef.h>
#include <string.h>

#define PROBE_TEARDOWN_CAPACITY 2u
#define PSP_THREAD_RUNNING 1u
#define PSP_THREAD_READY 2u
#define PSP_THREAD_WAITING 4u
#define PSP_THREAD_SUSPEND 8u
typedef int SceUID;
typedef struct {
    int size;
    unsigned int status;
} SceKernelThreadInfo;
struct probe_owned_uid {
    SceUID uid;
    int active;
};
static struct probe_owned_uid s_owned_threads[PROBE_TEARDOWN_CAPACITY];
static int atomic_rc;
static int refer_present;
static int refer_calls;
static int atomic_calls;
static int separate_terminate_calls;
static int separate_delete_calls;

static void reset_case(int rc, int present) {
    memset(s_owned_threads, 0, sizeof(s_owned_threads));
    s_owned_threads[0].uid = 7;
    s_owned_threads[0].active = 1;
    atomic_rc = rc;
    refer_present = present;
    refer_calls = 0;
    atomic_calls = 0;
    separate_terminate_calls = 0;
    separate_delete_calls = 0;
}

static void probe_untrack_uid(struct probe_owned_uid *entries, SceUID uid) {
    for (size_t i = 0; i < PROBE_TEARDOWN_CAPACITY; ++i) {
        if (entries[i].active && entries[i].uid == uid) entries[i].active = 0;
    }
}

int sceKernelTerminateDeleteThread(SceUID uid) {
    (void)uid;
    ++atomic_calls;
    return atomic_rc;
}

int sceKernelTerminateThread(SceUID uid) {
    (void)uid;
    ++separate_terminate_calls;
    return -1;
}

int probe_delete_thread(SceUID uid) {
    (void)uid;
    ++separate_delete_calls;
    return -1;
}

int sceKernelReferThreadStatus(SceUID uid, SceKernelThreadInfo *info) {
    (void)uid;
    if (!refer_present || (refer_present == 2 && refer_calls++ > 0)) return -1;
    ++refer_calls;
    info->status = PSP_THREAD_WAITING;
    return 0;
}

'''.replace("\n\n", "\n") + helper + "\n" + children + r'''

int main(void) {
    reset_case(-1, 1);
    if (probe_teardown_children() || !s_owned_threads[0].active ||
        atomic_calls != 1 || separate_terminate_calls != 0 ||
        separate_delete_calls != 0) return 1;

    reset_case(0, 1);
    if (probe_teardown_children() || !s_owned_threads[0].active ||
        atomic_calls != 1 || separate_terminate_calls != 0 ||
        separate_delete_calls != 0) return 2;

    reset_case(0, 2);
    if (!probe_teardown_children() || s_owned_threads[0].active ||
        atomic_calls != 1 || separate_terminate_calls != 0 ||
        separate_delete_calls != 0) return 3;
    return 0;
}
'''
        fixture = root / "fixtures" / "psp_oracle"
        with tempfile.TemporaryDirectory(prefix="child-teardown-host-", dir=fixture) as scratch:
            source_path = Path(scratch) / "child_teardown_host.c"
            binary_path = Path(scratch) / "child_teardown_host.exe"
            source_path.write_text(harness, encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(source_path), "-o", str(binary_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            executed = subprocess.run(
                [str(binary_path)], capture_output=True, text=True, check=False
            )
            self.assertEqual(executed.returncode, 0, executed.stderr)

    def test_probe_teardown_is_ordered_and_main_never_self_deletes(self) -> None:
        probe = (Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        teardown = probe.split("static void probe_teardown(", 1)[1].split("\n}\n", 1)[0]
        ordered_calls = (
            "sceKernelDcacheWritebackAll()", "probe_teardown_children()",
            "probe_teardown_objects()", "probe_teardown_io_audio()",
            "probe_teardown_memory()", "probe_teardown_state()",
            "probe_teardown_host0(emulated)", "NAKAGAWA_PSP_COMPLETE",
            "probe_park_until_stop()",
        )
        positions = [teardown.index(call) for call in ordered_calls]
        self.assertEqual(positions, sorted(positions))
        main = probe.split("int main(int argc, char *argv[])", 1)[1]
        self.assertNotIn("sceKernelExitDeleteThread", main)
        teardown_test = probe.split("static void run_teardown_test(", 1)[1].split("\n}", 1)[0]
        self.assertNotIn("sceKernelExitDeleteThread", teardown_test)

    def test_registry_done_counts_every_record_before_it(self) -> None:
        """registry-done out2 is the number of records before it (parser contract).

        The 2026-10-08 probe counted only census records (categories + keys), so a
        finished console stream failed its completion-count check by exactly the
        fixed records. Both emission paths must now advance the counter.
        """

        probe = (Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle" /
                 "probe.c").read_text(encoding="utf-8")
        emit = probe.split("static void emit_registry_record(", 1)[1].split("\n}\n", 1)[0]
        short_path = emit.split("size_t capacity = 512u;", 1)[0]
        self.assertIn("emit_record_extended(", short_path)
        self.assertLess(
            short_path.index("emit_record_extended("), short_path.index("s_registry_records++;")
        )
        self.assertLess(short_path.index("s_registry_records++;"), short_path.index("return;"))
        long_path = emit.split("size_t capacity = 512u;", 1)[1]
        self.assertIn("s_registry_records++;", long_path)

    def test_probe_module_stop_ends_parked_main_through_the_crt_runtime_teardown(self) -> None:
        """The probe owns the stop half of the lifecycle crt0_prx starts.

        PSPSDK's PRX CRT creates main in module_start and exports no
        module_stop; its exit path ends in sceKernelExitGame, which PSPLink
        hooks (reset or a non-deleting thread exit). module_stop must therefore
        end and delete main itself, and main must leave through the CRT's
        runtime de-initialisation without deleting itself.
        """

        fixture = Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
        probe = (fixture / "probe.c").read_text(encoding="utf-8")

        def body(signature: str) -> str:
            return probe.split(signature, 1)[1].split("\n}\n", 1)[0]

        main = body("int main(int argc, char *argv[]) {")
        self.assertTrue(
            main.lstrip().startswith("(void)argc;\n    (void)argv;\n    "
                                     "s_probe_main_thread = sceKernelGetThreadId();")
        )
        park = body("static void probe_park_until_stop(void) {")
        park_calls = (
            "while (!s_probe_stop_requested) sceKernelSleepThread();",
            "_fini();", "__libcglue_deinit();", "sceKernelExitThread(0);",
        )
        positions = [park.index(call) for call in park_calls]
        self.assertEqual(positions, sorted(positions))
        stop = body("int module_stop(SceSize args, void *argp) {")
        stop_calls = (
            "if (main_thread < 0) return 1;",
            "s_probe_stop_requested = 1;",
            "sceKernelWakeupThread(main_thread)",
            "SceUInt timeout = PROBE_MAIN_STOP_TIMEOUT_US;",
            "if (sceKernelWaitThreadEnd(main_thread, &timeout) < 0) return 1;",
            "if (sceKernelDeleteThread(main_thread) < 0) return 1;",
            "return 0;",
        )
        positions = [stop.index(call) for call in stop_calls]
        self.assertEqual(positions, sorted(positions))
        for section in (park, stop):
            self.assertNotIn("sceKernelExitDeleteThread", section)
            self.assertNotIn("sceKernelTerminate", section)
            self.assertNotIn("sceKernelExitGame", section)
        self.assertRegex(probe, r"(?m)^#define PROBE_MAIN_STOP_TIMEOUT_US 1000000u$")

        makefile = (fixture / "Makefile").read_text(encoding="utf-8")
        self.assertIn(
            "ifneq ($(filter $(BUILD_DIR)/probe.o,$(OBJS)),)\n"
            "PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp\nendif\n",
            makefile,
        )
        export_rule = makefile.split("$(BUILD_DIR)/probe_exports.exp: Makefile\n", 1)[1]
        export_rule = export_rule.split("\n\n", 1)[0]
        exported = (
            "'PSP_EXPORT_START(syslib, 0, 0x8000)'", "'PSP_EXPORT_FUNC_HASH(module_start)'",
            "'PSP_EXPORT_FUNC_HASH(module_stop)'", "'PSP_EXPORT_VAR_HASH(module_info)'",
        )
        for entry in exported:
            self.assertIn(entry, export_rule)
        self.assertLess(
            makefile.index("PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp"),
            makefile.index("include $(PSPSDK)/lib/build.mak"),
        )


class PspOracleBuildRouteTests(unittest.TestCase):
    """Keep the merged CASE/Makefile/import contract structurally explicit."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )
        self.fixture = self.root / "fixtures" / "psp_oracle"

    def test_alarm_oracle_case_is_buildable_and_has_a_campaign_parser(self) -> None:
        self.assertRegex(
            self.makefile,
            r"(?m)^else ifeq \(\$\(CASE\),kernel-alarm\)$\nCASE_ID = \d+$",
        )
        runner = (self.root / "tools" / "psp_oracle" / "run_psplink.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("CAMPAIGN_PROBE_CASES", runner)
        self.assertEqual(
            _campaign_completeness_contract("kernel-alarm"),
            "strict-golden-sequence",
        )

    def test_supported_case_names_map_to_unique_case_ids(self) -> None:
        routes = re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        )
        # 1-67 are the sequential probe.c cases; 90-96 are the H-oracle HLE families,
        # numbered apart so sequential additions cannot collide with them.
        self.assertGreaterEqual(len(routes), 74)
        names = [name for name, _ in routes]
        ids = [int(case_id) for _, case_id in routes]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(ids), len(set(ids)))
        self.assertLessEqual(set(range(1, 68)) | set(range(90, 97)), set(ids))
        self.assertNotIn("psp_b1_imports.S", self.makefile)
        self.assertNotIn("psp_b2_imports.S", self.makefile)
        self.assertNotIn("psp_b3_imports.S", self.makefile)

    def test_vfpu_compare_route_has_its_own_case_id(self) -> None:
        # Each probe added after 67 pins its own route here, so the shared route count and
        # id set above stay a lower bound that no two probe changes need to edit together.
        routes = dict(re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        ))
        self.assertEqual(routes.get("vfpu-compare"), "68")

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
            "dma-cells",
            "dma-invalid-tail-memcpy-dst",
            "dma-invalid-tail-memcpy-src",
            "dma-invalid-tail-try-dst",
            "dma-invalid-tail-try-src",
            "dma-invalid-tail-s0",
            "dma-size-matrix",
            "dma-size-matrix-cell",
        ):
            self.assertIn(f"else ifeq ($(CASE),{case})", self.makefile)
        self.assertIn("LIBS = -lpspdmac", self.makefile)
        self.assertIn("sceDmacMemcpy(dst, src, size)", self.probe)
        self.assertIn("sceDmacTryMemcpy(dst, src, size)", self.probe)

    def test_dma_alignment_and_overlap_cells_stay_inside_owned_guarded_buffers(self) -> None:
        self.assertIn("DMAC_CELL_BYTES 64u", self.probe)
        self.assertIn("1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u, 11u, 13u, 14u, 15u", self.probe)
        self.assertIn("s_dmac_overlap_offsets[] = {1u, 2u, 3u, 7u, 15u}", self.probe)
        self.assertIn("sceKernelDcacheWritebackInvalidateRange", self.probe)
        self.assertIn("dmac_cell_guard_mutations", self.probe)
        self.assertIn('"dmac-cells-done", "PASS", 0u, done_out', self.probe)
        for case in ("ge-nan", "delay-zero"):
            self.assertIn(f"else ifeq ($(CASE),{case})", self.makefile)

    def test_audio_probe_uses_only_two_channel_blocks_and_releases_owned_channels(self) -> None:
        self.assertIn("AUDIO_QUERY_BLOCKS 2", self.probe)
        self.assertIn("sceAudioGetChannelRestLength", self.probe)
        self.assertIn("sceAudioOutputBlocking", self.probe)
        self.assertIn("sceAudioOutput2GetRestSample", self.probe)
        self.assertIn("sceAudioOutput2OutputBlocking", self.probe)
        self.assertIn("sceAudioChRelease(channel)", self.probe)
        self.assertIn("sceAudioOutput2Release()", self.probe)
        self.assertIn("sceAudioSRCChRelease()", self.probe)

    def test_delay_zero_probe_records_equal_priority_ready_worker_and_pending_callback(self) -> None:
        self.assertIn("sceKernelDelayThreadCB(0u)", self.probe)
        self.assertIn("sceKernelDelayThread(0u)", self.probe)
        self.assertIn("sceKernelNotifyCallback(callback, 0x5a)", self.probe)
        self.assertIn("worker_status_before == PSP_THREAD_READY", self.probe)
        self.assertIn("s_delay_zero_worker_runs", self.probe)
        self.assertIn("sceKernelGetSystemTimeLow() - start", self.probe)

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

    def test_invalid_tail_source_declares_setup_guards_and_skip_support(self) -> None:
        """Source-shape guard only; this does not prove fail-closed control flow."""
        self.assertIn("PSP_LARGE_MEMORY = 0", self.makefile)
        self.assertIn("sceKernelAllocPartitionMemory", self.probe)
        self.assertIn("PSP_SMEM_High", self.probe)
        self.assertIn("PSP_SMEM_Addr", self.probe)
        self.assertIn('"oracle-dmac-scratch"', self.probe)
        self.assertIn("DMAC_INVALID_SETUP_END_NEIGHBOR", self.probe)
        self.assertIn("DMAC_INVALID_SETUP_BEGIN_NEIGHBOR", self.probe)
        self.assertIn("0xffffffff", self.probe.lower())
        self.assertIn("dst = (void *)(uintptr_t)UINT32_MAX", self.probe)
        self.assertIn("DMAC_INVALID_PAYLOAD_BYTES + DMAC_INVALID_MAX_DELTA <=", self.probe)
        self.assertIn("_Static_assert", self.probe)
        self.assertNotIn("DMAC_BASELINE_USER_END", self.probe)
        self.assertNotIn("DMAC_BOUNDARY_BLOCK_BASE", self.probe)
        self.assertIn('"SKIP"', self.probe)

    def test_invalid_tail_source_declares_zero_size_s0_and_scratch_bounds(self) -> None:
        """Keep the source declarations visible; geometry is tested separately."""
        self.assertIn("dmac_call(api, dst, src, 0u)", self.probe)
        self.assertIn("dmac_call(DMAC_INVALID_API, dst, src, requested)", self.probe)
        self.assertIn("DMAC_INVALID_SCRATCH_BYTES", self.probe)
        self.assertIn("DMAC_INVALID_TAIL_OFFSET", self.probe)

    def test_invalid_tail_geometry_and_cache_discipline_are_explicit(self) -> None:
        for needle in (
            "DMAC_INVALID_PRE_GUARD_BYTES 0x1000u",
            "DMAC_INVALID_PAYLOAD_BYTES 0x0000c000u",
            "DMAC_INVALID_POST_GUARD_BYTES 0x1000u",
            "DMAC_INVALID_OVERFLOW_BAND_BYTES 0x2000u",
            "DMAC_INVALID_TAIL_GUARD_BYTES 0x1000u",
            "DMAC_INVALID_MAX_DELTA 0x2000u",
            "dmac_cell_cache_before(block_head, DMAC_INVALID_SCRATCH_BYTES)",
            "dmac_cell_cache_after(block_head, DMAC_INVALID_SCRATCH_BYTES)",
            "guards_outside",
            "post_guard",
            "overflow_band",
            "tier=%s",
            "#if defined(__mips__)",
        ):
            self.assertIn(needle, self.probe)
        self.assertIn("else ifeq ($(CASE),dma-invalid-tail-s0)\nCASE_ID = 60", self.makefile)

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
        record_writer = self.probe.split("static void emit_record_extended", 1)[1].split(
            "\n}\n#endif", 1
        )[0]
        self.assertIn("probe_emit_durable(emulated, line,", record_writer)
        writer = self.probe.split("static void probe_emit_durable", 1)[1].split("\n}\n", 1)[0]
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
        for test_id in ("PSP-SYSTEM-001",):
            entry = next(item for item in manifest["tests"] if item["id"] == test_id)
            self.assertEqual(entry["status"], "implemented")
            self.assertEqual(entry["hardware_evidence"], "CAPTURED")
            for api in entry["apis"]:
                self.assertNotIn(
                    api,
                    oracle_apis,
                    f"{api} reaches HARDWARE_MEASURED from a probe source, not a run",
                )

    def test_fpu_and_cache_probe_families_are_measured_by_the_campaign(self) -> None:
        """`status` stays probe-source prose; the hardware tier comes from the cited run."""
        manifest = self._oracle_manifest()
        by_id = {entry["id"]: entry for entry in manifest["tests"]}
        for test_id in ("PSP-FPU-001", "PSP-CACHE-001"):
            self.assertEqual(by_id[test_id]["status"], "planned")
            self.assertEqual(by_id[test_id]["hardware_evidence"], "MEASURED")
            self.assertEqual(
                by_id[test_id]["evidence_ref"],
                "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize",
            )
            self.assertEqual(by_id[test_id]["evidence_cases"], by_id[test_id]["case_ids"])
            self.assertIn("f6ccfb33", by_id[test_id]["measurement_note"])
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
        # Rows measured before the 2026-10-10 campaign keep their own citations; the campaign
        # on f6ccfb33 measured the eighteen others and captured two without acceptance.
        self.assertEqual(
            {test_id for test_id, value in evidence.items() if value == "MEASURED"},
            {
                "PSP-DMAC-001", "PSP-DISPLAY-001", "PSP-EXCEPTION-001", "PSP-KERNEL-002",
                "PSP-TRANSPORT-001", "PSP-ALARM-001", "PSP-THREAD-003", "PSP-WAIT-001",
                "PSP-KERNEL-STATUS-001", "PSP-REGISTRY-001", "PSP-KERNEL-MISC-001", "PSP-SMOKE-001",
                "PSP-THREAD-EXIT-001", "PSP-IO-001", "PSP-DISPLAY-002", "PSP-DISPLAY-003",
                "PSP-DISPLAY-004", "PSP-FPU-001", "PSP-CACHE-001", "PSP-AUDIO-001", "PSP-GE-001",
                "PSP-MUTEX-001",
            },
        )
        self.assertEqual(
            {test_id for test_id, value in evidence.items() if value == "CAPTURED"},
            {
                "PSP-KERNEL-001", "PSP-SYSTEM-001", "PSP-GE-CONTROL-001", "PSP-TEARDOWN-001",
                # The HLE measurement families ran as single-case runs on 2026-10-10 with
                # complete streams; none was acceptance-eligible (post-unload shell
                # qualification), so they are captured, not measured.
                "PSP-HLE-KERNEL-STATUS-001", "PSP-HLE-VTIMER-001", "PSP-HLE-POWER-001",
                "PSP-HLE-HPRM-001", "PSP-HLE-CTRL-LATCH-001", "PSP-HLE-SYSPARAM-001",
                "PSP-HLE-GE-EDRAM-001",
            },
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
        self.assertEqual(dmac["issues"], [23, 303])
        self.assertEqual(dmac["hardware_evidence"], "MEASURED")
        self.assertEqual(len(dmac["case_ids"]), 43)
        self.assertIn("dma-invalid-tail-s0", dmac["diagnostic_case_ids"])
        self.assertIn("invalid-tail-s0-a-memcpy", dmac["case_ids"])
        self.assertIn("invalid-tail-b1-delta-0001", dmac["case_ids"])
        self.assertIn("invalid-tail-b4-delta-2000", dmac["case_ids"])
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

    def test_new_probe_families_are_registered_with_expected_evidence(self) -> None:
        manifest = self._oracle_manifest()
        by_id = {entry["id"]: entry for entry in manifest["tests"]}
        expected = {
            "PSP-AUDIO-001": (AUDIO_SPEC, "audio-query"),
            "PSP-GE-001": (GE_NAN_SPEC, "ge-nan"),
            "PSP-KERNEL-002": (None, "delay-zero"),
            "PSP-DMAC-001": (None, "dma-cells"),
        }
        for test_id, (spec, campaign_case) in expected.items():
            with self.subTest(test_id=test_id):
                entry = by_id[test_id]
                self.assertIn(campaign_case, entry.get("diagnostic_case_ids", []))
                if spec is not None:
                    self.assertEqual(entry["case_ids"], list(spec.ordered_cases))
                if test_id in {"PSP-AUDIO-001", "PSP-GE-001"}:
                    self.assertEqual(entry["status"], "planned")
                    self.assertEqual(entry["hardware_evidence"], "MEASURED")
                    self.assertEqual(entry["evidence_cases"], entry["case_ids"])
        for case_id in ("audio-query", "ge-nan", "dma-cells", "delay-zero"):
            self.assertIn(f"else ifeq ($(CASE),{case_id})", self.makefile)
        delay_zero = by_id["PSP-KERNEL-002"]
        self.assertEqual(delay_zero["status"], "implemented")
        self.assertEqual(delay_zero["hardware_evidence"], "MEASURED")
        self.assertEqual(delay_zero["case_ids"], [
            "delay-threadcb-zero", "delay-thread-zero", "delay-zero-done"
        ])
        self.assertEqual(
            delay_zero["evidence_ref"],
            "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize",
        )
        self.assertEqual(delay_zero["evidence_cases"], [
            "delay-threadcb-zero", "delay-thread-zero", "delay-zero-done"
        ])
        oracle_doc = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        for case_id in delay_zero["evidence_cases"]:
            with self.subTest(case_id=case_id):
                self.assertIn(f"`{case_id}`", oracle_doc)
        self.assertIn(340, delay_zero["issues"])


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


class UserModeImportGateTests(unittest.TestCase):
    """The link-time gate that keeps kernel-only imports out of user-mode probes."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.makefile = (self.fixture / "Makefile").read_text(encoding="utf-8")

    def _run_main(self, data: bytes) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp) / "probe.elf"
            module.write_bytes(data)
            out = io.StringIO()
            err = io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                status = user_mode_imports.main([str(module)])
        return status, out.getvalue(), err.getvalue()

    def test_kernel_only_reason_classifies_library_names(self) -> None:
        for library in ("sceDisplay_driver", "sceImpose_driver", "sceCtrl_driver",
                        "InterruptManagerForKernel", "ThreadManForKernel", "sceUmd"):
            self.assertIsNotNone(user_mode_imports.kernel_only_reason(library), library)
        for library in ("sceDisplay", "sceImpose", "ThreadManForUser", "Kernel_Library",
                        "InterruptManager", "SysMemUserForUser", "sceUmdUser", "sceCtrl"):
            self.assertIsNone(user_mode_imports.kernel_only_reason(library), library)

    def test_user_mode_module_with_only_user_libraries_passes(self) -> None:
        data = build_import_elf([
            ("ThreadManForUser", [0x0F000001, 0x0F000002]),
            ("sceDisplay", [0x0F000003]),
            ("sceImpose", [0x0F000004]),
        ])
        result = user_mode_imports.check_module(data)
        self.assertTrue(result.passed)
        self.assertFalse(result.kernel_mode)
        self.assertEqual(result.libraries, ("ThreadManForUser", "sceDisplay", "sceImpose"))
        status, out, err = self._run_main(data)
        self.assertEqual(status, user_mode_imports.EXIT_OK)
        self.assertIn("OK", out)
        self.assertEqual(err, "")

    def test_user_mode_module_importing_kernel_libraries_fails_with_names_and_nids(self) -> None:
        data = build_import_elf([
            ("ThreadManForUser", [0x0F000001]),
            ("sceDisplay_driver", [0x0F000002]),
            ("sceImpose_driver", [0x0F000003, 0x0F000004]),
            ("InterruptManagerForKernel", [0x0F000005]),
        ])
        result = user_mode_imports.check_module(data)
        self.assertFalse(result.passed)
        self.assertEqual(
            [(v.library, v.nids) for v in result.violations],
            [("sceDisplay_driver", (0x0F000002,)),
             ("sceImpose_driver", (0x0F000003, 0x0F000004)),
             ("InterruptManagerForKernel", (0x0F000005,))],
        )
        status, out, err = self._run_main(data)
        self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
        self.assertEqual(out, "")
        self.assertIn("sceDisplay_driver (name ends in _driver): 0x0F000002", err)
        self.assertIn("sceImpose_driver (name ends in _driver): 0x0F000003, 0x0F000004", err)
        self.assertIn("InterruptManagerForKernel (name ends in ForKernel): 0x0F000005", err)
        self.assertIn("0x8002013C", err)
        self.assertNotIn("ThreadManForUser (", err)

    def test_kernel_mode_module_is_outside_the_rule(self) -> None:
        data = build_import_elf(
            [("sceDisplay_driver", [0x0F000002])],
            module_attributes=user_mode_imports.PSP_MODULE_KERNEL,
        )
        result = user_mode_imports.check_module(data)
        self.assertTrue(result.kernel_mode)
        self.assertTrue(result.passed)
        status, out, _err = self._run_main(data)
        self.assertEqual(status, user_mode_imports.EXIT_OK)
        self.assertIn("kernel-mode module (SceModuleInfo attribute 0x1000)", out)

    def test_unattributed_stub_slots_fail_closed(self) -> None:
        user_shape = [(name.replace("ForKernel", "ForUser"), first, count)
                      for name, first, count in INTERLEAVED_SHAPE]
        result = user_mode_imports.check_module(
            build_interleaved_import_elf(user_shape, INTERLEAVED_NIDS))
        self.assertFalse(result.passed)
        self.assertEqual([v.library for v in result.violations], [UNATTRIBUTED_LIBRARY])

    def test_malformed_import_table_fails_closed(self) -> None:
        status, out, err = self._run_main(build_import_elf(
            [("ThreadManForUser", [0x0F000001])], corrupt="truncated_file"))
        self.assertEqual(status, user_mode_imports.EXIT_UNREADABLE)
        self.assertEqual(out, "")
        self.assertIn("cannot read import table", err)
        with redirect_stderr(io.StringIO()):
            self.assertEqual(user_mode_imports.main([]), user_mode_imports.EXIT_UNREADABLE)

    def test_hand_written_import_blocks_name_only_user_libraries(self) -> None:
        sources = sorted(self.fixture.glob("*.S"))
        self.assertTrue(sources)
        for source in sources:
            text = source.read_text(encoding="utf-8")
            for library in re.findall(r'IMPORT_(?:START|FUNC)\s+"([^"]+)"', text):
                self.assertIsNone(
                    user_mode_imports.kernel_only_reason(library),
                    f"{source.name} imports kernel-only library {library}",
                )

    def test_probe_sources_use_no_kernel_headers_or_kernel_archives(self) -> None:
        for source in sorted(self.fixture.glob("*.c")):
            text = source.read_text(encoding="utf-8")
            self.assertIsNone(
                re.search(r"#include\s*<psp\w*_(?:driver|kernel)\.h>", text),
                f"{source.name} includes a kernel-only PSPSDK header",
            )
        self.assertEqual(re.findall(r"-lpsp\w*(?:_kernel|_driver)\w*", self.makefile), [])

    def test_makefile_runs_the_gate_after_fixup_and_deletes_rejected_elves(self) -> None:
        fixup = next(line for line in self.makefile.splitlines() if line.startswith("FIXUP = "))
        self.assertLess(fixup.index("psp-fixup-imports"), fixup.index("$(USER_MODE_IMPORT_GATE)"))
        self.assertIn(
            "USER_MODE_IMPORT_GATE = $(FIXTURE_DIR)../../tools/psp_oracle/user_mode_imports.py",
            self.makefile,
        )
        self.assertTrue((self.root / "tools" / "psp_oracle" / "user_mode_imports.py").is_file())
        self.assertIn("\n.DELETE_ON_ERROR:\n", self.makefile)

    def test_display_user_imports_are_linked_into_kernel_misc_only(self) -> None:
        links = re.findall(r"^OBJS \+?= .*display_user_imports\.o.*$", self.makefile, re.MULTILINE)
        self.assertEqual(len(links), 1)
        start = self.makefile.index(links[0])
        self.assertEqual(self.makefile.rfind("ifeq ($(CASE),kernel-misc)", 0, start),
                         self.makefile.rfind("ifeq (", 0, start))
        shared = (self.fixture / "threadman_user_imports.S").read_text(encoding="utf-8")
        self.assertEqual(re.findall(r'IMPORT_START\s+"([^"]+)"', shared), ["ThreadManForUser"])


class ProbeProgressAndBoundedWaitTests(unittest.TestCase):
    """Step markers, bounded waits and record shapes of the campaign probes."""

    NEW_CASES = ("KERNEL_ALARM", "THREAD_SCHEDULER", "WAIT_OUTCOMES", "GE_BREAK_CONTINUE",
                 "REFER_STATUS_SIZE", "REGISTRY_READONLY", "KERNEL_MISC", "VFPU_COMPARE")

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")

    def _case_block(self, macro: str) -> str:
        start = self.probe.index(f"#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_{macro}\n")
        end = self.probe.index("\n#endif", start)
        return self.probe[start:end]

    def _function(self, signature: str) -> str:
        start = self.probe.index(signature)
        end = self.probe.index("\n}\n", start)
        return self.probe[start:end]

    def _stream(self, rows: list[str]) -> str:
        return META.format(
            source="psp", model="PSP-3000", firmware="6.61-ARK",
            binary=MEASURED_SHA, commit=MEASURED_COMMIT,
        ) + "".join(rows)

    def _campaign_rows(self, case: str, statuses: dict[str, str] | None = None) -> list[str]:
        spec, counts = CAMPAIGN_PROBE_CASES[case]
        rows = []
        for case_id in spec.ordered_cases:
            if case_id == spec.terminal_case:
                outs = f" out0=0x{len(spec.ordered_cases) - 1:x}"
            else:
                outs = "".join(f" out{i}=0x0" for i in range(counts[case_id]))
            status = (statuses or {}).get(case_id, "PASS")
            rows.append(
                f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={case_id} "
                f"status={status} result=0x0{outs}\n"
            )
        return rows

    def test_step_markers_are_collected_and_never_count_as_results(self) -> None:
        rows = self._campaign_rows("ge-break-continue")
        stepped = (rows[:5]
                   + ["NAKAGAWA_PSP_STEP schema=1 case_id=ge-break-continue step=continue-drain\n"]
                   + rows[5:])
        report = parse_campaign_probe_output(self._stream(stepped), "ge-break-continue")
        self.assertEqual(len(report.results), len(rows))
        parsed = parse_output(self._stream(stepped))
        self.assertEqual(parsed.last_step.case_id, "ge-break-continue")
        self.assertEqual(parsed.last_step.step, "continue-drain")
        self.assertIsNone(parse_output(self._stream(rows)).last_step)
        for bad in (
            "NAKAGAWA_PSP_STEP schema=1 case_id=x\n",
            "NAKAGAWA_PSP_STEP schema=2 case_id=x step=y\n",
            "NAKAGAWA_PSP_STEP schema=1 case_id=x step=has space\n",
            "NAKAGAWA_PSP_STEP schema=1 case_id=x step=y extra=1\n",
        ):
            with self.assertRaises(ProtocolError, msg=bad):
                parse_output(self._stream(rows[:1] + [bad]))

    def test_runner_completion_gate_accepts_host0_logs_with_step_markers(self) -> None:
        rows = self._campaign_rows("ge-break-continue")
        host0 = self._stream(
            ["NAKAGAWA_PSP_STEP schema=1 case_id=ge-break-continue step=break-active-list\n"]
            + rows
        ) + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
        self.assertTrue(_campaign_stream_complete(host0, "ge-break-continue"))
        truncated = self._stream(
            rows[:5]
            + ["NAKAGAWA_PSP_STEP schema=1 case_id=ge-break-continue step=continue-drain\n"]
        )
        self.assertFalse(_campaign_stream_complete(truncated, "ge-break-continue"))

    def test_ge_break_continue_records_a_timeout_as_a_measured_outcome(self) -> None:
        rows = self._campaign_rows(
            "ge-break-continue",
            {"ge-continue-drain": "TIMEOUT", "ge-quiesce-after-continue": "TIMEOUT"},
        )
        report = parse_campaign_probe_output(self._stream(rows), "ge-break-continue")
        statuses = {record.case_id: record.status for record in report.results.values()}
        self.assertEqual(statuses["ge-continue-drain"], "TIMEOUT")

    def test_campaign_specs_match_the_records_the_probe_emits(self) -> None:
        defines = {name: int(value) for name, value in
                   re.findall(r"^#define (\w+) (\d+)u?$", self.probe, re.MULTILINE)}
        for case, signature, helpers in (
            ("kernel-misc", "static void run_kernel_misc(int emulated) {", {}),
            ("ge-break-continue", "static void run_ge_break_continue(int emulated) {",
             {"emit_ge_quiesce": defines["GE_QUIESCE_OUTS"]}),
        ):
            body = self._function(signature)
            emitted: dict[str, int] = {}
            for call in re.finditer(
                    r"\b(emit_record_extended|emit_ge_control|emit_ge_quiesce)\((.*?)\);",
                    body, re.DOTALL):
                name, args = call.groups()
                ids = re.findall(r'"([a-z0-9][a-z0-9-]*)"', args)
                self.assertEqual(len(ids), 1, args)
                if name in helpers:
                    emitted[ids[0]] = helpers[name]
                    continue
                count = re.search(r",\s*(\w+)\s*$", args).group(1)
                emitted[ids[0]] = int(count) if count.isdigit() else defines[count]
            spec, counts = CAMPAIGN_PROBE_CASES[case]
            self.assertEqual(emitted, counts, case)
            self.assertEqual(tuple(emitted), spec.ordered_cases, case)
            done = re.search(r"uint32_t done = (\d+);", body)
            self.assertEqual(int(done.group(1)), len(spec.ordered_cases) - 1, case)

    def test_no_probe_blocks_on_the_ge_without_a_bound(self) -> None:
        for source in sorted(self.fixture.glob("*.c")):
            text = re.sub(r"/\*.*?\*/|//[^\n]*", "", source.read_text(encoding="utf-8"),
                          flags=re.DOTALL)
            self.assertIsNone(re.search(r"sceGeListSync\([^;]*,\s*0\s*\)", text), source.name)
            self.assertIsNone(re.search(r"sceGeDrawSync\(\s*0\s*\)", text), source.name)
            self.assertNotIn("sceGuSync(", text, source.name)

    def test_ge_break_continue_writes_the_list_back_before_enqueueing_it(self) -> None:
        body = self._function("static void run_ge_break_continue(int emulated) {")
        build = body.index("ge_build_list(")
        writeback = body.index("sceKernelDcacheWritebackAll();")
        enqueue = body.index("sceGeListEnQueue(")
        self.assertLess(build, writeback)
        self.assertLess(writeback, enqueue)
        self.assertNotIn("sceGeListUpdateStallAddr", body)

    def test_new_campaign_cases_have_no_unbounded_waits_or_loops(self) -> None:
        for macro in self.NEW_CASES:
            block = self._case_block(macro)
            self.assertNotIn("for (;;)", block, macro)
            self.assertNotIn("sceKernelSleepThread(", block, macro)
            for call in re.finditer(r"sceKernel(?:WaitSema|WaitThreadEnd)\(([^;]*)\);", block):
                self.assertFalse(call.group(1).rstrip().endswith("NULL"), (macro, call.group(0)))
            self.assertIn("probe_step(emulated,", block, macro)

    def test_exhaustion_loops_are_capped_and_record_the_cap(self) -> None:
        registry = self._case_block("REGISTRY_READONLY")
        self.assertIn("#define REGISTRY_OPEN_CAP 256u", registry)
        self.assertIn("while (opened_handles < REGISTRY_OPEN_CAP)", registry)
        self.assertIn("REGISTRY_OPEN_CAP,\n", registry)
        alarm = self._case_block("KERNEL_ALARM")
        self.assertIn("#define ALARM_EXHAUSTION_CAP 1024u", alarm)
        self.assertIn("while (alarm_count < ALARM_EXHAUSTION_CAP)", alarm)
        self.assertIn("out[2] = ALARM_EXHAUSTION_CAP;", alarm)

    def test_registry_forged_handle_call_runs_last_under_its_own_step(self) -> None:
        body = self._function("static void run_registry_readonly(int emulated) {")
        bad = body.index('REGISTRY_STEP("bad-handle");')
        for earlier in ('"registry-errors"', 'walk_registry_category(', 'REGISTRY_STEP("handle-exhaustion");'):
            self.assertLess(body.index(earlier), bad, earlier)
        self.assertLess(bad, body.index("sceRegGetKeysNum((REGHANDLE)0xffffffffu"))
        self.assertLess(body.index('"registry-bad-handle"'), body.index('"registry-done"'))


class CampaignHost0LogTests(unittest.TestCase):
    """Every campaign case writes its evidence to the host0 log the runner reads."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.probe = (self.root / "fixtures" / "psp_oracle" / "probe.c").read_text(encoding="utf-8")
        self.makefile = (self.root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8")

    def _probe_logs_by_case(self) -> dict[str, str]:
        case_ids = {name: int(value) for name, value in re.findall(
            r"^(?:else )?ifeq \(\$\(CASE\),([^)]+)\)\nCASE_ID = (\d+)$", self.makefile,
            re.MULTILINE)}
        macros = {int(value): name for name, value in re.findall(
            r"^#define PSP_ORACLE_CASE_(\w+) (\d+)$", self.probe, re.MULTILINE)}
        logs = dict(re.findall(
            r"^#(?:el)?if PSP_ORACLE_CASE == PSP_ORACLE_CASE_(\w+)\n"
            r"#define PROBE_HOST0_LOG \"host0:/([^\"]+)\"$", self.probe, re.MULTILINE))
        return {case: logs.get(macros.get(case_id, ""), "")
                for case, case_id in case_ids.items()}

    def test_every_campaign_case_has_the_host0_log_the_runner_reads(self) -> None:
        logs = self._probe_logs_by_case()
        for case in CAMPAIGN_QUEUE_CASES:
            expected = _campaign_host0_log_path(Path("host0"), case).name
            self.assertEqual(logs.get(case), expected, case)

    def test_host0_lines_go_through_the_durable_writer(self) -> None:
        # Records and step markers append through probe_emit_durable(); only
        # the metadata line (which truncates the log at start) and the final
        # completion marker open the log themselves.
        self.assertEqual(self.probe.count("sceIoOpen(PROBE_HOST0_LOG"), 3)
        writer = self.probe[self.probe.index("static void probe_emit_durable("):]
        writer = writer[:writer.index("\n}\n")]
        self.assertIn("PSP_O_APPEND", writer)
        self.assertIn("sceIoClose(fd);", writer)
        self.assertIn("emit(emulated, line);", writer)
        for emitter in ("static void emit_record_extended(", "static void probe_step("):
            body = self.probe[self.probe.index(emitter):]
            body = body[:body.index("\n}\n")]
            self.assertIn("probe_emit_durable(emulated, line,", body, emitter)




class VfpuCompareProbeTests(unittest.TestCase):
    """PSP-VFPU-CMP-001: vscmp.s, vsge.s and vslt.s cells against the project's model.

    The model is UNMEASURED on the console. These tests pin the probe's operand table
    to an independent Python copy of the interpreter's lane compares (VFPU3 sub-ops 5,
    6 and 7 in src/rt/vfpu_interp.c), the fixed record shape the parser registry
    expects, the emission order, and the case's wiring (Makefile number, host0 log,
    queue slot, manifest row, documentation).
    """

    CASE = "vfpu-compare"
    TEST_ID = "PSP-VFPU-CMP-001"
    OPS = ("vscmp", "vsge", "vslt")
    PAIRS = (
        ("lt", 0x3FC00000, 0x40200000),
        ("eq", 0x3FC00000, 0x3FC00000),
        ("gt", 0x40200000, 0x3FC00000),
        ("zero-pos-neg", 0x00000000, 0x80000000),
        ("zero-neg-pos", 0x80000000, 0x00000000),
        ("nan-left-quiet", 0x7FC00000, 0x3FC00000),
        ("nan-right-quiet", 0x3FC00000, 0x7FC00000),
        ("nan-left-signal", 0x7F800001, 0x3FC00000),
        ("nan-right-signal", 0x3FC00000, 0x7F800001),
        ("nan-left-negative", 0xFFC00000, 0x3FC00000),
        ("inf-pos-neg", 0x7F800000, 0xFF800000),
        ("inf-neg-pos", 0xFF800000, 0x7F800000),
        ("inf-pos-pos", 0x7F800000, 0x7F800000),
        ("inf-neg-neg", 0xFF800000, 0xFF800000),
        ("inf-pos-finite", 0x7F800000, 0x3FC00000),
    )

    @classmethod
    def cells(cls) -> tuple[str, ...]:
        return tuple(f"{op}-{pair[0]}" for op in cls.OPS for pair in cls.PAIRS)

    @classmethod
    def model_word(cls, op: str, a: int, b: int) -> int:
        """The project's model for one lane; IEEE comparisons, so a NaN compares false."""
        import struct

        x = struct.unpack("<f", struct.pack("<I", a))[0]
        y = struct.unpack("<f", struct.pack("<I", b))[0]
        if op == "vscmp":
            value = -1.0 if x < y else (1.0 if x > y else 0.0)
        elif op == "vsge":
            value = 1.0 if x >= y else 0.0
        else:
            value = 1.0 if x < y else 0.0
        return struct.unpack("<I", struct.pack("<f", value))[0]

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")
        self.makefile = (self.fixture / "Makefile").read_text(encoding="utf-8")

    def _block(self) -> str:
        start = self.probe.index(f"#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_{self.CASE.upper().replace('-', '_')}\n")
        return self.probe[start:self.probe.index("\n#endif", start)]

    def _stream(self, rows: list[str]) -> str:
        return META.format(
            source="psp", model="PSP-3000", firmware="6.61-ARK",
            binary=MEASURED_SHA, commit=MEASURED_COMMIT,
        ) + "".join(rows)

    def _rows(self, statuses: dict[str, str] | None = None) -> list[str]:
        spec, counts = CAMPAIGN_PROBE_CASES[self.CASE]
        rows = []
        for index, case_id in enumerate(spec.semantic_cases):
            status = (statuses or {}).get(case_id, "PASS" if index % 2 else "FAIL")
            outs = "".join(f" out{i}=0x{i:08x}" for i in range(3))
            rows.append(
                f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} case_id={case_id} "
                f"status={status} result=0x{index:08x}{outs}\n"
            )
        rows.append(
            f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} case_id={spec.terminal_case} "
            f"status=PASS result=0x0 out0=0x{spec.terminal_count:x}\n"
        )
        return rows

    def test_probe_table_matches_the_project_model(self) -> None:
        words = {"VFPU_COMPARE_ONE": 0x3F800000, "VFPU_COMPARE_MINUS_ONE": 0xBF800000}
        table = self.probe[self.probe.index("static const struct vfpu_compare_pair s_vfpu_compare_pairs"):]
        table = table[:table.index("};")]
        rows = re.findall(
            r'\{"([a-z-]+)", (0x[0-9a-fA-F]+)u, (0x[0-9a-fA-F]+)u, \{([^}]*)\}\}', table
        )
        self.assertEqual(len(rows), len(self.PAIRS))
        for (name, a, b, expect), (want_name, want_a, want_b) in zip(rows, self.PAIRS, strict=True):
            with self.subTest(pair=name):
                self.assertEqual(name, want_name)
                self.assertEqual(int(a, 16), want_a)
                self.assertEqual(int(b, 16), want_b)
                words_in_row = [
                    words[token.strip()] if token.strip() in words else int(token.strip().rstrip("u"), 0)
                    for token in expect.split(",")
                ]
                self.assertEqual(
                    words_in_row,
                    [self.model_word(op, want_a, want_b) for op in self.OPS],
                )

    def test_registered_spec_is_the_op_major_cell_list_with_a_terminal_count(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES[self.CASE]
        self.assertEqual(spec.test_id, self.TEST_ID)
        self.assertEqual(spec.semantic_cases, self.cells())
        self.assertEqual(spec.terminal_case, "vfpu-compare-done")
        self.assertEqual(spec.terminal_count, 45)
        self.assertEqual(set(counts), set(spec.ordered_cases))
        self.assertEqual({counts[case] for case in spec.semantic_cases}, {3})
        self.assertEqual(counts[spec.terminal_case], 1)
        self.assertEqual(_campaign_completeness_contract(self.CASE), "strict-golden-sequence")

    def test_probe_names_each_cell_from_the_op_and_pair_tables(self) -> None:
        self.assertIn('static const char *const op_names[VFPU_COMPARE_OPS] = {"vscmp", "vsge", "vslt"};',
                      self.probe)
        self.assertIn('snprintf(case_id, sizeof(case_id), "%s-%s", op_names[op], pair->name);',
                      self.probe)
        self.assertIn('emit_record_extended(emulated, "PSP-VFPU-CMP-001", "vfpu-compare-done",',
                      self.probe)
        self.assertIn("#define VFPU_COMPARE_PAIRS 15u", self.probe)
        self.assertEqual(len(self.PAIRS), 15)

    def test_measured_span_has_no_host_io_and_uses_only_the_vfpu_compare_instructions(self) -> None:
        block = self._block()
        cells = block[block.index('probe_step(emulated, "vfpu-compare", "cells");'):]
        cells = cells[:cells.index("for (uint32_t op = 0; op < VFPU_COMPARE_OPS; op++) {\n        for (uint32_t p = 0; p < VFPU_COMPARE_PAIRS; p++) {\n            const struct vfpu_compare_pair *pair = &s_vfpu_compare_pairs[p];\n            const uint32_t index")]
        self.assertNotIn("emit_", cells)
        self.assertNotIn("snprintf", cells)
        self.assertNotIn("for (;;)", block)
        for mnemonic in ("vscmp.s S002, S000, S001", "vsge.s S002, S000, S001",
                         "vslt.s S002, S000, S001"):
            self.assertEqual(self.probe.count(mnemonic), 1, mnemonic)
        self.assertIn('__asm__ volatile("ctc1 $0, $31" ::: "memory");', block)
        self.assertIn("#define VFPU_COMPARE_SENTINEL 0x7a5a5a5au", block)

    def test_status_says_the_cell_measured_not_that_it_matched_the_model(self) -> None:
        # The runner accepts a capture only when every record is PASS, so a console that
        # disagrees with the UNMEASURED model must still produce PASS records: the status
        # marks a written destination, and agreement is result against out0.
        block = self._block()
        self.assertIn('observed[index] == VFPU_COMPARE_SENTINEL ? "FAIL" : "PASS",', block)
        self.assertNotIn("== expect ?", block)
        self.assertIn("const uint32_t out[] = {expect, pair->a, pair->b};", block)

    def test_the_case_runs_with_the_vfpu_thread_attribute(self) -> None:
        # The attribute chain is `#elif GE_NAN || VFPU_COMPARE`, so the VFPU case is on
        # a continuation line; its PSP_MAIN_THREAD_ATTR must carry THREAD_ATTR_VFPU.
        start = self.probe.index(
            "#elif PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_NAN || \\\n"
            "      PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE\n"
        )
        attr = self.probe[start:self.probe.index("#else", start)]
        self.assertIn("PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);", attr)

    def test_complete_stream_parses_with_every_cell_pass_fail_or_skip(self) -> None:
        statuses = {case: "SKIP" for case in self.cells()[:2]}
        report = parse_campaign_probe_output(self._stream(self._rows(statuses)), self.CASE)
        self.assertTrue(report.complete)
        self.assertTrue(report.terminal_present)
        self.assertEqual(report.record_count, 46)
        seen = {record.status for case_id, record in report.results.items()
                if case_id in self.cells()}
        self.assertEqual(seen, {"PASS", "FAIL", "SKIP"})
        self.assertEqual(report.results["vscmp-lt"].status, "SKIP")

    def test_missing_extra_out_of_order_and_unknown_cells_are_refused(self) -> None:
        rows = self._rows()
        cases = [row.split("case_id=")[1].split()[0] for row in rows]
        self.assertEqual(cases[10], "vscmp-inf-pos-neg")
        duplicate = rows[:10] + [rows[10]] + rows[10:]
        swapped = rows[:10] + [rows[11], rows[10]] + rows[12:]
        unknown = rows[:10] + [rows[10].replace(cases[10], "vscmp-bogus")] + rows[11:]
        foreign = rows[:10] + [rows[10].replace(self.TEST_ID, "PSP-VFPU-CMP-002")] + rows[11:]
        short_terminal = rows[:-1] + [rows[-1].replace("out0=0x2d", "out0=0x2c")]
        # Both strictness modes refuse these shapes: none of them is a truncation.
        for name, body in (("duplicate", duplicate), ("swapped", swapped),
                           ("unknown", unknown), ("foreign", foreign),
                           ("short-terminal", short_terminal)):
            with self.subTest(shape=name):
                with self.assertRaises(ProtocolError):
                    parse_campaign_probe_output(self._stream(body), self.CASE)
                with self.assertRaises(ProtocolError):
                    parse_campaign_probe_output(self._stream(body), self.CASE,
                                                require_complete=False)
        # A missing middle cell is a truncation shape: strict mode refuses it and the
        # inspection mode reports it incomplete without passing it.
        missing = rows[:10] + rows[11:]
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(self._stream(missing), self.CASE)
        self.assertFalse(parse_campaign_probe_output(
            self._stream(missing), self.CASE, require_complete=False).complete)

    def test_manifest_row_is_not_run_and_grants_no_hardware_tier(self) -> None:
        manifest = json.loads((self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
            encoding="utf-8"))
        row = next(test for test in manifest["tests"] if test["id"] == self.TEST_ID)
        spec, _counts = CAMPAIGN_PROBE_CASES[self.CASE]
        self.assertEqual(row["hardware_evidence"], "NOT_RUN")
        self.assertEqual(row["status"], "implemented")
        self.assertEqual(row["case_ids"], list(spec.ordered_cases))
        self.assertIn("UNMEASURED", row["evidence_note"])
        self.assertNotIn("evidence_ref", row)
        self.assertNotIn(self.TEST_ID, {test_id for ids in
                                        hle_manifest.oracle_exercised_apis(manifest).values()
                                        for test_id in ids})

    def test_case_is_wired_into_the_makefile_queue_and_documentation(self) -> None:
        from psp_oracle.run_psplink import CAMPAIGN_CASE_ESTIMATE_SECONDS

        self.assertIn("#define PSP_ORACLE_CASE_VFPU_COMPARE 68", self.probe)
        self.assertIn("else ifeq ($(CASE),vfpu-compare)\nCASE_ID = 68\n", self.makefile)
        self.assertEqual(_campaign_host0_log_path(Path("host0"), self.CASE).name,
                         "vfpu_compare_log.txt")
        self.assertIn('#define PROBE_HOST0_LOG "host0:/vfpu_compare_log.txt"', self.probe)
        queue = list(CAMPAIGN_QUEUE_CASES)
        self.assertEqual(queue[queue.index("kernel-misc") + 1], self.CASE)
        self.assertIn(self.CASE, CAMPAIGN_CASE_ESTIMATE_SECONDS)
        oracle_doc = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        self.assertIn("| `vfpu-compare` | `PSP-VFPU-CMP-001` | `NOT_RUN`", oracle_doc)
        readme = (self.fixture / "README.md").read_text(encoding="utf-8")
        self.assertIn("| `vfpu-compare` | `PSP-VFPU-CMP-001` |", readme)


class HleMeasureProbeTests(unittest.TestCase):
    """H-oracle-hle-measure-1009: seven one-launch HLE measurement families.

    The probes measure what the console returns. These tests pin what makes each
    launch safe and parseable: the Makefile case table and the import block each
    PRX links, NIDs derived from their names, the source rules (user imports only,
    a STEP marker before each measured call, no state-changing Set call except the
    restored GE width), the host0 log each family writes, the fixed record shape
    the parser registry expects, and the GE restore invariant.
    """

    FAMILIES = (
        # campaign case, CASE_ID, PRX stem, family import block, probe section macro, test id
        ("hle-kernel-status", 90, "hle_kernel_status", None,
         "HLE_CASE_KERNEL_STATUS", "PSP-HLE-KERNEL-STATUS-001"),
        ("hle-vtimer", 91, "hle_vtimer", None, "HLE_CASE_VTIMER", "PSP-HLE-VTIMER-001"),
        ("hle-power-clock", 92, "hle_power_clock", "hle_power_imports.S",
         "HLE_CASE_POWER_CLOCK", "PSP-HLE-POWER-001"),
        ("hle-hprm", 93, "hle_hprm", "hle_hprm_imports.S", "HLE_CASE_HPRM", "PSP-HLE-HPRM-001"),
        ("hle-ctrl-latch", 94, "hle_ctrl_latch", "hle_ctrl_imports.S",
         "HLE_CASE_CTRL_LATCH", "PSP-HLE-CTRL-LATCH-001"),
        ("hle-sysparam", 95, "hle_sysparam", "hle_utility_imports.S",
         "HLE_CASE_SYSPARAM", "PSP-HLE-SYSPARAM-001"),
        ("hle-ge-edram", 96, "hle_ge_edram", "hle_ge_imports.S",
         "HLE_CASE_GE_EDRAM", "PSP-HLE-GE-EDRAM-001"),
    )
    # Every family block: file -> (library, version word, {function: NID}). The
    # version word 0x40010000 is the one the PSPSDK import stubs carry.
    PINNED_BLOCKS = {
        "hle_power_imports.S": ("scePower", 0x40010000, {
            "scePowerGetPllClockFrequencyInt": 0x34F9C463,
            "scePowerGetPllClockFrequencyFloat": 0xEA382A27,
            "scePowerGetCpuClockFrequency": 0xFEE03A2F,
            "scePowerGetCpuClockFrequencyInt": 0xFDB5BFE9,
            "scePowerGetCpuClockFrequencyFloat": 0xB1A52C83,
            "scePowerGetBusClockFrequency": 0x478FE6F5,
            "scePowerGetBusClockFrequencyInt": 0xBD681969,
            "scePowerGetBusClockFrequencyFloat": 0x9BADB3EB,
        }),
        "hle_hprm_imports.S": ("sceHprm", 0x40010000, {
            "sceHprmIsRemoteExist": 0x208DB1BD,
            "sceHprmIsHeadphoneExist": 0x7E69EDA4,
            "sceHprmIsMicrophoneExist": 0x219C58F1,
        }),
        "hle_ctrl_imports.S": ("sceCtrl", 0x40010000, {
            "sceCtrlReadLatch": 0x0B588501,
            "sceCtrlPeekLatch": 0xB1D0E5CD,
        }),
        "hle_utility_imports.S": ("sceUtility", 0x40010000, {
            "sceUtilityGetSystemParamInt": 0xA5DA2406,
            "sceUtilityGetSystemParamString": 0x34B78343,
        }),
        "hle_ge_imports.S": ("sceGe_user", 0x40010000, {
            "sceGeEdramGetSize": 0x1F6752AD,
            "sceGeEdramGetAddr": 0xE47E40E4,
            "sceGeEdramSetAddrTranslation": 0xB77905EA,
        }),
    }
    # Calls the probe makes that come from the SDK's default link (IO), not from a
    # declared stub block. Libc and CRT helpers are not sce-prefixed.
    SDK_DEFAULT_CALLS = frozenset({"sceIoOpen", "sceIoWrite", "sceIoRead", "sceIoClose"})
    MEASURED_CALL_RE = re.compile(
        r"\b(sceKernel(?:ReferSystemStatus|ReferFplStatus|CreateFpl|TryAllocateFpl|FreeFpl|"
        r"DeleteFpl|CreateVTimer|ReferVTimerStatus|GetVTimerTime|StartVTimer|StopVTimer|"
        r"DeleteVTimer|DelayThread)|scePowerGet\w+|sceHprmIs\w+|sceCtrl\w*Latch|"
        r"sceUtilityGetSystemParam\w+|sceGeEdram\w+)\s*\("
    )
    # A line that is nothing but a whole `int|float|void sce...(...);` declaration.
    # Only such a line is skipped as "not a call"; a declaration that shares its line
    # with a call is still scanned.
    PROTOTYPE_LINE_RE = re.compile(r"\s*(?:int|float|void)\s+sce\w+\s*\([^;]*\);\s*")
    GE_UNSET_OVERRIDES = {
        "edram-width-query-initial": ("PASS", 0x0, None),
        **{name: ("SKIP", 0, [0]) for name in (
            "edram-width-set-512", "edram-width-set-1024", "edram-width-set-2048",
            "edram-width-set-4096", "edram-width-restore")},
        "edram-width-query-final": ("PASS", 0x0, None),
    }
    # The behaviour captured on the PSP-3000 on 2026-10-10 (initial width 0x400; CAPTURED, the
    # run was not acceptance-eligible) through the revised probe: sceGeEdramSetAddrTranslation(w)
    # sets the width to w and returns the width it replaced, so each Set after a Set(0) returns
    # 0; the restore's verifying Set(initial) returns the restored width and keeps it, and the
    # final Set(initial) returns it again.
    GE_MEASURED_OVERRIDES = {
        "edram-width-query-initial": ("PASS", 0x400, None),
        "edram-width-set-512": ("PASS", 0x0, [0x200]),
        "edram-width-set-1024": ("PASS", 0x0, [0x400]),
        "edram-width-set-2048": ("PASS", 0x0, [0x800]),
        "edram-width-set-4096": ("PASS", 0x0, [0x1000]),
        "edram-width-restore": ("PASS", 0x0, [0x400]),
        "edram-width-query-final": ("PASS", 0x400, [0x400]),
    }

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[1]
        cls.fixture = cls.root / "fixtures" / "psp_oracle"
        cls.makefile = (cls.fixture / "Makefile").read_text(encoding="utf-8")
        cls.probe = (cls.fixture / "probe_hle_measure.c").read_text(encoding="utf-8")
        cls.threadman = (cls.fixture / "threadman_user_imports.S").read_text(encoding="utf-8")
        cls.manifest = json.loads((cls.root / "tools" / "psp_oracle" / "manifest.json")
                                  .read_text(encoding="utf-8"))

    @staticmethod
    def _nid(name: str) -> int:
        return int.from_bytes(hashlib.sha1(name.encode("ascii")).digest()[:4], "little")

    @staticmethod
    def _imports(text: str) -> list[tuple[str, int, str]]:
        return [
            (library, int(nid, 16), name)
            for library, nid, name in re.findall(
                r'IMPORT_FUNC\s+"([^"]+)",\s*0x([0-9A-Fa-f]+),\s*(\w+)', text)
        ]

    def _block_text(self, block: str) -> str:
        return (self.fixture / block).read_text(encoding="utf-8")

    def _declared_names(self, block: str | None) -> set[str]:
        names = {name for _lib, _nid, name in self._imports(self.threadman)}
        if block is not None:
            names |= {name for _lib, _nid, name in self._imports(self._block_text(block))}
        return names

    @staticmethod
    def _family_span(text: str, macro: str) -> tuple[int, int]:
        # Each family section opens right after its separator comment; the header
        # #if chain that names the same macro does not, so it is never matched.
        match = re.search(rf"\*/\n#if PSP_ORACLE_CASE == {macro}\n", text)
        if match is None:
            raise AssertionError(f"no family section for {macro}")
        end_marker = f"#endif /* {macro} */"
        end = text.index(end_marker, match.start()) + len(end_marker)
        return match.start(), end

    def _section(self, macro: str) -> str:
        start, end = self._family_span(self.probe, macro)
        return self.probe[start:end]

    def _shared_section(self) -> str:
        """Probe text outside every family section (common helpers and main)."""
        text = self.probe
        for _case, _cid, _stem, _block, macro, _test in reversed(self.FAMILIES):
            start, end = self._family_span(text, macro)
            text = text[:start] + text[end:]
        return text

    # ---- Makefile wiring ---------------------------------------------------------

    def test_case_ids_are_registered_once_and_match_the_families(self) -> None:
        pairs = re.findall(r"else ifeq \(\$\(CASE\),(hle-[a-z-]+)\)\nCASE_ID = (\d+)",
                           self.makefile)
        self.assertEqual(sorted(pairs),
                         sorted((case, str(case_id)) for case, case_id, *_ in self.FAMILIES))
        all_ids = re.findall(r"^CASE_ID = (\d+)$", self.makefile, re.MULTILINE)
        self.assertEqual(len(all_ids), len(set(all_ids)), "duplicate CASE_ID in Makefile")

    def test_each_family_links_the_probe_threadman_block_and_only_its_own_block(self) -> None:
        for case, _cid, stem, block, _macro, _test in self.FAMILIES:
            with self.subTest(case=case):
                match = re.search(
                    rf"else ifeq \(\$\(CASE\),{case}\)\nTARGET = \$\(BUILD_DIR\)/{stem}\n"
                    r"OBJS = (.*)\n",
                    self.makefile)
                self.assertIsNotNone(match, f"no TARGET/OBJS branch for {case}")
                expected = ["$(BUILD_DIR)/probe_hle_measure.o",
                            "$(BUILD_DIR)/threadman_user_imports.o"]
                if block is not None:
                    expected.append("$(BUILD_DIR)/" + block.replace(".S", ".o"))
                self.assertEqual(match.group(1).split(), expected)

    def test_module_stop_is_exported_for_the_probe_object(self) -> None:
        self.assertRegex(
            self.makefile,
            r"ifneq \(\$\(filter \$\(BUILD_DIR\)/probe_hle_measure\.o,\$\(OBJS\)\),\)\n"
            r"PRX_EXPORTS = \$\(BUILD_DIR\)/probe_exports\.exp\nendif\n",
        )
        self.assertIn("module_stop", self.probe)

    def test_hle_sources_have_compile_rules(self) -> None:
        for block in self.PINNED_BLOCKS:
            with self.subTest(block=block):
                self.assertIn(f"$(BUILD_DIR)/{block.replace('.S', '.o')}: {block}\n",
                              self.makefile)
        self.assertIn("$(BUILD_DIR)/probe_hle_measure.o: probe_hle_measure.c\n", self.makefile)

    # ---- import tables -----------------------------------------------------------

    def test_pinned_import_blocks_match_declared_nids_and_the_name_derivation(self) -> None:
        for block, (library, flags, pins) in self.PINNED_BLOCKS.items():
            with self.subTest(block=block):
                text = self._block_text(block)
                self.assertEqual(
                    re.findall(r'IMPORT_START\s+"([^"]+)",\s*0x([0-9A-Fa-f]+)', text),
                    [(library, f"{flags:08X}")])
                declared = self._imports(text)
                self.assertEqual({name: nid for _lib, nid, name in declared}, pins)
                for lib, nid, name in declared:
                    self.assertEqual(lib, library)
                    self.assertEqual(nid, self._nid(name), name)

    def test_threadman_block_declares_the_shared_reference_and_refer_status(self) -> None:
        self.assertEqual(
            [nid for _lib, nid, name in self._imports(self.threadman)
             if name == "sceKernelReferSystemStatus"],
            [0x627E6F3A],
        )
        self.assertEqual(self._nid("sceKernelReferSystemStatus"), 0x627E6F3A)

    def test_every_stub_the_probe_calls_is_declared_by_its_own_prx_import_tables(self) -> None:
        shared_defined = set(re.findall(r"^(?:int|float|void)\s+(sce\w+)\s*\(",
                                        self.probe, re.MULTILINE))
        shared_calls = set(re.findall(r"\b(sce\w+)\s*\(", self._shared_section()))
        self.assertEqual(
            sorted(shared_calls - self._declared_names(None) - self.SDK_DEFAULT_CALLS
                   - shared_defined),
            [],
        )
        for case, _cid, _stem, block, macro, _test in self.FAMILIES:
            with self.subTest(case=case):
                section = self._section(macro)
                called = set(re.findall(r"\b(sce\w+)\s*\(", section))
                defined = set(re.findall(r"^(?:int|float|void)\s+(sce\w+)\s*\(", section,
                                         re.MULTILINE))
                undeclared = sorted(called - self._declared_names(block)
                                    - self.SDK_DEFAULT_CALLS - defined)
                self.assertEqual(undeclared, [], f"{block} does not declare {undeclared}")

    def test_each_family_table_passes_the_user_mode_gate_and_names_only_user_libraries(self) -> None:
        for case, _cid, _stem, block, _macro, _test in self.FAMILIES:
            with self.subTest(case=case):
                tables: dict[str, list[int]] = {
                    "ThreadManForUser": [nid for _lib, nid, _name in self._imports(self.threadman)],
                }
                if block is not None:
                    library, _flags, _pins = self.PINNED_BLOCKS[block]
                    tables[library] = [nid for _lib, nid, _name in
                                       self._imports(self._block_text(block))]
                result = user_mode_imports.check_module(
                    build_import_elf(sorted(tables.items())))
                self.assertTrue(result.passed, result)
                self.assertFalse(result.kernel_mode)
                self.assertEqual(set(result.libraries), set(tables))

    def test_built_probe_import_tables_pass_the_gate_when_a_local_build_exists(self) -> None:
        build = self.fixture / "build"
        present = [build / f"{stem}.elf" for _c, _i, stem, _b, _m, _t in self.FAMILIES
                   if (build / f"{stem}.elf").is_file()]
        if not present:
            self.skipTest("no local hle_*.elf build; make CASE=<case> produces one to check")
        for path in present:
            with self.subTest(elf=path.name):
                result = user_mode_imports.check_module(path.read_bytes())
                self.assertTrue(result.passed)
                self.assertIn("ThreadManForUser", result.libraries)

    def test_no_stub_names_a_set_install_or_remove_function_except_the_restored_ge_width(
            self) -> None:
        forbidden = re.compile(r"sce\w*(?:Set|Install|Remove|Flush|Register|Reset)\w*")
        names = set(re.findall(r"\b(sce\w+)\b", self.probe))
        self.assertEqual({name for name in names if forbidden.fullmatch(name)},
                         {"sceGeEdramSetAddrTranslation"})
        for _library, _flags, pins in self.PINNED_BLOCKS.values():
            for name in pins:
                if name != "sceGeEdramSetAddrTranslation":
                    self.assertIsNone(forbidden.fullmatch(name), name)

    # ---- probe source rules ------------------------------------------------------

    def test_every_measured_call_follows_a_step_marker_within_three_lines(self) -> None:
        lines = self.probe.splitlines()
        checked = 0
        for index, line in enumerate(lines):
            if not self.MEASURED_CALL_RE.search(line):
                continue
            if self.PROTOTYPE_LINE_RE.fullmatch(line):
                continue  # a whole-line prototype, not a call
            checked += 1
            window = lines[max(0, index - 3):index]
            self.assertTrue(any("hle_step(" in previous for previous in window),
                            f"line {index + 1} has no hle_step before it: {line.strip()}")
        self.assertGreaterEqual(checked, 40)

    def test_every_loop_is_bounded_except_the_module_stop_park(self) -> None:
        self.assertEqual(self.probe.count("while ("), 2)
        self.assertIn("while (!s_hle_stop_requested)", self.probe)
        self.assertIn("while (offset < length)", self.probe)
        self.assertIn("for (uint32_t i = 0; i < HLE_POLL_ITERATIONS; i++) {", self.probe)
        self.assertRegex(self.probe, r"#define HLE_POLL_ITERATIONS 200u")
        self.assertRegex(self.probe, r"#define HLE_POLL_DELAY_US 10000u")
        self.assertNotIn("malloc(", self.probe)
        self.assertNotIn("sceKernelAllocPartitionMemory", self.probe)

    def test_string_nickname_bytes_never_reach_a_protocol_record(self) -> None:
        body = self.probe[self.probe.index("static void hle_string_param("):]
        body = body[:body.index("\n}\n")]
        self.assertIn('hle_record(case_id, "PASS", (uint32_t)rc, out, 3);', body)
        self.assertIn("hle_private_nickname_write(buf, nul);", body)
        private = self.probe[self.probe.index("static void hle_private_nickname_write("):]
        private = private[:private.index("\n}\n")]
        self.assertIn("host0:/hle_sysparam_private_strings.txt", private)
        self.assertNotIn("hle_record", private)
        self.assertNotIn("hle_emit", private)

    def test_probe_emits_the_protocol_lines_and_the_log_the_runner_reads(self) -> None:
        for marker in ("NAKAGAWA_PSP_META schema=1", "NAKAGAWA_PSP_STEP schema=1 case_id=",
                       "NAKAGAWA_PSP_TEST schema=1 test_id=",
                       "NAKAGAWA_PSP_COMPLETE schema=1 status="):
            self.assertIn(marker, self.probe)
        registrations = {
            case: (test_id, log) for case, test_id, log in re.findall(
                r'#define HLE_CAMPAIGN_ID "(hle-[a-z-]+)"\n#define HLE_TEST_ID "([A-Z0-9-]+)"\n'
                r'#define HLE_LOG "host0:/([a-z0-9_]+\.txt)"', self.probe)
        }
        self.assertEqual(len(registrations), 7)
        for case, _cid, _stem, _block, _macro, test_id in self.FAMILIES:
            with self.subTest(case=case):
                self.assertEqual(registrations[case][0], test_id)
                self.assertEqual(registrations[case][1],
                                 _campaign_host0_log_path(Path("/scratch"), case).name)

    # ---- host0 round trip ---------------------------------------------------------

    @staticmethod
    def _function_body(text: str, signature: str) -> str:
        """A function body with its whitespace collapsed, so line wrapping does not matter."""
        start = text.index(signature)
        return " ".join(text[start:text.index("\n}\n", start)].split())

    def test_every_family_finishes_with_the_round_trip_file_probe_c_writes(self) -> None:
        """The runner's _verify_host0_roundtrip fails a case whose round-trip file is absent.

        probe.c's teardown writes host0:/nakagawa_transport_write.bin and folds the result
        into its COMPLETE status. The HLE families have no teardown of their own, so
        hle_finish runs the same round trip once, in the shared section, after every
        measurement. probe.c is a separate translation unit, so both copies are pinned here.
        """
        probe_c = (self.fixture / "probe.c").read_text(encoding="utf-8")
        probe_path = re.search(r'#define PROBE_HOST0_ROUNDTRIP_PATH "([^"]+)"', probe_c)
        hle_path = re.search(r'#define HLE_ROUNDTRIP_PATH "([^"]+)"', self.probe)
        self.assertIsNotNone(probe_path)
        self.assertIsNotNone(hle_path)
        self.assertEqual(probe_path.group(1), "host0:/nakagawa_transport_write.bin")
        self.assertEqual(hle_path.group(1), probe_path.group(1))
        self.assertIn("#define HLE_ROUNDTRIP_BYTES 64u", self.probe)
        self.assertIn("static int hle_host0_roundtrip(void)", self._shared_section())

        probe_body = self._function_body(probe_c, "static int probe_host0_roundtrip(void)")
        hle_body = self._function_body(self.probe, "static int hle_host0_roundtrip(void)")
        for statement in (
            "for (size_t i = 0; i < sizeof(expected); i++) {",
            "expected[i] = (uint8_t)(0x5Au ^ (i * 0x25u + (i >> 3)));",
            "PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);",
            "written = sceIoWrite(fd, expected, (SceSize)sizeof(expected));",
            "written != (int)sizeof(expected) || write_close < 0",
            "PSP_O_RDONLY, 0);",
            "read = sceIoRead(fd, observed, (SceSize)sizeof(observed));",
            "read == (int)sizeof(observed) && read_close >= 0",
            "memcmp(expected, observed, sizeof(expected)) == 0",
        ):
            with self.subTest(statement=statement):
                self.assertIn(statement, probe_body)
                self.assertIn(statement, hle_body)

        finish = self._function_body(self.probe, "static void hle_finish(int ok)")
        self.assertIn("hle_host0_roundtrip()", finish)
        self.assertIn('(ok && !s_log_failed && roundtrip) ? "PASS" : "FAIL"', finish)
        main = self._function_body(self.probe, "int main(int argc, char **argv)")
        self.assertLess(main.index("hle_run()"), main.index("hle_finish(ok)"))

    # ---- registry, manifest, and the parser contract -----------------------------

    def test_each_family_is_a_fixed_shape_strict_stream_in_the_registry(self) -> None:
        for case, _cid, _stem, _block, _macro, test_id in self.FAMILIES:
            with self.subTest(case=case):
                spec, counts = CAMPAIGN_PROBE_CASES[case]
                self.assertEqual(spec.test_id, test_id)
                self.assertEqual(spec.terminal_case, f"{case}-done")
                self.assertEqual(_campaign_completeness_contract(case), "strict-golden-sequence")
                self.assertEqual(set(counts), set(spec.ordered_cases))
                self.assertEqual(counts[spec.terminal_case], 1)
                for semantic in spec.semantic_cases:
                    self.assertIn(f'"{semantic}"', self.probe, semantic)

    def test_manifest_entries_are_captured_and_cite_the_registry(self) -> None:
        """The 2026-10-10 single-case runs captured every family without acceptance."""
        by_id = {test["id"]: test for test in self.manifest["tests"]}
        for case, _cid, stem, _block, _macro, test_id in self.FAMILIES:
            with self.subTest(case=case):
                entry = by_id[test_id]
                spec, _counts = CAMPAIGN_PROBE_CASES[case]
                self.assertEqual(entry["hardware_evidence"], "CAPTURED")
                self.assertIn("not acceptance-eligible", entry["evidence_note"])
                self.assertIn("establishes nothing", entry["evidence_note"])
                self.assertEqual(entry["status"], "implemented")
                self.assertEqual(entry["source"], "fixtures/psp_oracle/probe_hle_measure.c")
                self.assertEqual(entry["prx"], f"{stem}.prx")
                self.assertEqual(entry["case_ids"], list(spec.ordered_cases))
                self.assertEqual(entry["diagnostic_case_ids"], [case])
                self.assertEqual(entry["issues"], [])

    def _probe_calls(self, macro: str) -> set[str]:
        """The sce stubs one family PRX calls: its own section plus the shared probe code.

        The shared code (main, the module lifecycle and the log writer) is linked into
        every family PRX, so its calls are imports of every family. Prototype lines are
        declarations, not calls, and are dropped before the scan.
        """
        text = self._shared_section() + self._section(macro)
        calls = "\n".join(line for line in text.splitlines()
                          if not self.PROTOTYPE_LINE_RE.fullmatch(line))
        return set(re.findall(r"\b(sce\w+)\s*\(", calls))

    def test_manifest_apis_name_every_import_each_family_calls(self) -> None:
        """A family's `apis` are exactly the sce stubs its PRX calls.

        Each import block the family links must be fully called, so a stub it declares
        but never uses cannot hide in the manifest.
        """
        by_id = {test["id"]: test for test in self.manifest["tests"]}
        for case, _cid, _stem, block, macro, test_id in self.FAMILIES:
            with self.subTest(case=case):
                called = self._probe_calls(macro)
                listed = {api for api in by_id[test_id]["apis"] if api.startswith("sce")}
                self.assertEqual(sorted(listed), sorted(called))
                if block is not None:
                    declared = {name for _lib, _nid, name in
                                self._imports(self._block_text(block))}
                    self.assertEqual(sorted(declared - called), [])

    def _stream(self, case: str, overrides: dict | None = None) -> str:
        """A complete synthetic stream: every cell PASS with zero fields unless overridden."""
        spec, counts = CAMPAIGN_PROBE_CASES[case]
        if overrides is None:
            overrides = self.GE_UNSET_OVERRIDES if case == "hle-ge-edram" else {}
        lines = [
            "NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
            "binary_sha256=" + "0" * 64 + " source_commit=" + "b" * 40 + " fixture=hle-measure",
        ]
        for semantic in spec.semantic_cases:
            lines.append(f"NAKAGAWA_PSP_STEP schema=1 case_id={case} step={semantic}")
            status, result, outs = overrides.get(semantic, ("PASS", 0, None))
            outs = list(outs) if outs is not None else [0] * counts[semantic]
            fields = " ".join(f"out{i}=0x{value:x}" for i, value in enumerate(outs))
            lines.append(f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} case_id={semantic} "
                         f"status={status} result=0x{result:x} {fields}".rstrip())
        lines.append(f"NAKAGAWA_PSP_TEST schema=1 test_id={spec.test_id} "
                     f"case_id={spec.terminal_case} status=PASS result=0x0 "
                     f"out0=0x{len(spec.semantic_cases):x}")
        lines.append("NAKAGAWA_PSP_COMPLETE schema=1 status=PASS")
        return "\n".join(lines) + "\n"

    @staticmethod
    def _body(text: str) -> str:
        """The record stream without its COMPLETE sentinel, as the runner parses it."""
        return text.split("NAKAGAWA_PSP_COMPLETE")[0]

    def test_parser_accepts_each_complete_stream_and_rejects_shape_damage(self) -> None:
        for case, *_rest in self.FAMILIES:
            with self.subTest(case=case):
                text = self._stream(case)
                self.assertTrue(parse_campaign_probe_output(self._body(text), case).complete)
                self.assertTrue(_campaign_stream_complete(text, case))
                self.assertEqual(parse_probe_completion_sentinel(text), "PASS")
                spec, _counts = CAMPAIGN_PROBE_CASES[case]
                truncated = "\n".join(text.splitlines()[:-3]) + "\n"
                self.assertFalse(_campaign_stream_complete(truncated, case))
                with self.assertRaises(ProtocolError):
                    parse_campaign_probe_output(self._body(truncated), case)
                damaged = text.replace(f"case_id={spec.semantic_cases[0]} ",
                                       f"case_id={spec.semantic_cases[0]}x ", 1)
                self.assertFalse(_campaign_stream_complete(damaged, case))
                foreign = text.replace(f"test_id={spec.test_id}", "test_id=PSP-OTHER-001", 1)
                with self.assertRaises(ProtocolError):
                    parse_campaign_probe_output(self._body(foreign), case)

    def test_ge_restore_invariant_accepts_a_restored_width_and_rejects_a_changed_one(self) -> None:
        case = "hle-ge-edram"
        # Initial width 0x200 under set-returning-previous: each Set returns the 0 that
        # the preceding Set(0) left, the restore returns 0 with out0 0x200 read back by
        # Set(0x200), and the final Set(0x200) returns the restored 0x200.
        restored = {
            "edram-width-query-initial": ("PASS", 0x200, None),
            "edram-width-set-512": ("PASS", 0x0, [0x200]),
            "edram-width-set-1024": ("PASS", 0x0, [0x400]),
            "edram-width-set-2048": ("PASS", 0x0, [0x800]),
            "edram-width-set-4096": ("PASS", 0x0, [0x1000]),
            "edram-width-restore": ("PASS", 0x0, [0x200]),
            "edram-width-query-final": ("PASS", 0x200, [0x200]),
        }
        validate_hle_edram_restore(self._body(self._stream(case, restored)))
        self.assertIsNone(_validate_campaign_contract(self._body(self._stream(case, restored)), case))

        wrong_restore = dict(restored)
        wrong_restore["edram-width-restore"] = ("PASS", 0x0, [0x400])
        with self.assertRaises(ProtocolError):
            validate_hle_edram_restore(self._body(self._stream(case, wrong_restore)))
        with self.assertRaises(ProtocolError):
            _validate_campaign_contract(self._body(self._stream(case, wrong_restore)), case)

        changed_final = dict(restored)
        # A final read of 0 is what the previous probe revision produced: its verifying
        # Set(0) undid the restore. The parser must refuse it.
        changed_final["edram-width-query-final"] = ("PASS", 0x0, [0x200])
        with self.assertRaises(ProtocolError):
            validate_hle_edram_restore(self._body(self._stream(case, changed_final)))

    def test_measured_ge_sequence_is_accepted_under_set_returning_previous(self) -> None:
        case = "hle-ge-edram"
        body = self._body(self._stream(case, self.GE_MEASURED_OVERRIDES))
        validate_hle_edram_restore(body)
        self.assertIsNone(_validate_campaign_contract(body, case))

    def test_final_read_not_returning_the_restored_width_is_rejected_by_the_rule(self) -> None:
        """The final read is Set(initial): a 0 (the previous probe revision's destructive
        Set(0) verify, which undid the restore) or another width is refuted."""
        case = "hle-ge-edram"
        for final in (0x0, 0x200):
            with self.subTest(final=hex(final)):
                overrides = dict(self.GE_MEASURED_OVERRIDES)
                overrides["edram-width-query-final"] = ("PASS", final, [0x400])
                with self.assertRaisesRegex(ProtocolError, "set-returning-previous"):
                    validate_hle_edram_restore(self._body(self._stream(case, overrides)))

    def test_ge_width_cells_skip_when_the_original_width_is_not_restorable(self) -> None:
        case = "hle-ge-edram"
        validate_hle_edram_restore(self._body(self._stream(case, self.GE_UNSET_OVERRIDES)))
        ran_anyway = dict(self.GE_UNSET_OVERRIDES)
        ran_anyway["edram-width-set-512"] = ("PASS", 0x0, [0x200])
        with self.assertRaises(ProtocolError):
            validate_hle_edram_restore(self._body(self._stream(case, ran_anyway)))


if __name__ == "__main__":
    unittest.main()


class RegistryProbeNeverWritesTests(unittest.TestCase):
    """The registry oracle must be read-only on real firmware.

    On PSP-3000 6.6.1 sceRegOpenCategory on a category that does not exist creates it and
    persists it to flash even in mode 1, and stored category names are cut to 26 bytes so a
    long name cannot be reopened (a reopen then creates another copy). These checks pin the
    source-level rules that keep the probe from issuing such an implicit write.
    """

    PROBE = Path(__file__).resolve().parent.parent / "fixtures" / "psp_oracle" / "probe.c"
    WRITE_APIS = ("sceRegSetKeyValue", "sceRegCreateKey", "sceRegRemoveCategory",
                  "sceRegRemoveRegistry", "sceRegFlushRegistry", "sceRegFlushCategory")

    def setUp(self) -> None:
        self.source = self.PROBE.read_text(encoding="utf-8")

    def test_no_registry_write_api_is_called(self) -> None:
        for api in self.WRITE_APIS:
            self.assertNotRegex(self.source, rf"\b{api}\s*\(", api)

    def test_open_category_literals_name_only_the_config_root(self) -> None:
        literals = re.findall(r'sceRegOpenCategory\s*\([^,]+,\s*"([^"]*)"', self.source)
        self.assertEqual(sorted(set(literals)), ["/CONFIG"], literals)

    def test_walk_descends_only_into_reopenable_categories(self) -> None:
        self.assertIn("registry_category_reopenable(name)", self.source)
        self.assertRegex(self.source, r"#define REGISTRY_SAFE_NAME_MAX 26u")
        self.assertNotIn("__NAKAGAWA_ORACLE_UNKNOWN_CATEGORY__", self.source)
