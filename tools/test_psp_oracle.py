# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_manifest
import scratch_dir
from import_fixtures import (
    INTERLEAVED_NIDS,
    INTERLEAVED_SHAPE,
    build_import_elf,
    build_interleaved_import_elf,
)
from psp_import_table import UNATTRIBUTED_LIBRARY
from psp_oracle import user_mode_imports
from psp_oracle import parse_golden
from psp_oracle.protocol import (
    ProtocolError,
    STATUSES,
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
    VFPU_TRANSCENDENTAL_OPS,
    VFPU_TRANSCENDENTAL_SEMANTIC_CASES,
    parse_audio_query_output,
    parse_campaign_probe_output,
    parse_delay_zero_output,
    parse_dmac_cells_output,
    parse_dmac_invalid_tail_output,
    parse_font_files_output,
    parse_font_list_output,
    parse_ge_nan_output,
    parse_hle_intr_cells_output,
    parse_registry_font_output,
    parse_registry_readonly_output,
    parse_umd_io_output,
    UMDIO_DOPENS,
    UMDIO_OPENS,
    UMDRAW_CASES,
    UMDRAW_HEADER_READ_SIZES,
    UMDRAW_IOFILEMGR_NIDS,
    UMDRAW_RAW_FLAGS,
    UMDRAW_READONLY_FLAGS,
    UMDRAW_READ_SIZES,
    parse_umd_raw_read_output,
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

# PSP-VRAM-MIRROR-001 measured rules, from the PSP-3000 runs on 2026-10-16
# (commits 24c1472e, fd3efc64, and df41dba2). These are algorithmic constants;
# tests synthesize their own result words and contain no captured PSP words.
MEASURED_VRAM_IDENTITY_COLUMNS = (None, None) + tuple(
    1 << bit for bit in range(2, 21)
)
MEASURED_VRAM_ROTATED_COLUMNS = (None, None) + tuple(
    1 << (bit + 1) if 5 <= bit <= 8 else 1 << 5 if bit == 9 else 1 << bit
    for bit in range(2, 21)
)
MEASURED_VRAM_ALIAS_RULES = {
    0x04000000: (0x000000, MEASURED_VRAM_IDENTITY_COLUMNS),
    0x04200000: (0x002040, MEASURED_VRAM_IDENTITY_COLUMNS),
    0x04400000: (0x000000, MEASURED_VRAM_IDENTITY_COLUMNS),
    0x04600000: (0x002040, MEASURED_VRAM_ROTATED_COLUMNS),
    0x44000000: (0x000000, MEASURED_VRAM_IDENTITY_COLUMNS),
    0x44200000: (0x002040, MEASURED_VRAM_IDENTITY_COLUMNS),
    0x44400000: (0x000000, MEASURED_VRAM_IDENTITY_COLUMNS),
    0x44600000: (0x002040, MEASURED_VRAM_ROTATED_COLUMNS),
}
MEASURED_VRAM_DEPTH_BYTE_XOR = 0x002040
# Flat Z and rectangles aligned to eight pixels leave byte XOR phases 0x2..0xE
# observationally equivalent; preserve the reported mask as a candidate and test
# that the analyzer exposes the geometry's remaining ambiguity.
MEASURED_VRAM_DEPTH_XOR_CANDIDATES = tuple(
    MEASURED_VRAM_DEPTH_BYTE_XOR | low_xor
    for low_xor in (0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE)
)
MEASURED_VRAM_DEPTH_PHASE_AMBIGUITY = 0x00000E
MEASURED_VRAM_DEPTH_ZBP = 0x000CC000
MEASURED_VRAM_DEPTH_SCOPE_AMBIGUOUS = True


def _apply_measured_vram_alias_rule(alias_base: int, offset: int) -> int:
    constant, columns = MEASURED_VRAM_ALIAS_RULES[alias_base]
    physical = constant | (offset & 3)
    for bit in range(2, 21):
        if offset & (1 << bit):
            column = columns[bit]
            if column is None:
                raise AssertionError(f"missing synthetic column for input bit {bit}")
            physical ^= column
    return physical


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
                return_value=(True, "HELD_AND_QUALIFIED", {"boot_number": 42}),
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

    def test_smoke_compile_recipe_has_no_blanket_warning_suppression(self) -> None:
        # A bare -w hides every warning from the smoke compile, including the
        # ATRAC3P sources it builds in-line. Narrow -Wno-<warning> flags stay
        # allowed because each one names the single warning it hides.
        lines = (Path(__file__).resolve().parents[1] / "Makefile").read_text(
            encoding="utf-8"
        ).splitlines()
        rules = [
            index for index, line in enumerate(lines)
            if line.startswith("$(PSP_ORACLE_SMOKE_EXE):")
        ]
        self.assertEqual(len(rules), 1)
        recipe: list[str] = []
        for line in lines[rules[0] + 1:]:
            if not line.startswith("\t"):
                break
            recipe.append(line)
        tokens = " ".join(recipe).replace("\\", " ").split()
        self.assertIn("-DSR_PSP_ORACLE_SMOKE", tokens)
        self.assertNotIn("-w", tokens)

    def test_selftest_defines_profile_hooks_once_for_the_smoke_build(self) -> None:
        # The smoke define used to add a second sr_profile_block/g_prof_enabled pair
        # on top of the unconditional ones, which broke the smoke compile.
        source = (
            Path(__file__).resolve().parents[1] / "src" / "rt" / "hle_thread_selftest.c"
        ).read_text(encoding="utf-8")
        self.assertEqual(
            source.count("void sr_profile_block(uint32_t target_pc) { (void)target_pc; }"), 1
        )
        self.assertEqual(source.count("int g_prof_enabled;"), 1)

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

    # Synthetic copies shaped like the recorded PSP-3000 `thlist` captures: 17
    # rows, system names replaced by placeholders, UIDs invented. Not device output.
    RECORDED_SHAPE_ROWS = frozenset(
        [(f"0x00a0{number:04x}", f"sys-thread-{number:02d}") for number in range(15)]
        + [("0x04310001", "PspLink"), ("0x04310002", "USBThread")]
    )
    PROBE_MODULE = ("0x04280001", "NAKAGAWA_PSP_ORACLE")
    PROBE_MAIN = ("0x04280002", "user_main")
    # Stand-in row for the host0 driver thread until hardware names it.
    DRIVER_ROW = ("0x00000004", "host0-driver")

    @staticmethod
    def thlist(rows) -> str:
        return f"<Thread List ({len(rows)} entries)>\n" + "".join(
            f"UID: {uid} - Name: {name}\n" for uid, name in sorted(rows)
        )

    def probe_triplet(self, baseline_threads, *, unload_threads=None):
        before = self.snapshot(threads=baseline_threads)
        after_probe = self.snapshot(
            threads=set(baseline_threads) | {self.PROBE_MAIN},
            modules={("0x00000003", "PspLink"), self.PROBE_MODULE},
        )
        after_unload = self.snapshot(
            threads=baseline_threads if unload_threads is None else unload_threads
        )
        return before, after_probe, after_unload

    def evaluate(self, before, after_probe, after_unload):
        return evaluate_teardown_snapshots(
            before, after_probe, after_unload, self.PROBE_MODULE[0], {self.PROBE_MAIN},
            unload_confirmed=True, sentinel_status="PASS", shell_qualified=True,
            host0_roundtrip=True,
        )

    def test_recorded_shape_thread_lists_parse_and_probe_main_is_the_only_delta(self) -> None:
        s0 = parse_psplink_thread_snapshot(self.thlist(self.RECORDED_SHAPE_ROWS))
        s1 = parse_psplink_thread_snapshot(
            self.thlist(self.RECORDED_SHAPE_ROWS | {self.PROBE_MAIN})
        )
        self.assertEqual(len(s0), 17)
        self.assertEqual(s1 - s0, {self.PROBE_MAIN})
        self.assertEqual(s0 - s1, set())

    def test_unwarmed_baseline_reproduces_the_recorded_teardown_failure(self) -> None:
        # The recorded transport-write run read S0 before the driver existed:
        # S0 17, S1 18, S2 18, and the verifier reported this exact issue.
        before, after_probe, after_unload = self.probe_triplet(
            self.RECORDED_SHAPE_ROWS,
            unload_threads=self.RECORDED_SHAPE_ROWS | {self.DRIVER_ROW},
        )
        report = self.evaluate(before, after_probe, after_unload)
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["issues"], ["post-unload thread set differs from S0"])
        self.assertEqual(report["s0_thread_count"], 17)
        self.assertEqual(report["s2_thread_count"], 18)

    def test_warmed_baseline_passes_when_the_driver_thread_survives_unload(self) -> None:
        warmed = self.RECORDED_SHAPE_ROWS | {self.DRIVER_ROW}
        report = self.evaluate(*self.probe_triplet(warmed))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["issues"], [])
        self.assertEqual((report["s0_thread_count"], report["s2_thread_count"]), (18, 18))

    def test_warmed_baseline_still_fails_on_a_genuine_post_unload_leak(self) -> None:
        warmed = self.RECORDED_SHAPE_ROWS | {self.DRIVER_ROW}
        leaked = ("0x04290009", "stray-thread")
        report = self.evaluate(*self.probe_triplet(warmed, unload_threads=warmed | {leaked}))
        self.assertEqual(report["status"], "FAIL")
        self.assertIn("post-unload thread set differs from S0", report["issues"])
        self.assertNotIn(
            "probe main thread survived module stop/unload; "
            "the probe's module_stop did not end and delete it",
            report["issues"],
        )
        self.assertEqual(report["s2_leftover_threads"], [list(leaked)])

    def test_baseline_warms_host0_driver_before_reading_s0_and_rereads_threads(self) -> None:
        runner = PsplinkCampaignRunner(
            SimpleNamespace(), console_model="synthetic", source_commit="0" * 40
        )
        warmed = self.thlist(self.RECORDED_SHAPE_ROWS | {self.DRIVER_ROW})
        outputs = [
            (0, "", "", "PROCESS_EXITED"),
            (0, warmed, "", "PROCESS_EXITED"),
            (0, self.MEMORY, "", "PROCESS_EXITED"),
            (0, self.MODULES, "", "PROCESS_EXITED"),
            (0, warmed, "", "PROCESS_EXITED"),
        ]
        with patch.object(runner, "_request", side_effect=outputs) as request:
            snapshot, problem = runner._take_baseline()
        self.assertIsNone(problem)
        self.assertIsNotNone(snapshot)
        self.assertEqual(snapshot.threads, self.RECORDED_SHAPE_ROWS | {self.DRIVER_ROW})
        self.assertEqual(
            [entry.args[0] for entry in request.call_args_list],
            ["ls host0:/", "thlist", "meminfo", "modlist", "thlist"],
        )

    def test_baseline_refuses_when_the_host0_warm_up_fails(self) -> None:
        runner = PsplinkCampaignRunner(
            SimpleNamespace(), console_model="synthetic", source_commit="0" * 40
        )
        with patch.object(
            runner, "_request", side_effect=[(None, "", "", "TIMEOUT")]
        ) as request:
            snapshot, problem = runner._take_baseline()
        self.assertIsNone(snapshot)
        self.assertIn("warm-up", problem)
        self.assertEqual(request.call_count, 1)

    def test_baseline_refuses_a_thread_set_that_moves_after_warm_up(self) -> None:
        runner = PsplinkCampaignRunner(
            SimpleNamespace(), console_model="synthetic", source_commit="0" * 40
        )
        warmed_rows = self.RECORDED_SHAPE_ROWS | {self.DRIVER_ROW}
        moved_rows = (warmed_rows - {("0x04310002", "USBThread")}) | {
            ("0x04310009", "USBThread")
        }
        outputs = [
            (0, "", "", "PROCESS_EXITED"),
            (0, self.thlist(warmed_rows), "", "PROCESS_EXITED"),
            (0, self.MEMORY, "", "PROCESS_EXITED"),
            (0, self.MODULES, "", "PROCESS_EXITED"),
            (0, self.thlist(moved_rows), "", "PROCESS_EXITED"),
        ]
        with patch.object(runner, "_request", side_effect=outputs) as request:
            snapshot, problem = runner._take_baseline()
        self.assertIsNone(snapshot)
        self.assertIn("not stable", problem)
        self.assertEqual(request.call_count, 5)

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
        with scratch_dir.TemporaryDirectory(prefix="interrupt-resume-host-", dir=fixture) as scratch:
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
        with scratch_dir.TemporaryDirectory(prefix="child-teardown-host-", dir=fixture) as scratch:
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
        # Sequential probe.c and HLE cases use distinct numbered ranges. The count and
        # the id set are exact: a probe PR bumps both, and an accidental removal fails here.
        self.assertEqual(len(routes), 105)
        names = [name for name, _ in routes]
        ids = [int(case_id) for _, case_id in routes]
        self.assertEqual(len(names), len(set(names)))
        self.assertEqual(len(ids), len(set(ids)))
        self.assertEqual(set(range(1, 78)) | set(range(90, 104)) | {106, 107, 108, 109}
                         | {110, 111, 112, 113, 114, 115, 116, 117, 118, 119}, set(ids))
        self.assertNotIn("psp_b1_imports.S", self.makefile)
        self.assertNotIn("psp_b2_imports.S", self.makefile)
        self.assertNotIn("psp_b3_imports.S", self.makefile)

    def test_vfpu_compare_route_has_its_own_case_id(self) -> None:
        # Each probe added after 67 also pins its own route here, so a wrong id names the
        # case rather than only failing the shared count above.
        routes = dict(re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        ))
        self.assertEqual(routes.get("vfpu-compare"), "68")
        self.assertEqual(routes.get("sas-init"), "117")

    def test_thread_k0_block_route_has_its_own_case_id(self) -> None:
        routes = dict(re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        ))
        self.assertEqual(routes.get("thread-k0-block"), "116")

    def test_mutex_import_block_is_limited_to_mutex_cases(self) -> None:
        self.assertIn("OBJS = $(BUILD_DIR)/probe.o\n", self.makefile)
        self.assertIn("MUTEX_CASES = mutex-refer-unlocked mutex-timeout-quanta", self.makefile)
        self.assertIn("mutex-priority-inheritance mutex-interrupt-context", self.makefile)
        self.assertIn("mutex-interrupt-context cpu-timing", self.makefile)
        self.assertIn("ifneq ($(filter $(MUTEX_CASES),$(CASE)),)\n", self.makefile)
        self.assertIn("OBJS += $(BUILD_DIR)/mutex_imports.o\n", self.makefile)
        self.assertNotIn(
            "OBJS = $(BUILD_DIR)/probe.o $(BUILD_DIR)/mutex_imports.o",
            self.makefile,
        )

    def test_cpu_timing_imports_only_the_required_kernel_library_nids(self) -> None:
        self.assertIn("ifeq ($(CASE),cpu-timing)", self.makefile)
        self.assertIn(
            "Kernel_Library=0x092968F4,0x5F10D406,0x47A0B729,0xB55249D2,"
            "0xBEA46419,0x15B6446B",
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


    def test_llsc_link_route_has_its_own_case_id(self) -> None:
        routes = dict(re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        ))
        self.assertEqual(routes.get("llsc-link"), "69")

    def test_fpu_cc_route_has_its_own_case_id(self) -> None:
        routes = dict(re.findall(
            r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
            self.makefile,
            re.MULTILINE,
        ))
        self.assertEqual(routes.get("fpu-cc"), "70")
        probe = (Path(__file__).resolve().parent.parent / "fixtures" / "psp_oracle"
                 / "probe.c").read_text(encoding="utf-8")
        self.assertIn("#define PSP_ORACLE_CASE_FPU_CC 70", probe)

    def test_every_linked_object_depends_on_the_selected_build_configuration(self) -> None:
        # Objects are compiled with case-selected flags (-DPSP_ORACLE_CASE and the build
        # commit among them), so a new CASE must rebuild them; without this dependency
        # `make CASE=registry-font` then `make CASE=font-list` linked the registry case's
        # probe.o. The stamp records the case and every flag it selects.
        self.assertIn(
            "BUILD_CONFIG = CASE=$(CASE) CFLAGS=$(CFLAGS) LIBS=$(LIBS) SFOFLAGS=$(SFOFLAGS)\n",
            self.makefile,
        )
        self.assertIn("BUILD_CONFIG_STAMP = $(BUILD_DIR)/build-config.stamp\n", self.makefile)
        self.assertIn("EXTRA_CLEAN += $(BUILD_CONFIG_STAMP)\n", self.makefile)
        # Rule targets expand where they are read, so the dependency must follow the last
        # OBJS assignment to name every object the selected case links.
        dependency = "\n$(OBJS) $(PSP_EBOOT_SFO): $(BUILD_CONFIG_STAMP)\n"
        self.assertIn(dependency, self.makefile)
        last_objs = max(
            match.start()
            for match in re.finditer(r"^OBJS \+?= ", self.makefile, re.MULTILINE)
        )
        self.assertGreater(self.makefile.index(dependency), last_objs)
        # The stamp is a prerequisite of that one line only: never of the ELF, whose SDK
        # link rule passes $^ to the linker.
        self.assertEqual(
            re.findall(r"^[^#\t\n][^\n]*:[^\n=]*\$\(BUILD_CONFIG_STAMP\)", self.makefile, re.MULTILINE),
            [dependency.strip()],
        )
        # FORCE runs the recipe on every make, but the stamp is replaced (and its time
        # moved) only when the recorded configuration differs from the stored one.
        rule = self.makefile.split("$(BUILD_CONFIG_STAMP): FORCE\n", 1)[1].split("\n.PHONY: FORCE\nFORCE:\n", 1)
        self.assertEqual(len(rule), 2, "the stamp rule must be followed by the phony FORCE target")
        recipe = rule[0]
        self.assertIn("'$(subst ','\\'',$(BUILD_CONFIG))' > $@.tmp", recipe)
        self.assertIn("if cmp -s $@.tmp $@; then rm -f $@.tmp;", recipe)
        self.assertIn("mv -f $@.tmp $@; fi", recipe)
        self.assertLess(
            self.makefile.index("include $(PSPSDK)/lib/build.mak"),
            self.makefile.index("$(BUILD_CONFIG_STAMP): FORCE"),
            "SFOFLAGS and the SDK's CFLAGS additions are defined by build.mak",
        )

    def test_switching_case_without_clean_rebuilds_the_probe_object(self) -> None:
        # Runs the real Makefile under GNU Make with a stub SDK and a stand-in compiler that
        # writes the case macro into the object. The throwaway repository holds one commit
        # so the clean-tree guard passes, and git ignores the host's configuration.
        if sys.platform == "win32":
            self.skipTest("the oracle Makefile's recipes need a POSIX shell")
        make = shutil.which("make")
        if make is None or shutil.which("git") is None or shutil.which("cmp") is None:
            self.skipTest("GNU Make, git and cmp are unavailable")
        with scratch_dir.TemporaryDirectory(prefix="oracle-case-stamp-") as scratch:
            root = Path(scratch)
            repo = root / "repo"
            repo.mkdir()
            dated = time.time() - 3600
            # The Makefile and every source and header it may name as a prerequisite.
            sources = [self.fixture / "Makefile"]
            sources += sorted(p for p in self.fixture.iterdir() if p.suffix in {".c", ".h", ".S"})
            for source in sources:
                shutil.copyfile(source, repo / source.name)
                os.utime(repo / source.name, (dated, dated))
            (root / "sdk" / "lib").mkdir(parents=True)
            (root / "sdk" / "lib" / "build.mak").write_text(
                "# Stub SDK: the test names the object it builds.\n", encoding="utf-8"
            )
            compiler = root / "stand_in_cc.py"
            compiler.write_text(
                "import sys\n"
                "args = sys.argv[1:]\n"
                "case = [arg for arg in args if arg.startswith('-DPSP_ORACLE_CASE=')]\n"
                "with open(args[args.index('-o') + 1], 'w') as out:\n"
                "    out.write(' '.join(case) + '\\n')\n"
                "with open(sys.argv[0] + '.log', 'a') as log:\n"
                "    log.write('compiled\\n')\n",
                encoding="utf-8",
            )
            env = {
                **os.environ,
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_CONFIG_NOSYSTEM": "1",
                "GIT_AUTHOR_NAME": "oracle-case-stamp test",
                "GIT_AUTHOR_EMAIL": "oracle-case-stamp@example.invalid",
                "GIT_COMMITTER_NAME": "oracle-case-stamp test",
                "GIT_COMMITTER_EMAIL": "oracle-case-stamp@example.invalid",
            }
            for git_args in (("init", "-q"), ("add", "--all"),
                             ("commit", "-q", "-m", "fixture")):
                subprocess.run(["git", *git_args], cwd=repo, env=env, check=True,
                               capture_output=True)
            probe_object = root / "build" / "probe.o"
            stamp = root / "build" / "build-config.stamp"
            log = Path(f"{compiler}.log")

            def build(case: str) -> tuple[str, int]:
                # Give the object and the stamp one earlier time (still after probe.c), so
                # the object rebuilds only if this make rewrites the stamp, and a coarse
                # file clock cannot hide a rewrite by tying the two.
                for path in (probe_object, stamp):
                    if path.exists():
                        os.utime(path, (dated + 60, dated + 60))
                completed = subprocess.run(
                    [make, "-C", str(repo), f"CASE={case}", f"PSPSDK={root / 'sdk'}",
                     f"BUILD_DIR={root / 'build'}",
                     f"CC={shlex.join([sys.executable, str(compiler)])}", str(probe_object)],
                    env=env, capture_output=True, text=True, check=False,
                )
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                return (probe_object.read_text(encoding="utf-8"),
                        log.read_text(encoding="utf-8").count("compiled\n"))

            self.assertEqual(build("smoke"), ("-DPSP_ORACLE_CASE=0\n", 1))
            # An unchanged configuration leaves the stamp, and so the object, alone.
            self.assertEqual(build("smoke"), ("-DPSP_ORACLE_CASE=0\n", 1))
            # A new case rewrites the stamp and recompiles; before the stamp existed this
            # make kept the smoke object.
            self.assertEqual(build("callback-notify-check"), ("-DPSP_ORACLE_CASE=1\n", 2))
            self.assertEqual(build("smoke"), ("-DPSP_ORACLE_CASE=0\n", 3))


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

    def test_cpu_timing_bounds_blocking_ge_calls_and_stalled_queue_occupancy(self) -> None:
        self.assertIn("#define CPU_TIMING_DRAW_SYNC_CALLS 4u", self.probe)
        self.assertIn("s_cpu_timing_drawsync_job.calls = CPU_TIMING_DRAW_SYNC_CALLS;", self.probe)
        self.assertIn("const uint32_t calls = CPU_TIMING_DRAW_SYNC_CALLS;", self.probe)
        self.assertIn("#define CPU_TIMING_GE_CALLS_PER_TRIAL 1u", self.probe)
        self.assertIn("CPU_TIMING_GE_CALLS_PER_TRIAL, &error", self.probe)
        self.assertIn("sceGuStart(GU_SEND, s_cpu_timing_ge_list);", self.probe)
        self.assertIn("s_cpu_timing_ge_list_bytes = (uint32_t)list_bytes;", self.probe)
        self.assertIn("(uintptr_t)s_cpu_timing_ge_list +", self.probe)
        self.assertIn("(uint32_t)list_bytes <= sizeof(s_cpu_timing_ge_list)", self.probe)

    def test_cpu_timing_tears_down_libpspgu_before_recording_the_ge_cells(self) -> None:
        bodies = dict(_c_function_list(_c_code(self.probe)))
        ge = bodies["cpu_timing_measure_ge_calls"]
        teardown = ge.index("const GeGuTeardown gu_down = ge_gu_teardown(gu_rc, -1);")
        self.assertLess(ge.index("sceGuInit()"), teardown)
        self.assertLess(teardown, ge.index("cpu_timing_emit_ge_cell("))
        self.assertLess(teardown, ge.index('"cpu-timing-gu-teardown"'))
        # Every path emits the teardown row, so the stream keeps its shape.
        self.assertIn("cpu_timing_emit_gu_teardown_skip(emulated);\n        return 0;", ge)
        self.assertIn("cpu_timing_emit_gu_teardown_skip(emulated);", bodies["run_cpu_timing"])
        self.assertIn("#define CPU_TIMING_RESULT_COUNT 65u", self.probe)
        mutant = self.probe.replace(
            "    const int gu_rc = sceGuInit();\n",
            "    const int gu_rc = sceGuInit();\n    if (gu_rc < 0) {\n        return 0;\n    }\n", 1)
        self.assertNotEqual(mutant, self.probe)
        self.assertNotEqual(_gu_teardown_problems(mutant), [])

    def test_cpu_timing_displays_its_own_framebuffer_when_none_is_displayed(self) -> None:
        # 2026-10-27 (boot 47, standalone PSPLink): sceDisplayGetFrameBuf reported no
        # framebuffer, so the display cell was SKIP and the capture was not eligible.
        body = dict(_c_function_list(_c_code(self.probe)))["run_cpu_timing"]
        order = [
            body.index("sceDisplayGetFrameBuf("),
            body.index("if (frame_rc >= 0 && s_cpu_timing_framebuffer == NULL) {"),
            body.index("s_cpu_timing_framebuffer = sceGeEdramGetAddr();"),
            body.index("cpu_timing_call_setframebuf, CPU_TIMING_SYSCALL_CALLS"),
            body.index("sceDisplaySetFrameBuf(NULL, frame_found_stride, frame_found_format,"),
            body.index('"cpu-timing-framebuffer"'),
        ]
        self.assertEqual(order, sorted(order))
        self.assertIn("if (frame_owned) {", body)
        # Changing the stride or format with IMMEDIATE returns 0x80000107 (measured on boot
        # 48), so both changes are NEXTFRAME followed by the latching vblank.
        own_set = body[order[2]:order[3]]
        self.assertIn("PSP_DISPLAY_SETBUF_NEXTFRAME);", own_set)
        self.assertIn("sceDisplayWaitVblankStart();", own_set)
        self.assertNotIn("PSP_DISPLAY_SETBUF_IMMEDIATE", own_set)
        restore = body[order[4]:order[5]]
        self.assertIn("PSP_DISPLAY_SETBUF_NEXTFRAME);", restore)
        self.assertIn("sceDisplayWaitVblankStart();", restore)
        spec, counts = CAMPAIGN_PROBE_CASES["cpu-timing"]
        cases = list(spec.ordered_cases)
        self.assertEqual(cases.index("cpu-timing-framebuffer"),
                         cases.index("syscall-display-setframebuf") + 1)
        self.assertEqual(counts["cpu-timing-framebuffer"], 5)

    def test_cpu_timing_manifest_row_is_measured_and_cites_every_record(self) -> None:
        manifest = json.loads((self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
            encoding="utf-8"))
        row = next(test for test in manifest["tests"] if test["id"] == "PSP-CPU-TIMING-001")
        spec, _counts = CAMPAIGN_PROBE_CASES["cpu-timing"]
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(row["case_ids"], list(spec.ordered_cases))
        self.assertEqual(row["evidence_cases"], row["case_ids"])
        self.assertEqual(row["evidence_ref"],
                         "docs/HARDWARE_ORACLE.md#psp-cpu-timing-001-measured-run-2026-10-27")
        self.assertIn("3a14a88e", row["measurement_note"])
        # Every record is named in the cited section; the APIs are listed but not cited,
        # so the row grants no HLE evidence tier.
        section = hle_manifest.measured_evidence_section(row)
        self.assertIn("`3a14a88e`", section)
        self.assertFalse(set(row["apis"]) & set(row["evidence_cases"]))

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
        self.assertEqual(system["hardware_evidence"], "MEASURED")
        self.assertEqual(system["case_ids"], ["model-profile"])
        self.assertEqual(system["evidence_cases"][0], "model-profile")

    def test_implemented_status_alone_is_not_accepted_hardware_evidence(self) -> None:
        """`status: implemented` is probe-source prose, never hardware evidence."""
        manifest = self._oracle_manifest()
        oracle_apis = hle_manifest.oracle_exercised_apis(manifest)
        for test_id in ("PSP-SYSTEM-001",):
            entry = next(item for item in manifest["tests"] if item["id"] == test_id)
            self.assertEqual(entry["status"], "implemented")
            self.assertEqual(entry["hardware_evidence"], "MEASURED")
            # Status alone grants nothing: an API is granted only when the cited run names it.
            for api in entry["apis"]:
                self.assertEqual(
                    api in oracle_apis,
                    api in entry["evidence_cases"],
                    f"{api} tier must follow the cited run, not the probe source",
                )

    def test_fpu_and_cache_probe_families_are_measured_by_the_campaign(self) -> None:
        """`status` stays probe-source prose; the hardware tier comes from the cited run."""
        manifest = self._oracle_manifest()
        by_id = {entry["id"]: entry for entry in manifest["tests"]}
        for test_id in ("PSP-FPU-001", "PSP-CACHE-001"):
            self.assertEqual(
                by_id[test_id]["status"],
                "implemented" if test_id == "PSP-CACHE-001" else "planned",
            )
            self.assertEqual(by_id[test_id]["hardware_evidence"], "MEASURED")
            self.assertEqual(
                by_id[test_id]["evidence_ref"],
                "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize",
            )
            self.assertEqual(by_id[test_id]["evidence_cases"], by_id[test_id]["case_ids"])
            self.assertIn("f6ccfb33", by_id[test_id]["measurement_note"])
        self.assertEqual(len(by_id), len(manifest["tests"]))

    def test_issue_311_rescoped_groups_read_implemented(self) -> None:
        """#311 re-scoped its remaining groups to PSP-AUDIO-001 and PSP-CACHE-001.

        Each probe source is complete: a strict stream parser owns its whole case list, the
        probe flushes that stream under the row's id, and the Makefile builds its campaign case.
        Each case is cited from an acceptance-eligible run, so the row reads `implemented`.
        `status` still grants nothing; the tier stays on `hardware_evidence`. The cache row's
        runtime boundary is named in the measured index, not left implied.
        """
        by_id = {entry["id"]: entry for entry in self._oracle_manifest()["tests"]}
        for test_id, spec, campaign_case in (
            ("PSP-AUDIO-001", AUDIO_SPEC, "audio-query"),
            ("PSP-CACHE-001", parse_golden.CACHE_SPEC, "cache-alias"),
        ):
            with self.subTest(test_id=test_id):
                entry = by_id[test_id]
                self.assertEqual(spec.test_id, test_id)
                self.assertEqual(entry["case_ids"], list(spec.ordered_cases))
                self.assertEqual(entry["evidence_cases"], entry["case_ids"])
                self.assertNotIn("uncited_cases", entry)
                self.assertEqual(entry["hardware_evidence"], "MEASURED")
                self.assert_probe_contains(f'flush_deferred(emulated, "{test_id}");')
                self.assertIn(f"else ifeq ($(CASE),{campaign_case})", self.makefile)
                self.assertEqual(entry["status"], "implemented")
        doc = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        self.assertIn("Runtime boundary (`PSP-CACHE-001`)", doc)
        self.assertIn("coherent", doc[doc.index("Runtime boundary (`PSP-CACHE-001`)"):][:600])

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
        # on f6ccfb33 measured eighteen others and captured two without acceptance, and the
        # 2026-10-15 runs of build e45e667b measured the HLE single-case probes and the three
        # CPU-semantics probes.
        self.assertEqual(
            {test_id for test_id, value in evidence.items() if value == "MEASURED"},
            {
                "PSP-DMAC-001", "PSP-DISPLAY-001", "PSP-EXCEPTION-001", "PSP-KERNEL-002",
                "PSP-TRANSPORT-001", "PSP-ALARM-001", "PSP-THREAD-003", "PSP-WAIT-001",
                "PSP-KERNEL-STATUS-001", "PSP-REGISTRY-001", "PSP-KERNEL-MISC-001", "PSP-SMOKE-001",
                "PSP-THREAD-EXIT-001", "PSP-IO-001", "PSP-UMDIO-001",
                "PSP-DISPLAY-002", "PSP-DISPLAY-003",
                "PSP-DISPLAY-004", "PSP-FPU-001", "PSP-CACHE-001", "PSP-AUDIO-001", "PSP-GE-001",
                "PSP-MUTEX-001", "PSP-KERNEL-001", "PSP-SYSTEM-001",
                # The 2026-10-15 single-case runs on e45e667b were acceptance-eligible.
                "PSP-HLE-KERNEL-STATUS-001", "PSP-HLE-VTIMER-001", "PSP-HLE-POWER-001",
                "PSP-HLE-HPRM-001", "PSP-HLE-CTRL-LATCH-001", "PSP-HLE-SYSPARAM-001",
                "PSP-HLE-GE-EDRAM-001",
                # The 2026-10-15 CPU-semantics runs of build e45e667b.
                "PSP-FPU-CC-001", "PSP-VFPU-CMP-001", "PSP-LLSC-001",
                # The 2026-10-15 re-run of the revised ge-break-continue probe (c642039a).
                "PSP-GE-CONTROL-001",
                # The 2026-10-15 single-case runs of build b5d6fc5b (#1020 cases).
                "PSP-HLE-VTIMER-002", "PSP-HLE-VTIMER-003", "PSP-HLE-POWER-002",
                # The 2026-10-15 single-case runs of the #1037 probes: the VFPU corpus and the
                # timer-order case from 22b027f6, the audio SRC case from 7fc1641e.
                "PSP-VFPU-TRANSCENDENTAL-001", "PSP-HLE-TIMER-ORDER-001",
                "PSP-HLE-AUDIO-SRC-001",
                # The 2026-10-16 acceptance-eligible PSP-3000 VMIR3 run.
                "PSP-VRAM-MIRROR-001",
                # The 2026-10-27 acceptance-eligible ge-timing runs of 8f707fc0 (#1296).
                "PSP-GE-TIMING-001",
                # The 2026-10-27 acceptance-eligible cpu-timing run of 3a14a88e (#1296).
                "PSP-CPU-TIMING-001",
                # The 2026-10-28 acceptance-eligible vfpu-forms run of 781ccd34 (#326).
                "PSP-VFPU-FORMS-001",
                # The 2026-10-29 acceptance-eligible vfpu-funnel run of 8d0f75ad (#326).
                "PSP-VFPU-FUNNEL-001",
                # The 2026-10-28 acceptance-eligible ge-sample run of 86e55611 (#282).
                "PSP-GE-SAMPLE-001",
                # The 2026-10-29 acceptance-eligible insn-trace run of 84568280 (#312).
                "PSP-TRACE-001",
            },
        )
        self.assertEqual(
            {test_id for test_id, value in evidence.items() if value == "CAPTURED"},
            {
                # Captured only: the runner classified its envelope as not acceptance-eligible.
                "PSP-TEARDOWN-001",
                # b5d6fc5b, 2026-10-15: an incomplete NULL-pointer stream. (The pool-bounds
                # capture of the same day belongs to a revision since replaced; NOT_RUN.)
                "PSP-HLE-NULL-OUT-001",
                # 2026-10-17: raw UMD capture stopped before the 32768-byte seek/read result.
                "PSP-UMDRAW-001",
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
        self.assertIn("mbx-delete-wait", kernel["case_ids"])
        self.assertIn("mbx-timeout-control", kernel["case_ids"])
        self.assertEqual(kernel["hardware_evidence"], "MEASURED")
        self.assertEqual(
            kernel["uncited_cases"],
            ["callback-notify-check", "wait-cancel", "thread-lifecycle"],
        )
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
            "PSP-VRAM-MIRROR-001": (parse_golden.VRAM_MIRROR_SPEC, "vram-mirror"),
        }
        for test_id, (spec, campaign_case) in expected.items():
            with self.subTest(test_id=test_id):
                entry = by_id[test_id]
                self.assertIn(campaign_case, entry.get("diagnostic_case_ids", []))
                if spec is not None:
                    self.assertEqual(entry["case_ids"], list(spec.ordered_cases))
                if test_id in {"PSP-AUDIO-001", "PSP-GE-001"}:
                    self.assertEqual(
                        entry["status"],
                        "implemented" if test_id == "PSP-AUDIO-001" else "planned",
                    )
                    self.assertEqual(entry["hardware_evidence"], "MEASURED")
                    self.assertEqual(entry["evidence_cases"], entry["case_ids"])
        for case_id in ("audio-query", "ge-nan", "dma-cells", "delay-zero",
                        "vram-mirror"):
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
        with scratch_dir.TemporaryDirectory(prefix="mbx-cleanup-host-", dir=fixture) as scratch:
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
                 "REFER_STATUS_SIZE", "REGISTRY_READONLY", "KERNEL_MISC", "VFPU_COMPARE",
                 "CPU_TIMING")

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

    def test_ge_break_continue_resets_before_any_dequeue(self) -> None:
        # The reset cell is separable only if the sceGeBreak(1) reset cell comes before every
        # sceGeListDeQueue cell, and the cancelled-list cells are read after that reset.
        spec, _ = CAMPAIGN_PROBE_CASES["ge-break-continue"]
        order = spec.ordered_cases
        reset = order.index("ge-break-reset-no-dequeue")
        cancelled = order.index("ge-list-sync-cancelled")
        dequeue = order.index("ge-list-dequeue-stalled")
        self.assertLess(reset, cancelled)
        self.assertLess(cancelled, dequeue)

    def test_ge_break_continue_busy_dequeue_is_a_pass_cell_that_gates_nothing(self) -> None:
        # The stalled-list dequeue result is a measured cell (the eligible 2026-10-15 re-run
        # read BUSY 0x80000021). Its status is PASS whatever it
        # returns, so the cancelled cells and the envelope stay eligible.
        rows = self._campaign_rows("ge-break-continue")
        busy = [
            row.replace("status=PASS result=0x0", "status=PASS result=0x80000021", 1)
            if "case_id=ge-list-dequeue-stalled " in row else row
            for row in rows
        ]
        report = parse_campaign_probe_output(self._stream(busy), "ge-break-continue")
        statuses = {record.case_id: record.status for record in report.results.values()}
        self.assertEqual(set(statuses.values()), {"PASS"})

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
        # A blocking sync is allowed only where it is the measurement (ge-timing's C2/C6,
        # cpu-timing's blocking sceGeDrawSync(0) cell): inside a helper-thread entry whose
        # creator waits for it with a timeout and terminates it when the wait fails, so a
        # sync that never returns costs one cell.
        for source in sorted(self.fixture.glob("*.c")):
            text = re.sub(r"/\*.*?\*/|//[^\n]*", "", source.read_text(encoding="utf-8"),
                          flags=re.DOTALL)
            for call in re.finditer(r"sceGeListSync\([^;]*,\s*0\s*\)|sceGeDrawSync\(\s*0\s*\)", text):
                head = text.rfind("\nstatic ", 0, call.start())
                entry = re.match(r"\nstatic int (\w+)\(SceSize \w+, void \*\w+\) \{",
                                 text[head:text.index("{", head) + 1])
                self.assertIsNotNone(entry, f"{source.name}: {call.group(0)} outside a helper thread")
                create = re.search(r"sceKernelCreateThread\([^;]*\b%s\b" % entry.group(1), text)
                self.assertIsNotNone(create, f"{source.name}: {entry.group(1)} is never started")
                caller_head = text.rfind("\nstatic ", 0, create.start())
                caller = text[caller_head:text.index("\n}\n", create.start())]
                self.assertRegex(caller, r"sceKernelWaitThreadEnd\(\s*\w+\s*,\s*&\w+\s*\)", source.name)
                self.assertIn("sceKernelTerminateDeleteThread(", caller, source.name)
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


def _c_function_list(code: str) -> list[tuple[str, str]]:
    """(name, body with braces) of every top-level function definition, in source order.

    A list rather than a mapping: different case blocks of probe.c may reuse a name.
    """
    found = []
    for match in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}]*)\)\s*\{",
                             code, re.MULTILINE):
        if match.group(1) in _C_NOT_CALLS:
            continue
        brace = code.index("{", match.end() - 1)
        found.append((match.group(1), code[brace:_c_close(code, brace) + 1]))
    return found


def _c_depth(body: str, at: int) -> int:
    """Brace depth at index `at` of a function body that starts at its opening brace."""
    text = re.sub(r'"(?:[^"\\]|\\.)*"', '""', body[:at])
    return text.count("{") - text.count("}")


def _gu_teardown_problems(source: str) -> list[str]:
    """Paths of a PSP oracle source that can unload with a GE callback still registered.

    sceGuInit registers libpspgu's GE signal and finish callbacks (PSPSDK src/gu/sceGuInit.c)
    and only sceGuTerm removes them (src/gu/sceGuTerm.c); a callback a case registers itself is
    removed only by sceGeUnsetCallback.  The runner unloads the probe after the case returns, so
    a registration left behind points the next GE interrupt at freed code.  Checked on the
    comment-free text, per function:

    - a function that calls sceGuInit passes its result to ge_gu_teardown in a statement at the
      function's top level, with no return or goto between the two;
    - a function that calls sceGeSetCallback passes that ID to ge_gu_teardown, or to
      sceGeUnsetCallback after a bounded GE wait, the same way;
    - ge_gu_teardown waits for idle with the bound, resets the queues when the GE is still
      busy, unregisters the case's callback, and only then calls sceGuTerm, with no return
      before it; sceGuTerm is called nowhere else.
    """
    code = _c_code(source)
    functions = _c_function_list(code)
    problems: list[str] = []
    if "sceGuInit(" not in code and "sceGeSetCallback(" not in code:
        return problems

    def reaches(name: str, body: str, start: int, pattern: str, what: str) -> int:
        """Index of the first top-level `pattern` after `start` with no exit in between, or -1."""
        for match in re.finditer(pattern, body[start:]):
            at = start + match.start()
            statement = body[max(body.rfind(c, 0, at) for c in ";{}") + 1:at]
            if _c_depth(body, at) != 1 or re.search(r"\b(?:if|else|for|while|do|case)\b|\?", statement):
                continue  # inside a block or a conditional statement: not every path
            if re.search(r"\b(?:return|goto)\b", body[start:at]):
                problems.append(f"{name} can return before {what}")
            return at
        problems.append(f"{name} never reaches {what} at its top level")
        return -1

    term_owners = [name for name, body in functions if "sceGuTerm(" in body]
    if term_owners != ["ge_gu_teardown"]:
        problems.append(f"sceGuTerm is called from {term_owners}, not only from ge_gu_teardown")
    helpers = [body for name, body in functions if name == "ge_gu_teardown"]
    if len(helpers) != 1:
        problems.append(f"{len(helpers)} definitions of ge_gu_teardown, not one")
    else:
        helper = helpers[0]
        steps = [re.search(pattern, helper) for pattern in (
            r"ge_wait_idle\(\s*-1\s*,\s*GE_IDLE_DEADLINE_US\s*\)",
            r"if \(!\w+\.idle\) \{[^{}]*ge_reset_queues\(",
            r"sceGeUnsetCallback\(\s*own_cbid\s*\)",
            r"if \(init_rc >= 0\) \{\s*sceGuTerm\(\);",
        )]
        if not all(steps):
            problems.append("ge_gu_teardown lacks the bounded wait, the reset, the callback "
                            "unregistration or the guarded sceGuTerm")
        elif [step.start() for step in steps] != sorted(step.start() for step in steps):
            problems.append("ge_gu_teardown does not wait, reset and unregister before sceGuTerm")
        elif re.search(r"\breturn\b", helper[:steps[-1].start()]):
            problems.append("ge_gu_teardown can return before sceGuTerm")

    for name, body in functions:
        if name == "ge_gu_teardown":
            continue
        for call in re.finditer(r"\b(\w+) = sceGuInit\(\)", body):
            reaches(name, body, call.end(),
                    r"ge_gu_teardown\(\s*%s\s*," % re.escape(call.group(1)),
                    f"ge_gu_teardown({call.group(1)}, ...)")
        if body.count("sceGuInit(") != len(re.findall(r"\b\w+ = sceGuInit\(\)", body)):
            problems.append(f"{name} calls sceGuInit without keeping its result")
        for call in re.finditer(r"\b(\w+) = sceGeSetCallback\(", body):
            cbid = re.escape(call.group(1))
            at = reaches(name, body, call.end(),
                         r"ge_gu_teardown\([^;]*,\s*%s\s*\)|sceGeUnsetCallback\(\s*%s\s*\)" % (cbid, cbid),
                         f"the unregistration of {call.group(1)}")
            if at >= 0 and not body[at:].startswith("ge_gu_teardown(") and \
                    "ge_wait_idle(" not in body[call.end():at]:
                problems.append(f"{name} unregisters {call.group(1)} without a bounded GE wait")
        if body.count("sceGeSetCallback(") != len(re.findall(r"\b\w+ = sceGeSetCallback\(", body)):
            problems.append(f"{name} calls sceGeSetCallback without keeping its ID")
    return problems


class GuTeardownContractTests(unittest.TestCase):
    """#1296: no probe unloads with a libpspgu or probe-owned GE callback still registered."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")

    def test_every_gu_init_and_ge_callback_is_torn_down_on_every_path(self) -> None:
        for source in sorted(self.fixture.glob("*.c")):
            with self.subTest(source=source.name):
                self.assertEqual(_gu_teardown_problems(source.read_text(encoding="utf-8")), [])
        code = _c_code(self.probe)
        owners = {name for name, body in _c_function_list(code) if "sceGuInit(" in body}
        self.assertIn("run_ge_nan", owners)
        self.assertIn("run_vram_mirror", owners)
        self.assertIn("ge_run_case", {name for name, body in _c_function_list(code)
                                      if "sceGeSetCallback(" in body})

    def test_the_teardown_check_refuses_each_leak(self) -> None:
        helper_term = "    if (init_rc >= 0) {\n        sceGuTerm();\n        t.term_called = 1u;\n    }\n"
        helper_wait = "    GeIdleWait w = ge_wait_idle(-1, GE_IDLE_DEADLINE_US);\n"
        nan_init = "    const int init_rc = sceGuInit();\n    s_ge_nan_frame"
        vram_down = "const GeGuTeardown gu_down = ge_gu_teardown(gu_init_rc, -1);"
        mask_unset = "    sceGeUnsetCallback(cbid);\n"
        mask_set = "    const int cbid = sceGeSetCallback(&cb);\n"
        for anchor in (helper_term, helper_wait, nan_init, vram_down, mask_unset, mask_set):
            self.assertEqual(self.probe.count(anchor), 1, anchor)
        mutants = {
            # The shape every sceGuInit case had before #1296 round 3: no teardown at all.
            "no-teardown": re.sub(r"const GeGuTeardown gu_down = ge_gu_teardown\([^;]*\);",
                                  "const GeGuTeardown gu_down = {0};", self.probe),
            "no-term": self.probe.replace(helper_term, ""),
            "term-before-wait": self.probe.replace(helper_term, "").replace(
                helper_wait, helper_term.replace("t.term_called", "unused") + helper_wait),
            "no-reset": self.probe.replace("t.reset_rc = ge_reset_queues(&w);", "", 1),
            "term-elsewhere": self.probe.replace(nan_init, nan_init.replace(
                "sceGuInit();\n", "sceGuInit();\n    sceGuTerm();\n")),
            "early-return": self.probe.replace(nan_init, nan_init.replace(
                "sceGuInit();\n", "sceGuInit();\n    if (init_rc < 0) {\n        return;\n    }\n")),
            "result-dropped": self.probe.replace(nan_init, nan_init.replace(
                "const int init_rc = sceGuInit();", "const int init_rc = 0;\n    sceGuInit();")),
            "conditional-teardown": self.probe.replace(
                vram_down, "GeGuTeardown gu_down = {0};\n    if (clear.completed)\n        gu_down = "
                "ge_gu_teardown(gu_init_rc, -1);"),
            "nested-teardown": self.probe.replace(
                vram_down, "GeGuTeardown gu_down = {0};\n    if (clear.completed) {\n        gu_down = "
                "ge_gu_teardown(gu_init_rc, -1);\n    }"),
            "callback-kept": self.probe.replace(mask_unset, ""),
            "callback-early-return": self.probe.replace(
                mask_set, mask_set + "    if (cbid < 0) {\n        return;\n    }\n"),
        }
        for name, mutant in mutants.items():
            with self.subTest(mutant=name):
                self.assertNotEqual(mutant, self.probe)
                self.assertNotEqual(_gu_teardown_problems(mutant), [], name)

    def test_ge_nan_and_vram_mirror_record_the_teardown(self) -> None:
        bodies = dict(_c_function_list(_c_code(self.probe)))
        nan = bodies["run_ge_nan"]
        self.assertRegex(nan, r'defer_record\("ge-nan-done", gu_down\.idle \? "PASS" : "TIMEOUT",')
        self.assertLess(nan.index("ge_gu_teardown("), nan.index('"ge-nan-done"'))
        vram = bodies["run_vram_mirror"]
        self.assertIn("const int ge_idle = sprite_c.ge_idle && gu_down.idle;", vram)
        self.assertLess(vram.index("ge_gu_teardown("), vram.index("restore-vram"))

    def test_ge_timing_unregisters_its_callback_through_the_teardown(self) -> None:
        gt_down = "const GeGuTeardown gu_down = ge_gu_teardown(init_rc, s_gt_cbid);"
        self.assertEqual(self.probe.count(gt_down), 1)
        gt = dict(_c_function_list(_c_code(self.probe)))["run_ge_timing"]
        self.assertLess(gt.index("ge_gu_teardown("), gt.index('"gt-gu-teardown"'))
        self.assertLess(gt.index('"gt-gu-teardown"'), gt.index('"gt-done"'))
        self.assertNotIn("sceGeUnsetCallback(", gt)
        for name, mutant in {
            "callback-kept": self.probe.replace(gt_down, gt_down.replace("s_gt_cbid", "-1")),
            "only-after-passes": self.probe.replace(
                gt_down, "GeGuTeardown gu_down = {0};\n    if (passes)\n        gu_down = "
                "ge_gu_teardown(init_rc, s_gt_cbid);"),
        }.items():
            with self.subTest(mutant=name):
                self.assertNotEqual(_gu_teardown_problems(mutant), [], name)


class GeTimingContractTests(unittest.TestCase):
    """#1296: the ge-timing stream has a strict ordered completion contract."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.probe = (self.root / "fixtures" / "psp_oracle" / "probe.c").read_text(encoding="utf-8")
        self.spec, self.counts = CAMPAIGN_PROBE_CASES["ge-timing"]

    def test_contract_is_strict_and_counts_the_records(self) -> None:
        self.assertEqual(_campaign_completeness_contract("ge-timing"),
                         "strict-golden-sequence")
        self.assertEqual(self.spec.test_id, "PSP-GE-TIMING-001")
        self.assertEqual(self.spec.terminal_case, "ge-timing-done")
        # gt-setup, 334 cells per clock pass, the teardown and gt-done.
        self.assertEqual(self.spec.terminal_count, 1 + 2 * 334 + 2)
        self.assertEqual(self.spec.ordered_cases[:2], ("gt-setup", "gt-b111-t0-empty-0"))
        self.assertEqual(self.spec.ordered_cases[-3:], ("gt-gu-teardown", "gt-done", "ge-timing-done"))
        self.assertEqual(
            [case[len("gt-b111-"):] for case in self.spec.ordered_cases if case.startswith("gt-b111-")],
            [case[len("gt-b166-"):] for case in self.spec.ordered_cases if case.startswith("gt-b166-")])

    def test_every_record_is_counted_by_the_completion_record(self) -> None:
        block = self.probe[self.probe.index("#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_GE_TIMING\n/* GE execution"):]
        block = _c_code(block[:block.index("\n#endif")])
        emits = re.findall(r'emit_record_extended\(emulated, "PSP-GE-TIMING-001", ([^,]+),', block)
        self.assertEqual(emits, ["case_id", '"ge-timing-done"'])
        wrapper = dict(_c_function_list(block))["gt_record"]
        self.assertIn("s_gt_records++;", wrapper)
        run = dict(_c_function_list(block))["run_ge_timing"]
        self.assertLess(run.index('gt_record(emulated, "gt-done"'), run.index('"ge-timing-done"'))
        self.assertIn("const uint32_t records = s_gt_records;", run)

    def test_contract_names_only_cells_the_probe_emits(self) -> None:
        self.assertIn('snprintf(s_gt_clk, sizeof(s_gt_clk), "b%u", (unsigned int)bus);', self.probe)
        self.assertIn("gt_pass(emulated, 222, 222, 111);", self.probe)
        self.assertIn("gt_pass(emulated, 333, 333, 166);", self.probe)
        cells = {re.match(r"[a-z]\d+-", case[len("gt-b111-"):]).group(0)
                 for case in self.spec.ordered_cases if case.startswith("gt-b111-")}
        # t0-t15, s1-s4 and the completion cells c13, c2, c4, c5 and c6.
        self.assertEqual(len(cells), 16 + 4 + 5)
        for cell in sorted(cells):
            with self.subTest(cell=cell):
                self.assertRegex(self.probe, r'["-]%s' % re.escape(cell))

    def test_the_measured_order_parses_and_a_reordered_stream_does_not(self) -> None:
        rows = []
        for case_id in self.spec.ordered_cases:
            out = " ".join(f"out{i}=0x00000000" for i in range(self.counts[case_id]))
            if case_id == self.spec.terminal_case:
                out = f"out0=0x{self.spec.terminal_count:08x}"
            rows.append(f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-GE-TIMING-001 case_id={case_id} "
                        f"status=PASS result=0x00000000 {out}\n")
        meta = ("NAKAGAWA_PSP_META schema=1 source=psp model=PSP-3000 firmware=6.6.1 "
                "binary_sha256=" + "0" * 64 + " source_commit=" + "0" * 40 + "\n")
        self.assertTrue(parse_campaign_probe_output(meta + "".join(rows), "ge-timing").complete)
        swapped = rows[:]
        swapped[1], swapped[2] = swapped[2], swapped[1]
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(meta + "".join(swapped), "ge-timing")
        clock_skip = ("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-GE-TIMING-001 case_id=gt-b166-clock "
                      "status=SKIP result=0x80000107 out0=0x80000107 out1=0x000000de out2=0x0000006f\n")
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(meta + "".join(rows[:335]) + clock_skip, "ge-timing",
                                        require_complete=False)

    def test_manifest_row_is_measured_and_cites_every_record(self) -> None:
        manifest = json.loads((self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
            encoding="utf-8"))
        row = next(test for test in manifest["tests"] if test["id"] == "PSP-GE-TIMING-001")
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(row["case_ids"], list(self.spec.ordered_cases))
        self.assertEqual(row["evidence_cases"], row["case_ids"])
        self.assertEqual(row["evidence_ref"],
                         "docs/HARDWARE_ORACLE.md#psp-ge-timing-001-measured-runs-2026-10-27")
        self.assertIn("8f707fc0", row["measurement_note"])
        # The cited section names every record; the probe's APIs are listed but not cited, so
        # this row grants no HLE evidence tier.
        section = hle_manifest.measured_evidence_section(row)
        self.assertIn("`8f707fc0`", section)
        self.assertFalse(set(row["apis"]) & set(row["evidence_cases"]))


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

    A PSP-3000 measured all 45 cells on 2026-10-15 (build e45e667b). These tests pin the
    measured words, the probe's operand table to an independent Python copy of the
    interpreter's lane compares (VFPU3 sub-ops 5, 6 and 7 in src/rt/vfpu_interp.c), the
    agreement of that model with every measured word, the fixed record shape the parser
    registry expects, the emission order, and the case's wiring (Makefile number, host0
    log, queue slot, manifest row, documentation).
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

    # The 45 result words the console returned, per pair: (vscmp.s, vsge.s, vslt.s).
    MEASURED = {
        "lt": (0xBF800000, 0x00000000, 0x3F800000),
        "eq": (0x00000000, 0x3F800000, 0x00000000),
        "gt": (0x3F800000, 0x3F800000, 0x00000000),
        "zero-pos-neg": (0x00000000, 0x3F800000, 0x00000000),
        "zero-neg-pos": (0x00000000, 0x3F800000, 0x00000000),
        "nan-left-quiet": (0x3F800000, 0x00000000, 0x00000000),
        "nan-right-quiet": (0xBF800000, 0x00000000, 0x00000000),
        "nan-left-signal": (0x3F800000, 0x00000000, 0x00000000),
        "nan-right-signal": (0xBF800000, 0x00000000, 0x00000000),
        "nan-left-negative": (0xBF800000, 0x00000000, 0x00000000),
        "inf-pos-neg": (0x3F800000, 0x3F800000, 0x00000000),
        "inf-neg-pos": (0xBF800000, 0x00000000, 0x3F800000),
        "inf-pos-pos": (0x00000000, 0x3F800000, 0x00000000),
        "inf-neg-neg": (0x00000000, 0x3F800000, 0x00000000),
        "inf-pos-finite": (0x3F800000, 0x3F800000, 0x00000000),
    }

    @classmethod
    def model_word(cls, op: str, a: int, b: int) -> int:
        """The project's model for one lane.

        vsge and vslt are IEEE comparisons, so a NaN compares false. vscmp orders a lane
        with a NaN by sign and magnitude of the raw words, as the console does.
        """
        import struct

        x = struct.unpack("<f", struct.pack("<I", a))[0]
        y = struct.unpack("<f", struct.pack("<I", b))[0]
        if op == "vscmp" and ((a & 0x7FFFFFFF) > 0x7F800000 or (b & 0x7FFFFFFF) > 0x7F800000):
            def key(word: int) -> int:
                return -(word & 0x7FFFFFFF) if word >> 31 else word & 0x7FFFFFFF
            value = -1.0 if key(a) < key(b) else (1.0 if key(a) > key(b) else 0.0)
        elif op == "vscmp":
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

    def test_model_returns_every_measured_word(self) -> None:
        # PSP-VFPU-CMP-001: the model the interpreter, the generated code and the probe
        # table carry must be the 45 words the console returned.
        self.assertEqual(tuple(self.MEASURED), tuple(pair[0] for pair in self.PAIRS))
        for name, a, b in self.PAIRS:
            for index, op in enumerate(self.OPS):
                with self.subTest(cell=f"{op}-{name}"):
                    self.assertEqual(self.model_word(op, a, b), self.MEASURED[name][index])

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
        # disagrees with the model must still produce PASS records: the status
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
            "      PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_COMPARE || \\\n"
            "      PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS || \\\n"
            "      PSP_ORACLE_CASE == PSP_ORACLE_CASE_CPU_TIMING\n"
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

    def test_manifest_row_is_measured_and_cites_every_cell(self) -> None:
        manifest = json.loads((self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
            encoding="utf-8"))
        row = next(test for test in manifest["tests"] if test["id"] == self.TEST_ID)
        spec, _counts = CAMPAIGN_PROBE_CASES[self.CASE]
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(row["status"], "implemented")
        self.assertEqual(row["case_ids"], list(spec.ordered_cases))
        self.assertEqual(row["evidence_cases"], row["case_ids"])
        self.assertEqual(
            row["evidence_ref"],
            "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize",
        )
        self.assertIn("e45e667b", row["measurement_note"])
        self.assertNotIn("evidence_note", row)
        # The citation must name every cell verbatim; instruction rows grant no HLE tier.
        section = hle_manifest.measured_evidence_section(row)
        self.assertIn(f"`{self.TEST_ID}`", section)

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
        self.assertIn("| `vfpu-compare` | `PSP-VFPU-CMP-001` | `MEASURED` (2026-10-15, `e45e667b`)",
                      oracle_doc)
        readme = (self.fixture / "README.md").read_text(encoding="utf-8")
        self.assertIn("| `vfpu-compare` | `PSP-VFPU-CMP-001` |", readme)


class VfpuTranscendentalParserFormatTests(unittest.TestCase):
    """Parser/format-only checks for the raw PSP-VFPU-TRANSCENDENTAL-001 corpus.

    The synthetic records use arbitrary words. This class checks how those words are
    mapped to the interpreter's scalar-op family and transport chunks; it does not
    compare a result to a PSP or interpreter value.
    """

    CASE = "vfpu-transcendental-corpus"
    TEST_ID = "PSP-VFPU-TRANSCENDENTAL-001"
    OPS = VFPU_TRANSCENDENTAL_OPS
    CASES = VFPU_TRANSCENDENTAL_SEMANTIC_CASES

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")
        self.makefile = (self.fixture / "Makefile").read_text(encoding="utf-8")
        self.interpreter = (self.root / "src" / "rt" / "vfpu_interp.c").read_text(
            encoding="utf-8")
        self.manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
                encoding="utf-8"))

    def _synthetic_stream(self, first_status: str = "PASS") -> tuple[str, dict[str, list[int]]]:
        spec, _counts = CAMPAIGN_PROBE_CASES[self.CASE]
        rows: list[str] = []
        expected: dict[str, list[int]] = {}
        for op_index, op in enumerate(self.OPS):
            for chunk in range(8):
                case_id = f"{op}-chunk-{chunk:02d}"
                first = chunk * 8
                count = 7 if chunk == 7 else 8
                words = [0xA5000000 + op_index * 0x1000 + first + lane
                         for lane in range(count)]
                expected[case_id] = words
                outs = "".join(f" out{lane}=0x{word:08x}"
                               for lane, word in enumerate(words))
                status = first_status if not rows else "PASS"
                rows.append(
                    f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} "
                    f"case_id={case_id} status={status} result=0x{first:08x}{outs}\n"
                )
        rows.append(
            f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} "
            f"case_id={spec.terminal_case} status=PASS result=0x0 "
            f"out0=0x{spec.terminal_count:x}\n"
        )
        header = META.format(
            source="psp", model="unknown", firmware="unknown",
            binary="0" * 64, commit="1" * 40,
        )
        return header + "".join(rows), expected

    def test_parser_format_preserves_raw_words_and_cross_references_interpreter_ops(self) -> None:
        """Cross-reference labels and field positions only, never numeric semantics."""
        spec, _counts = CAMPAIGN_PROBE_CASES[self.CASE]
        for operation in (
            "sr_vfpu_rcp(v[i])", "sr_vfpu_rsqrt(v[i])", "sr_vfpu_sin(v[i])",
            "sr_vfpu_cos(v[i])", "sr_vfpu_exp2(v[i])", "sr_vfpu_log2(v[i])",
            "sr_vfpu_sqrt(v[i])", "sr_vfpu_asin(v[i])",
        ):
            with self.subTest(operation=operation):
                self.assertIn(operation, self.interpreter)

        text, expected = self._synthetic_stream()
        parsed = parse_campaign_probe_output(text, self.CASE)
        self.assertTrue(parsed.complete)
        self.assertEqual(parsed.record_count, 89)
        self.assertEqual(
            [case for case in parsed.results if case != spec.terminal_case],
            list(self.CASES),
        )
        for case_id, words in expected.items():
            values = dict(parsed.results[case_id].values)
            self.assertEqual(int(values["result"], 0), int(case_id[-2:]) * 8)
            self.assertEqual(
                [int(values[f"out{lane}"], 0) for lane in range(len(words))],
                words,
            )
        # A5-prefixed synthetic words are transport sentinels, not model or hardware
        # expectations. Parsing must retain them without interpretation.
        self.assertEqual(
            int(dict(parsed.results["vsin-chunk-00"].values)["out0"], 0), 0xA5000000
        )

    def test_raw_record_status_and_shape_fail_closed(self) -> None:
        text, _expected = self._synthetic_stream(first_status="FAIL")
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(text, self.CASE)

        text, _expected = self._synthetic_stream()
        malformed = text.replace("out7=0xa5000007", "out6=0xa5000007", 1)
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(malformed, self.CASE)

    def test_constant_grids_are_complete_and_do_not_repeat_measured_vasin_inputs(self) -> None:
        special_match = re.search(
            r"s_vfpu_trans_specials\[VFPU_TRANS_SPECIAL_COUNT\]\s*=\s*\{([^}]*)\}",
            self.probe,
        )
        asin_special_match = re.search(
            r"s_vfpu_trans_asin_specials\[VFPU_TRANS_SPECIAL_COUNT\]\s*=\s*\{([^}]*)\}",
            self.probe,
        )
        self.assertIsNotNone(special_match)
        self.assertIsNotNone(asin_special_match)

        def words(body: str) -> list[int]:
            return [int(word, 16) for word in re.findall(r"0x([0-9a-fA-F]{8})u", body)]

        shared_specials = words(special_match.group(1))
        asin_specials = words(asin_special_match.group(1))
        self.assertEqual(len(shared_specials), 20)
        self.assertEqual(len(asin_specials), 20)
        self.assertTrue({
            0x00000000, 0x80000000, 0x00000001, 0x80000001,
            0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000,
            0x40000000, 0xc0000000, 0x7f800000, 0xff800000,
            0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001,
        }.issubset(shared_specials))

        domains: dict[str, list[int]] = {}
        for name in (
            "angle", "exp", "log", "root", "recip", "asin",
        ):
            match = re.search(
                rf"s_vfpu_trans_{name}_domain\[VFPU_TRANS_DOMAIN_COUNT\]\s*=\s*\{{([^}}]*)\}}",
                self.probe,
            )
            self.assertIsNotNone(match, name)
            domains[name] = words(match.group(1))
            self.assertEqual(len(domains[name]), 43, name)

        old_probe = (self.root / "fixtures" / "vfpu_oracle" / "vfpu_probe.c").read_text(
            encoding="utf-8")
        old_match = re.search(r"VASIN_DOMAIN_INPUTS\[\]\s*=\s*\{([^}]*)\}", old_probe)
        self.assertIsNotNone(old_match)
        measured_vasin_inputs = set(words(old_match.group(1)))
        self.assertFalse(
            measured_vasin_inputs & (set(asin_specials) | set(domains["asin"])),
            "the new VASIN input grid must not repeat its measured edge corpus",
        )
        for op, domain_name in (
            ("vsin", "angle"), ("vcos", "angle"), ("vexp2", "exp"),
            ("vlog2", "log"), ("vrsq", "root"), ("vrcp", "recip"),
            ("vsqrt", "root"), ("vasin", "asin"), ("vnrcp", "recip"),
            ("vnsin", "angle"), ("vrexp2", "exp"),
        ):
            specials = asin_specials if op == "vasin" else shared_specials
            grid = specials + domains[domain_name]
            with self.subTest(operation=op):
                self.assertEqual(len(grid), 63)
                self.assertEqual(len(set(grid)), 63, "input word repeated in its grid")

    def test_case_is_measured_outside_the_queue_and_has_the_full_raw_word_stream(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES[self.CASE]
        row = next(test for test in self.manifest["tests"] if test["id"] == self.TEST_ID)
        self.assertEqual(spec.semantic_cases, self.CASES)
        self.assertEqual(spec.terminal_case, "vfpu-transcendental-corpus-done")
        self.assertEqual(spec.terminal_count, 88)
        self.assertEqual(len(spec.ordered_cases), 89)
        self.assertEqual(set(counts.values()), {1, 7, 8})
        # The 2026-10-15 run of 22b027f6 was complete and acceptance-eligible, so every one of
        # the 89 records is cited; the words themselves stay in the private campaign directory.
        self.assertEqual(row["status"], "implemented")
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(
            row["evidence_ref"],
            "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize")
        self.assertEqual(row["evidence_cases"], list(spec.ordered_cases))
        self.assertNotIn("uncited_cases", row)
        self.assertNotIn("evidence_note", row)
        for word in ("2026-10-15", "22b027f6", "acceptance-eligible with 0 blockers",
                     "88 data records", "693 words", "compares no value"):
            self.assertIn(word, row["measurement_note"])
        self.assertEqual(row["case_ids"], list(spec.ordered_cases))
        self.assertNotIn(self.CASE, CAMPAIGN_QUEUE_CASES)

        self.assertIn("#define PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS 71", self.probe)
        self.assertIn("else ifeq ($(CASE),vfpu-transcendental-corpus)\nCASE_ID = 71\n",
                      self.makefile)
        self.assertIn(
            '#define PROBE_HOST0_LOG "host0:/vfpu_transcendental_corpus_log.txt"',
            self.probe,
        )
        self.assertIn("s_vfpu_transcendental_observed", self.probe)
        self.assertIn("DEFERRED_MAX_RECORDS 128", self.probe)
        start = self.probe.index(
            "#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_VFPU_TRANSCENDENTAL_CORPUS\n")
        end = self.probe.index("\n#endif", start)
        self.assertNotIn("sr_vfpu_", self.probe[start:end])

        hardware = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        readme = (self.fixture / "README.md").read_text(encoding="utf-8")
        self.assertIn("vfpu-transcendental-corpus", hardware)
        self.assertIn("vfpu-transcendental-corpus", readme)


# ---- probe_hle_measure.c analysis: out-word counts per cell -----------------------
#
# A cell's out-word count is the last argument of its hle_record call, or the second
# argument of hle_skip. The cell name and the count reach those calls through helper
# parameters, so the analysis walks hle_run and every helper it calls inside one
# family's #if section, substituting the literal arguments of each call. Both branches
# of a helper are walked. A cell reached twice must carry one count, or the probe is
# refused as inconsistent.

_C_NOT_CALLS = frozenset({"if", "for", "while", "switch", "return", "sizeof", "do"})
_C_CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
_C_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
_C_INT_RE = re.compile(r"(0x[0-9A-Fa-f]+|\d+)[uUlL]*")


def _c_code(text: str) -> str:
    """The C text with its comments removed."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _c_close(text: str, open_at: int) -> int:
    """Index of the bracket that closes the one at open_at; string literals are skipped."""
    closer = {"(": ")", "{": "}", "[": "]"}[text[open_at]]
    opener = text[open_at]
    depth = 0
    i = open_at
    while i < len(text):
        ch = text[i]
        if ch == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif ch == opener:
            depth += 1
        elif ch == closer:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced brackets in the probe source")


def _c_split(text: str) -> list[str]:
    """The top-level comma-separated pieces of an argument list."""
    pieces = []
    depth = 0
    start = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "," and depth == 0:
            pieces.append(text[start:i].strip())
            start = i + 1
        i += 1
    tail = text[start:].strip()
    if tail:
        pieces.append(tail)
    return pieces


def _c_functions(section: str) -> dict[str, tuple[list[str], str]]:
    """Name -> (parameter names, body) for each static function defined in a section."""
    found: dict[str, tuple[list[str], str]] = {}
    for match in re.finditer(r"^static [^\n;{}]*?\b(\w+)\s*\(", section, re.M):
        open_paren = match.end() - 1
        close_paren = _c_close(section, open_paren)
        brace = section.find("{", close_paren)
        if brace < 0 or section[close_paren + 1:brace].strip():
            continue  # a prototype, not a definition
        raw = section[open_paren + 1:close_paren].strip()
        params: list[str] = []
        if raw not in ("", "void"):
            for piece in _c_split(raw):
                named = (re.search(r"\(\*\s*(\w+)\s*\)", piece)
                         or re.search(r"(\w+)\s*(\[[^\]]*\])?\s*$", piece))
                params.append(named.group(1))
        found[match.group(1)] = (params, section[brace + 1:_c_close(section, brace)])
    return found


def _c_value(arg: str, env: dict, defines: dict, arrays: dict):
    """(kind, value): ("str", text), ("strs", [texts]), ("int", n) or ("unknown", text)."""
    arg = arg.strip()
    if _C_STRING_RE.fullmatch(arg):
        return ("str", arg[1:-1])
    if _C_INT_RE.fullmatch(arg):
        return ("int", int(_C_INT_RE.fullmatch(arg).group(1), 0))
    if re.fullmatch(r"\w+", arg):
        if arg in env:
            return env[arg]
        if arg in defines:
            return ("int", defines[arg])
    indexed = re.fullmatch(r"(\w+)\s*\[\s*\w+\s*\]", arg)
    if indexed and indexed.group(1) in arrays:
        return ("strs", arrays[indexed.group(1)])
    cast = re.fullmatch(r"\(\s*\w+\s*\)\s*(.+)", arg)
    if cast:
        return _c_value(cast.group(1), env, defines, arrays)
    return ("unknown", arg)


def _c_walk(name, env, functions, defines, arrays, emit, active=()):
    if name in active:
        raise AssertionError(f"helper {name} calls itself")
    body = functions[name][1]
    for match in _C_CALL_RE.finditer(body):
        callee = match.group(1)
        if callee in _C_NOT_CALLS:
            continue
        close = _c_close(body, match.end() - 1)
        args = _c_split(body[match.end():close])
        if callee == "hle_record" and len(args) == 5:
            emit("record", _c_value(args[0], env, defines, arrays),
                 _c_value(args[4], env, defines, arrays), name)
        elif callee == "hle_skip" and len(args) == 2:
            emit("record", _c_value(args[0], env, defines, arrays),
                 _c_value(args[1], env, defines, arrays), name)
        elif callee == "hle_step":
            emit("step", None, None, name)
        elif callee in functions:
            params = functions[callee][0]
            inner = {p: _c_value(a, env, defines, arrays)
                     for p, a in zip(params, args, strict=False)}
            _c_walk(callee, inner, functions, defines, arrays, emit, active + (name,))


def _probe_cells(probe: str, macro: str) -> tuple[list[tuple[str, int]], int]:
    """The (cell, out-word count) pairs a family's record calls can emit, in source order,
    and the number of hle_step calls the walk reaches. A cell can appear more than once
    when a skip branch and a record branch both emit it."""
    anchor = re.search(rf"\*/\n#if PSP_ORACLE_CASE == {macro}\n", probe)
    if anchor is None:
        raise AssertionError(f"no family section for {macro}")
    end_marker = f"#endif /* {macro} */"
    end = probe.index(end_marker, anchor.start()) + len(end_marker)
    section = _c_code(probe[anchor.start():end])
    code = _c_code(probe)
    defines = {name: int(value, 0) for name, value in re.findall(
        r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)[uUlL]*\s*$", code, re.M)}
    arrays = {name: _C_STRING_RE.findall(body) for name, body in re.findall(
        r"\b(\w+)\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", code)}
    functions = _c_functions(section)
    emitted: list[tuple[str, int]] = []
    steps: list[str] = []

    def emit(kind, case, count, where):
        if kind == "step":
            steps.append(where)
            return
        if case[0] == "str":
            names = [case[1]]
        elif case[0] == "strs":
            names = list(case[1])
        else:
            raise AssertionError(f"{macro}: cell name in {where} is not a literal: {case[1]}")
        if count[0] != "int":
            raise AssertionError(f"{macro}: out-word count in {where} is not a constant: "
                                 f"{count[1]}")
        emitted.extend((name, count[1]) for name in names)

    _c_walk("hle_run", {}, functions, defines, arrays, emit)
    return emitted, len(steps)


def _out_count_mismatches(probe: str, campaign: str, macro: str) -> list[str]:
    """Differences between the counts the probe source yields and the parser registry."""
    spec, registry = CAMPAIGN_PROBE_CASES[campaign]
    cells, _steps = _probe_cells(probe, macro)
    by_name: dict[str, set[int]] = {}
    for name, count in cells:
        by_name.setdefault(name, set()).add(count)
    problems = [f"{name}: conflicting counts {sorted(values)}"
                for name, values in by_name.items() if len(values) != 1]
    derived = {name: next(iter(values)) for name, values in by_name.items() if len(values) == 1}
    expected = {name: count for name, count in registry.items() if name != spec.terminal_case}
    for name in sorted(set(derived) | set(expected)):
        if derived.get(name) != expected.get(name):
            problems.append(f"{name}: source {derived.get(name)} != registry {expected.get(name)}")
    if list(by_name) != list(spec.semantic_cases):
        problems.append("cell order in the source differs from the registry")
    return problems


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
    # The seven single-launch cases in the same file (97-103), added after the families above.
    # They build, link and parse like those families; their pass rule is machinery only. Their
    # recorded hardware outcomes for cases 97-101 are pinned by HleSingleLaunchProbeTests.
    SINGLE_LAUNCH_FAMILIES = (
        ("hle-vtimer-unit", 97, "hle_vtimer_unit", None,
         "HLE_CASE_VTIMER_UNIT", "PSP-HLE-VTIMER-002"),
        ("hle-vtimer-errors", 98, "hle_vtimer_errors", None,
         "HLE_CASE_VTIMER_ERRORS", "PSP-HLE-VTIMER-003"),
        ("hle-kernel-pool-bounds", 99, "hle_kernel_pool_bounds", None,
         "HLE_CASE_KERNEL_POOL_BOUNDS", "PSP-HLE-KERNEL-STATUS-002"),
        ("hle-power-float-v0", 100, "hle_power_float_v0", "hle_power_imports.S",
         "HLE_CASE_POWER_FLOAT_V0", "PSP-HLE-POWER-002"),
        ("hle-null-out-pointers", 101, "hle_null_out_pointers", None,
         "HLE_CASE_NULL_OUT_POINTERS", "PSP-HLE-NULL-OUT-001"),
        ("hle-audio-src", 102, "hle_audio_src", None,
         "HLE_CASE_AUDIO_SRC", "PSP-HLE-AUDIO-SRC-001"),
        ("hle-timer-order", 103, "hle_timer_order", None,
         "HLE_CASE_TIMER_ORDER", "PSP-HLE-TIMER-ORDER-001"),
    )
    ALL_FAMILIES = FAMILIES + SINGLE_LAUNCH_FAMILIES
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
    # Calls supplied by the SDK's default IO/audio libraries, not by the hand-written
    # ThreadMan import block. Libc and CRT helpers are not sce-prefixed.
    SDK_DEFAULT_CALLS = frozenset({
        "sceIoOpen", "sceIoWrite", "sceIoRead", "sceIoClose", "sceIoRemove",
        "sceAudioSRCChReserve", "sceAudioSRCChRelease", "sceAudioSRCOutputBlocking",
        "sceAudioOutput2Reserve", "sceAudioOutput2Release",
    })
    MEASURED_CALL_RE = re.compile(
        r"\b(sceKernel(?:ReferSystemStatus|ReferFplStatus|CreateFpl|TryAllocateFpl|FreeFpl|"
        r"DeleteFpl|CreateVTimer|ReferVTimerStatus|GetVTimerTime|StartVTimer|StopVTimer|"
        r"DeleteVTimer|DelayThread|GetVTimerBase|GetSystemTimeWide|CreateVpl|TryAllocateVpl|"
        r"FreeVpl|DeleteVpl|SetAlarm|CancelAlarm|SetVTimerHandler)|"
        r"sceAudio(?:SRC(?:ChReserve|ChRelease|OutputBlocking)|Output2(?:Reserve|Release))|"
        r"scePowerGet\w+|sceHprmIs\w+|sceCtrl\w*Latch|"
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
        return int.from_bytes(hashlib.sha1(name.encode("ascii"), usedforsecurity=False).digest()[:4], "little")

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
        for _case, _cid, _stem, _block, macro, _test in reversed(self.ALL_FAMILIES):
            start, end = self._family_span(text, macro)
            text = text[:start] + text[end:]
        return text

    # ---- Makefile wiring ---------------------------------------------------------

    def test_case_ids_are_registered_once_and_match_the_families(self) -> None:
        pairs = re.findall(r"else ifeq \(\$\(CASE\),(hle-[a-z0-9-]+)\)\nCASE_ID = (\d+)",
                           self.makefile)
        # The HLE issue cells (106-107) are routed too, from their own source.
        registered = self.ALL_FAMILIES + HleIssuesProbeTests.CASES
        self.assertEqual(sorted(pairs),
                         sorted((case, str(case_id)) for case, case_id, *_ in registered))
        all_ids = re.findall(r"^CASE_ID = (\d+)$", self.makefile, re.MULTILINE)
        self.assertEqual(len(all_ids), len(set(all_ids)), "duplicate CASE_ID in Makefile")

    def test_each_family_links_the_probe_threadman_block_and_only_its_own_block(self) -> None:
        for case, _cid, stem, block, _macro, _test in self.ALL_FAMILIES:
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
        for case, _cid, _stem, block, macro, _test in self.ALL_FAMILIES:
            with self.subTest(case=case):
                section = self._section(macro)
                called = self._calls_in(section)
                defined = set(re.findall(r"^(?:int|float|void)\s+(sce\w+)\s*\(", section,
                                         re.MULTILINE))
                undeclared = sorted(called - self._declared_names(block)
                                    - self.SDK_DEFAULT_CALLS - defined)
                self.assertEqual(undeclared, [], f"{block} does not declare {undeclared}")

    def test_each_family_table_passes_the_user_mode_gate_and_names_only_user_libraries(self) -> None:
        for case, _cid, _stem, block, _macro, _test in self.ALL_FAMILIES:
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
        present = [build / f"{stem}.elf" for _c, _i, stem, _b, _m, _t in self.ALL_FAMILIES
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
        # sceIoRemove deletes only the sysparam family's own private host0 file (#1072 M4);
        # it changes no console state.
        self.assertEqual({name for name in names if forbidden.fullmatch(name)},
                         {"sceGeEdramSetAddrTranslation", "sceKernelSetAlarm",
                          "sceKernelSetVTimerHandler", "sceIoRemove"})
        for _library, _flags, pins in self.PINNED_BLOCKS.values():
            for name in pins:
                if name != "sceGeEdramSetAddrTranslation":
                    self.assertIsNone(forbidden.fullmatch(name), name)

    # ---- probe source rules ------------------------------------------------------

    def test_every_measured_call_follows_a_step_marker_within_three_lines(self) -> None:
        lines = self.probe.splitlines()
        callback_start = self.probe.index("static void hle_timer_order_capture(")
        callback_end = self.probe.index("\n}\n", callback_start)
        callback_clock_line = "const uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();"
        checked = 0
        for index, line in enumerate(lines):
            if not self.MEASURED_CALL_RE.search(line):
                continue
            if self.PROTOTYPE_LINE_RE.fullmatch(line):
                continue  # a whole-line prototype, not a call
            if callback_start <= self.probe.find(line) < callback_end and callback_clock_line in line:
                continue  # the callback samples time; host logging is unsafe there
            checked += 1
            window = lines[max(0, index - 3):index]
            self.assertTrue(any("hle_step(" in previous for previous in window),
                            f"line {index + 1} has no hle_step before it: {line.strip()}")
        self.assertGreaterEqual(checked, 40)

    def test_timer_callback_samples_only_the_clock_and_owned_capture(self) -> None:
        start = self.probe.index("static void hle_timer_order_capture(")
        body = self.probe[start:self.probe.index("\n}\n", start)]
        self.assertIn("sceKernelGetSystemTimeWide()", body)
        self.assertIn("capture->sequence = sequence;", body)
        self.assertNotIn("hle_step(", body)
        self.assertNotIn("hle_write(", body)
        self.assertNotIn("hle_record(", body)

    def test_new_probes_use_only_probe_owned_pointers_and_finite_waits(self) -> None:
        audio = self._section("HLE_CASE_AUDIO_SRC")
        timer = self._section("HLE_CASE_TIMER_ORDER")
        for section in (audio, timer):
            with self.subTest(section=section[:36]):
                self.assertEqual(
                    set(re.findall(r"\b(sce\w+)\s*\([^;]*\bNULL\b", section)), set()
                )
        self.assertIn("static int16_t s_hle_asrc_silence", audio)
        self.assertIn("__attribute__((aligned(64)))", audio)
        self.assertIn("memset(s_hle_asrc_silence, 0, sizeof(s_hle_asrc_silence));", audio)
        self.assertIn("sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_hle_asrc_silence)",
                      audio)
        self.assertIn("&s_hle_vtimer_options", timer)
        self.assertIn("&s_hle_vtimer_due", timer)
        self.assertIn("&s_hle_vtimer_event", timer)
        self.assertIn("sceKernelDelayThread(timeout_us);", timer)
        self.assertRegex(timer, r"#define HLE_TIMER_WAIT_LIMIT_US 250000u")
        self.assertRegex(timer, r"#define HLE_TIMER_ZERO_WAIT_US 100000u")
        self.assertIn("sceKernelCancelAlarm(uid)", timer)
        self.assertIn("sceKernelDeleteVTimer(vtimer)", timer)

        nids = json.loads((self.root / "tools" / "nid_corpus.json").read_text(
            encoding="utf-8"))["entries"]
        audio_names = {entry["name"] for entry in nids
                       if entry["name"].startswith("sceAudioSRC")}
        self.assertEqual(audio_names, {
            "sceAudioSRCChReserve", "sceAudioSRCChRelease", "sceAudioSRCOutputBlocking",
        })

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
        self.assertIn("HLE_PRIVATE_STRINGS_PATH", private)
        self.assertNotIn("hle_record", private)
        self.assertNotIn("hle_emit", private)

    def test_private_nickname_file_is_removed_after_its_read_back(self) -> None:
        """#1072 M4: the nickname file must not outlive the case. No tool reads it, so the
        probe reads it back, compares it with what it wrote, then removes it whatever the
        comparison found; a failed round trip or removal fails the run."""
        section = self._section("HLE_CASE_SYSPARAM")
        self.assertIn('#define HLE_PRIVATE_STRINGS_PATH "host0:/hle_sysparam_private_strings.txt"',
                      section)
        private = self._function_body(section, "static void hle_private_nickname_write(")
        steps = [
            "sceIoOpen(HLE_PRIVATE_STRINGS_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);",
            "sceIoWrite(fd, written, total);",
            "sceIoOpen(HLE_PRIVATE_STRINGS_PATH, PSP_O_RDONLY, 0);",
            "sceIoRead(fd, observed, (SceSize)sizeof(observed));",
            "memcmp(written, observed, total) == 0",
            "if (sceIoRemove(HLE_PRIVATE_STRINGS_PATH) < 0 || !verified) { s_log_failed = 1; }",
        ]
        positions = [private.find(step) for step in steps]
        self.assertNotIn(-1, positions, dict(zip(steps, positions, strict=True)))
        self.assertEqual(positions, sorted(positions))
        # Once the write is issued every path reaches the removal: no return after it, and
        # the read-back result only decides PASS or FAIL.
        self.assertNotIn("return", private[positions[1]:])
        self.assertEqual(private.count("sceIoClose(fd)"), 2)
        # The read-back holds one byte more than was written, so a longer file is caught.
        self.assertIn("static uint8_t observed[sizeof(written) + 1u];", private)
        # Removal is this file's alone: no other host0 file and no other family removes.
        self.assertEqual(re.findall(r"\bsceIoRemove\s*\(([^)]*)\)", self.probe),
                         ["HLE_PRIVATE_STRINGS_PATH"])
        issues = (self.fixture / "probe_hle_issues.c").read_text(encoding="utf-8")
        self.assertNotRegex(issues, r"\bsceIoRemove\s*\(")

    def test_probe_emits_the_protocol_lines_and_the_log_the_runner_reads(self) -> None:
        for marker in ("NAKAGAWA_PSP_META schema=1", "NAKAGAWA_PSP_STEP schema=1 case_id=",
                       "NAKAGAWA_PSP_TEST schema=1 test_id=",
                       "NAKAGAWA_PSP_COMPLETE schema=1 status="):
            self.assertIn(marker, self.probe)
        registrations = {
            case: (test_id, log) for case, test_id, log in re.findall(
                r'#define HLE_CAMPAIGN_ID "(hle-[a-z0-9-]+)"\n#define HLE_TEST_ID "([A-Z0-9-]+)"\n'
                r'#define HLE_LOG "host0:/([a-z0-9_]+\.txt)"', self.probe)
        }
        self.assertEqual(len(registrations), len(self.ALL_FAMILIES))
        for case, _cid, _stem, _block, _macro, test_id in self.ALL_FAMILIES:
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
        for case, _cid, _stem, _block, _macro, test_id in self.ALL_FAMILIES:
            with self.subTest(case=case):
                spec, counts = CAMPAIGN_PROBE_CASES[case]
                self.assertEqual(spec.test_id, test_id)
                self.assertEqual(spec.terminal_case, f"{case}-done")
                self.assertEqual(_campaign_completeness_contract(case), "strict-golden-sequence")
                self.assertEqual(set(counts), set(spec.ordered_cases))
                self.assertEqual(counts[spec.terminal_case], 1)
                for semantic in spec.semantic_cases:
                    self.assertIn(f'"{semantic}"', self.probe, semantic)

    def test_out_word_counts_match_the_parser_registry_for_every_family(self) -> None:
        """Each cell's out-word count, read from the record call that emits it, equals the
        parser registry's count, and the cells come in registry order, for families 90
        through 101."""
        for case, _cid, _stem, _block, macro, _test in self.ALL_FAMILIES:
            with self.subTest(case=case):
                self.assertEqual(_out_count_mismatches(self.probe, case, macro), [])

    def test_a_count_mutant_is_refused_by_the_same_comparison(self) -> None:
        """Mutant: the no-slot create record declares one out word where it writes two."""
        original = 'hle_record("vtimer-err-noslot-create", "PASS", refusal, create_out, 2);'
        mutant = self.probe.replace(original, original.replace(", 2);", ", 1);"))
        self.assertNotEqual(mutant, self.probe)
        problems = _out_count_mismatches(mutant, "hle-vtimer-errors", "HLE_CASE_VTIMER_ERRORS")
        self.assertIn("vtimer-err-noslot-create: source 1 != registry 2", problems)

    def test_manifest_entries_are_captured_and_cite_the_registry(self) -> None:
        """The 2026-10-15 single-case runs on e45e667b measured every family (acceptance-eligible)."""
        by_id = {test["id"]: test for test in self.manifest["tests"]}
        for case, _cid, stem, _block, _macro, test_id in self.FAMILIES:
            with self.subTest(case=case):
                entry = by_id[test_id]
                spec, _counts = CAMPAIGN_PROBE_CASES[case]
                self.assertEqual(entry["hardware_evidence"], "MEASURED")
                self.assertEqual(
                    entry["evidence_ref"],
                    "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize",
                )
                self.assertTrue(set(entry["case_ids"]).issubset(entry["evidence_cases"]))
                self.assertIn("2026-10-15", entry["measurement_note"])
                self.assertIn("acceptance-eligible", entry["measurement_note"])
                self.assertEqual(entry["status"], "implemented")
                self.assertEqual(entry["source"], "fixtures/psp_oracle/probe_hle_measure.c")
                self.assertEqual(entry["prx"], f"{stem}.prx")
                self.assertEqual(entry["case_ids"], list(spec.ordered_cases))
                self.assertEqual(entry["diagnostic_case_ids"], [case])
                self.assertEqual(entry["issues"], [])

    # A raw-wrapper invocation names its target stub as its second argument.
    WRAPPER_CALL_RE = re.compile(r"\bHLE_PFV_WRAPPER\(\s*\w+\s*,\s*(sce\w+)\s*\)")

    @classmethod
    def _calls_in(cls, text: str) -> set[str]:
        """Stub names a probe text calls: direct calls, raw `jal` targets, and the
        targets named in HLE_PFV_WRAPPER invocations."""
        return (set(re.findall(r"\b(sce\w+)\s*\(", text))
                | set(re.findall(r"\bjal\s+(sce\w+)", text))
                | set(cls.WRAPPER_CALL_RE.findall(text)))

    def _probe_calls(self, macro: str) -> set[str]:
        """The sce stubs one family PRX calls: its own section plus the shared probe code.

        The shared code (main, the module lifecycle and the log writer) is linked into
        every family PRX, so its calls are imports of every family. Prototype lines are
        declarations, not calls, and are dropped before the scan.
        """
        text = self._shared_section() + self._section(macro)
        calls = "\n".join(line for line in text.splitlines()
                          if not self.PROTOTYPE_LINE_RE.fullmatch(line))
        return self._calls_in(calls)

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
        for case, *_rest in self.ALL_FAMILIES:
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


class HleSingleLaunchProbeTests(unittest.TestCase):
    """The seven single-launch HLE cases (97-103) in probe_hle_measure.c.

    These tests pin what keeps them machinery-only: the Makefile routes, their absence
    from the campaign queue, the host0 records, the bounded loops and their cleanup, the
    stream length the default runner timeout has to cover, where NULL is passed (a
    mandatory out pointer only in the null-out-pointers case), the raw `jal` wrapper of
    the power getters, and the parser's refusal of every status but PASS and SKIP. They
    also pin the manifest and docs rows: cases 97-101 have recorded runs from the
    2026-10-15 PSP-3000 session on b5d6fc5b (three measured, one earlier revision
    captured only, and one incomplete), and cases 102-103 were measured the same day (the
    timer-order case from 22b027f6, the audio SRC case from 7fc1641e after a first run of
    22b027f6 that was captured only).
    """

    SINGLE = HleMeasureProbeTests.SINGLE_LAUNCH_FAMILIES
    # Manifest hardware_evidence per case after the 2026-10-15 runs. The pool-bounds probe was
    # revised after its captured run, so its revised form is NOT_RUN.
    HARDWARE = {
        "hle-vtimer-unit": "MEASURED",
        "hle-vtimer-errors": "MEASURED",
        "hle-kernel-pool-bounds": "NOT_RUN",
        "hle-power-float-v0": "MEASURED",
        "hle-null-out-pointers": "CAPTURED",
        "hle-audio-src": "MEASURED",
        "hle-timer-order": "MEASURED",
    }
    # The public commit each MEASURED case's cited run was built from.
    RUN_COMMIT = {
        "hle-vtimer-unit": "b5d6fc5b",
        "hle-vtimer-errors": "b5d6fc5b",
        "hle-power-float-v0": "b5d6fc5b",
        "hle-audio-src": "7fc1641e",
        "hle-timer-order": "22b027f6",
    }
    # The pending-campaign status cell each case carries in docs/HARDWARE_ORACLE.md.
    DOC_STATUS = {
        "hle-vtimer-unit": "`MEASURED` (2026-10-15, `b5d6fc5b`; single launch, not in the "
                           "campaign queue)",
        "hle-vtimer-errors": "`MEASURED` (2026-10-15, `b5d6fc5b`; single launch, not in the "
                             "campaign queue)",
        "hle-kernel-pool-bounds": "`NOT_RUN` (revised form; single launch, not in the campaign "
                                  "queue; the earlier revision's 2026-10-15 `b5d6fc5b` run was "
                                  "captured only)",
        "hle-power-float-v0": "`MEASURED` (2026-10-15, `b5d6fc5b`; single launch, not in the "
                              "campaign queue)",
        "hle-null-out-pointers": "`CAPTURED` (2026-10-15, `b5d6fc5b`; incomplete, not measured; "
                                 "retired, never to be scheduled)",
        "hle-audio-src": "`MEASURED` (2026-10-15, `7fc1641e`; the earlier run of `22b027f6` "
                         "was captured only; single launch, not in the campaign queue)",
        "hle-timer-order": "`MEASURED` (2026-10-15, `22b027f6`; single launch, not in the "
                           "campaign queue)",
    }
    NULL_CASE = "hle-null-out-pointers"
    OBJECT_CASES = frozenset({"hle-vtimer-unit", "hle-vtimer-errors",
                              "hle-kernel-pool-bounds", "hle-null-out-pointers",
                              "hle-audio-src", "hle-timer-order"})
    HOST0_LOGS = {
        "hle-vtimer-unit": "hle_vtimer_unit_log.txt",
        "hle-vtimer-errors": "hle_vtimer_errors_log.txt",
        "hle-kernel-pool-bounds": "hle_kernel_pool_bounds_log.txt",
        "hle-power-float-v0": "hle_power_float_v0_log.txt",
        "hle-null-out-pointers": "hle_null_out_pointers_log.txt",
        "hle-audio-src": "hle_audio_src_log.txt",
        "hle-timer-order": "hle_timer_order_log.txt",
    }
    POWER_FLOAT_GETTERS = frozenset({
        "scePowerGetPllClockFrequencyFloat", "scePowerGetCpuClockFrequencyFloat",
        "scePowerGetBusClockFrequencyFloat",
    })
    # Declared in hle_power_imports.S and never called by the float family.
    POWER_UNCALLED = frozenset({
        "scePowerGetPllClockFrequencyInt", "scePowerGetCpuClockFrequency",
        "scePowerGetCpuClockFrequencyInt", "scePowerGetBusClockFrequency",
        "scePowerGetBusClockFrequencyInt",
    })
    # Calls whose out pointer pspthreadman.h makes a mandatory output. Only the
    # null-out-pointers case passes NULL to them.
    MANDATORY_NULL_CALLS = frozenset({
        "sceKernelReferSystemStatus", "sceKernelGetVTimerTime", "sceKernelGetVTimerBase",
        "sceKernelReferVTimerStatus",
    })
    # Calls that take a NULL option pointer, which is the API's optional argument.
    OPTIONAL_NULL_CALLS = frozenset({"sceKernelCreateVTimer", "sceKernelCreateFpl",
                                     "sceKernelCreateVpl"})
    NULL_RISK = ("high: retired and never to be scheduled; PSP-3000 hangs on the first call, "
                 "sceKernelReferSystemStatus(NULL), because address 0 is not rejected by the "
                 "kernel pointer check and the call writes through NULL.")
    # Worst-case lines per launch: META, one STEP per reached call, one record per cell,
    # the `-done` record and COMPLETE. Pinned so a change to a stream shows up here.
    WORST_CASE_LINES = {
        "hle-vtimer-unit": 29, "hle-vtimer-errors": 23, "hle-kernel-pool-bounds": 19,
        "hle-power-float-v0": 9, "hle-null-out-pointers": 15,
        "hle-audio-src": 49, "hle-timer-order": 41,
    }

    @classmethod
    def setUpClass(cls) -> None:
        HleMeasureProbeTests.setUpClass()
        # Its helpers read the probe, Makefile and manifest that setUpClass just loaded.
        cls.h = HleMeasureProbeTests("test_case_ids_are_registered_once_and_match_the_families")

    def test_routes_are_97_to_103_and_each_links_its_own_prx(self) -> None:
        pairs = re.findall(r"else ifeq \(\$\(CASE\),(hle-[a-z0-9-]+)\)\nCASE_ID = (\d+)",
                           self.h.makefile)
        routed = sorted((case, int(case_id)) for case, case_id in pairs
                        if case in self.HOST0_LOGS)
        self.assertEqual(routed, sorted((case, case_id) for case, case_id, *_ in self.SINGLE))
        self.assertEqual(sorted(case_id for _case, case_id in routed),
                         [97, 98, 99, 100, 101, 102, 103])
        for case, _cid, stem, _block, _macro, _test in self.SINGLE:
            with self.subTest(case=case):
                self.assertRegex(
                    self.h.makefile,
                    rf"else ifeq \(\$\(CASE\),{case}\)\nTARGET = \$\(BUILD_DIR\)/{stem}\n")

    def test_none_of_the_single_launch_cases_is_in_the_campaign_queue(self) -> None:
        for case, *_rest in self.SINGLE:
            with self.subTest(case=case):
                self.assertNotIn(case, CAMPAIGN_QUEUE_CASES)

    def test_host0_records_carry_each_case_its_own_test_id_and_log(self) -> None:
        for case, _cid, _stem, _block, _macro, test_id in self.SINGLE:
            with self.subTest(case=case):
                log = self.HOST0_LOGS[case]
                self.assertIn(
                    f'#define HLE_CAMPAIGN_ID "{case}"\n#define HLE_TEST_ID "{test_id}"\n'
                    f'#define HLE_LOG "host0:/{log}"',
                    self.h.probe)
                spec, _counts = CAMPAIGN_PROBE_CASES[case]
                self.assertEqual(spec.test_id, test_id)
                self.assertEqual(spec.terminal_case, f"{case}-done")
                self.assertEqual(_campaign_host0_log_path(Path("/scratch"), case).name, log)

    def test_each_cell_records_pass_or_skip_and_never_fail(self) -> None:
        for case, _cid, _stem, _block, macro, _test in self.SINGLE:
            with self.subTest(case=case):
                section = self.h._section(macro)
                self.assertNotIn('"FAIL"', section)
                self.assertEqual(
                    set(re.findall(r'hle_record\(\s*[^,]+,\s*"([A-Z]+)"', section)), {"PASS"})
                # SKIP is recorded only by hle_skip, which the object-holding cases reach
                # when a UID was not obtained; the float family holds no object.
                self.assertEqual(bool(re.search(r"\bhle_skip\(", section)),
                                 case in self.OBJECT_CASES)

    def test_loops_are_capped_and_every_object_they_make_is_deleted(self) -> None:
        vte = self.h._section("HLE_CASE_VTIMER_ERRORS")
        self.assertIn("#define HLE_VTE_NOSLOT_CAP 64u", vte)
        self.assertIn("for (uint32_t i = 0; i < HLE_VTE_NOSLOT_CAP; i++) {", vte)
        self.assertIn("for (uint32_t i = 0; i < created; i++) {", vte)
        self.assertIn("sceKernelDeleteVTimer(slots[i])", vte)
        pool = self.h._section("HLE_CASE_KERNEL_POOL_BOUNDS")
        self.assertIn("#define HLE_KSN_VPL_CAP 64u", pool)
        self.assertIn("for (uint32_t i = 0; i < HLE_KSN_VPL_CAP; i++) {", pool)
        self.assertIn("for (uint32_t i = 0; i < taken; i++) {", pool)
        self.assertIn("sceKernelFreeVpl(vpl, blocks[i])", pool)
        self.assertIn("sceKernelDeleteVpl(vpl)", pool)
        self.assertIn("sceKernelDeleteFpl(uid)", pool)

    def test_no_step_marker_sits_inside_a_loop(self) -> None:
        """A step per iteration would scale the stream with the loop cap (64)."""
        for case, _cid, _stem, _block, macro, _test in self.SINGLE:
            with self.subTest(case=case):
                section = _c_code(self.h._section(macro))
                for loop in re.finditer(r"\bfor\s*\(", section):
                    brace = section.index("{", loop.end())
                    body = section[brace + 1:_c_close(section, brace)]
                    self.assertNotIn("hle_step(", body)

    def test_vtimer_errors_deletes_the_stale_timer_before_the_live_cells(self) -> None:
        section = _c_code(self.h._section("HLE_CASE_VTIMER_ERRORS"))
        run = HleMeasureProbeTests._function_body(section, "static int hle_run(")
        time_valid = run.index('hle_vte_time_valid("vtimer-err-time-deleted", stale);')
        cleanup = run.index("hle_vte_cleanup_stale(stale);")
        live = run.index('hle_vte_create("vtimer-err-create-live")')
        self.assertLess(time_valid, cleanup)
        self.assertLess(cleanup, live)
        body = HleMeasureProbeTests._function_body(section, "static void hle_vte_cleanup_stale(")
        self.assertIn("sceKernelDeleteVTimer(uid);", body)
        self.assertNotIn("if (uid", body)  # unconditional: the delete cell may have failed
        self.assertNotIn("return issued == created;", section)

    def test_stream_lengths_are_pinned_and_fit_the_default_runner_timeout(self) -> None:
        """The default --timeout is 30 s for the whole launch, so a stream of N lines
        needs each host0 operation (open, write, close: three per line, plus the
        round trip) under 30 s / (3N + 6) on the console. No measured host0 operation
        cost exists in this repository, so the bound is an assumption to measure."""
        for case, _cid, _stem, _block, macro, _test in self.SINGLE:
            with self.subTest(case=case):
                spec, _counts = CAMPAIGN_PROBE_CASES[case]
                _cells, steps = _probe_cells(self.h.probe, macro)
                lines = steps + len(spec.semantic_cases) + 3
                self.assertEqual(lines, self.WORST_CASE_LINES[case])
                self.assertLessEqual(lines, 64)

    def test_null_is_passed_to_mandatory_outs_only_in_the_null_case(self) -> None:
        for case, _cid, _stem, _block, macro, _test in self.SINGLE:
            with self.subTest(case=case):
                section = self.h._section(macro)
                with_null = set(re.findall(r"\b(sce\w+)\s*\([^;]*\bNULL\b", section))
                if case == self.NULL_CASE:
                    self.assertEqual(with_null,
                                     self.MANDATORY_NULL_CALLS | {"sceKernelCreateVTimer"})
                else:
                    self.assertTrue(with_null <= self.OPTIONAL_NULL_CALLS, with_null)
                    self.assertEqual(with_null & self.MANDATORY_NULL_CALLS, set())

    def test_null_out_pointer_cells_are_one_call_each_after_a_step(self) -> None:
        section = _c_code(self.h._section("HLE_CASE_NULL_OUT_POINTERS"))
        run = HleMeasureProbeTests._function_body(section, "static int hle_run(")
        order = [run.index(call) for call in (
            "hle_null_vtimer_create();", "hle_null_sys_status();", "hle_null_time(vt);",
            "hle_null_base(vt);", "hle_null_refer(vt);", "hle_null_delete(vt);")]
        self.assertEqual(order, sorted(order))
        for helper, call in (("static void hle_null_sys_status(", "sceKernelReferSystemStatus(NULL)"),
                             ("static void hle_null_time(", "sceKernelGetVTimerTime(uid, NULL)"),
                             ("static void hle_null_base(", "sceKernelGetVTimerBase(uid, NULL)"),
                             ("static void hle_null_refer(", "sceKernelReferVTimerStatus(uid, NULL)")):
            with self.subTest(helper=helper):
                body = HleMeasureProbeTests._function_body(section, helper)
                self.assertEqual(body.count("sce"), 1)
                self.assertIn(call, body)
                self.assertLess(body.index("hle_step("), body.index(call))
                self.assertEqual(body.count("hle_record("), 1)
                self.assertIn("NULL, 0);", body)  # the record holds only the return code

    def test_power_float_wrapper_is_one_jal_per_getter_and_stores_v0_and_f0(self) -> None:
        section = self.h._section("HLE_CASE_POWER_FLOAT_V0")
        for fragment in (r'".pushsection .text\n\t"', r'".set noreorder\n\t"',
                         r'"li $t1, " HLE_SENTINEL_HEX "\n\t"', r'"mtc1 $t1, $f0\n\t"',
                         r'"jal " #target "\n\t"', r'"mfc1 $t1, $f0\n\t"',
                         r'"sw $v0, 0($t0)\n\t"', r'"sw $t1, 4($t0)\n\t"',
                         r'"sw $ra, 28($sp)\n\t"', r'"jr $ra\n\t"', r'".popsection\n")'):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, section)
        # $f0 is preset to the sentinel before the jal, so an untouched $f0 is recorded.
        self.assertLess(section.index('"mtc1 $t1, $f0'), section.index('"jal " #target'))
        sentinel_text = re.search(r'#define HLE_SENTINEL_HEX "(0x[0-9A-Fa-f]+)"', self.h.probe)
        self.assertEqual(int(sentinel_text.group(1), 16), 0xA5A5A5A5)
        self.assertIn("#define HLE_SENTINEL 0xA5A5A5A5u", self.h.probe)
        self.assertEqual(
            re.findall(r"^HLE_PFV_WRAPPER\((\w+), (\w+)\);$", section, re.MULTILINE),
            [("hle_pfv_raw_pll", "scePowerGetPllClockFrequencyFloat"),
             ("hle_pfv_raw_cpu", "scePowerGetCpuClockFrequencyFloat"),
             ("hle_pfv_raw_bus", "scePowerGetBusClockFrequencyFloat")])
        cell = HleMeasureProbeTests._function_body(section, "static void hle_pfv_cell(")
        self.assertLess(cell.index("hle_step(case_id);"), cell.index("raw(words);"))
        self.assertIn('hle_record(case_id, "PASS", v0, &f0, 1);', cell)

    def test_power_float_declares_five_getters_it_never_calls(self) -> None:
        declared = {name for _lib, _nid, name in
                    self.h._imports(self.h._block_text("hle_power_imports.S"))}
        called = self.h._probe_calls("HLE_CASE_POWER_FLOAT_V0")
        self.assertLessEqual(self.POWER_FLOAT_GETTERS, called)
        self.assertEqual(declared - called, self.POWER_UNCALLED)

    def test_parser_accepts_arbitrary_machinery_values(self) -> None:
        for case, _cid, _stem, _block, _macro, _test in self.SINGLE:
            with self.subTest(case=case):
                spec, counts = CAMPAIGN_PROBE_CASES[case]
                cells = {name: ("PASS", 0xFFFFFFFF, [0xDEADBEEF] * counts[name])
                         for name in spec.semantic_cases}
                first = spec.semantic_cases[0]
                cells[first] = ("SKIP", 0, None)
                if case == self.POOL_CASE:
                    # Only the fixed-pool delete bookkeeping is constrained: with no pool
                    # the delete cell made no call; with one, it issued a delete.
                    cells["fpl-delete-size-0x00"] = ("PASS", 0, [0])
                    cells["fpl-create-size-0x7fffffff"] = ("PASS", 0xFFFFFFFF, [1])
                    cells["fpl-delete-size-0x7fffffff"] = ("PASS", 0xFFFFFFFF, [1])
                accepted = parse_campaign_probe_output(
                    self.h._stream(case, cells).split("NAKAGAWA_PSP_COMPLETE")[0], case)
                self.assertTrue(accepted.complete)

    POOL_CASE = "hle-kernel-pool-bounds"
    # A refused create (any negative word, out0 0) followed by a delete cell with no pool,
    # and a created pool (a UID, out0 1) followed by an issued delete.
    POOL_REFUSED = {
        "fpl-create-size-0x00": ("PASS", 0x800201B7, [0]),
        "fpl-delete-size-0x00": ("PASS", 0, [0]),
        "fpl-create-size-0x7fffffff": ("PASS", 0x80020190, [0]),
        "fpl-delete-size-0x7fffffff": ("PASS", 0, [0]),
    }
    POOL_CREATED = {
        "fpl-create-size-0x00": ("PASS", 0x04300001, [1]),
        "fpl-delete-size-0x00": ("PASS", 0, [1]),
        "fpl-create-size-0x7fffffff": ("PASS", 0x04300003, [1]),
        "fpl-delete-size-0x7fffffff": ("PASS", 0, [1]),
    }

    def _pool_body(self, overrides: dict) -> str:
        return self.h._stream(self.POOL_CASE, overrides).split("NAKAGAWA_PSP_COMPLETE")[0]

    def test_pool_delete_without_a_pool_is_a_pass_that_issues_no_delete(self) -> None:
        """A refused fixed-pool create is the measured outcome, so the delete cell after it
        is PASS with out0 0 and makes no call; a SKIP there made the 2026-10-15 envelope
        ineligible."""
        section = _c_code(self.h._section("HLE_CASE_KERNEL_POOL_BOUNDS"))
        body = HleMeasureProbeTests._function_body(section, "static void hle_ksn_fpl_delete(")
        self.assertNotIn("hle_skip(", body)
        branch = body[body.index("if (uid < 0) {"):body.index("return; }")]
        self.assertIn("const uint32_t no_object = 0;", branch)
        self.assertIn('hle_record(case_id, "PASS", 0u, &no_object, 1);', branch)
        self.assertNotIn("sceKernelDeleteFpl", branch)
        self.assertNotIn("hle_step(", branch)
        issued = body[body.index("return; }"):]
        self.assertIn("const uint32_t issued = 1;", issued)
        self.assertLess(issued.index("hle_step(case_id);"), issued.index("sceKernelDeleteFpl(uid)"))
        self.assertIn('hle_record(case_id, "PASS", (uint32_t)rc, &issued, 1);', issued)
        # The VPL cells keep their SKIP: a VPL that was not obtained is a missing object.
        self.assertIn('hle_skip("vpl-try-alloc-refusal", 2);', section)

    def test_parser_accepts_refused_and_created_pools_with_matching_deletes(self) -> None:
        for name, overrides in (("refused", self.POOL_REFUSED), ("created", self.POOL_CREATED)):
            with self.subTest(pools=name):
                accepted = parse_campaign_probe_output(self._pool_body(overrides), self.POOL_CASE)
                self.assertTrue(accepted.complete)
                self.assertTrue(all(record.status == "PASS"
                                    for record in accepted.results.values()))

    def test_parser_refuses_a_skip_or_mismatched_fixed_pool_delete(self) -> None:
        refused = self.POOL_REFUSED
        cases = {
            # The earlier revision's shape: a SKIP delete after a refused create.
            "skip": {**refused, "fpl-delete-size-0x00": ("SKIP", 0, [0])},
            # A delete cell that claims a call although its create returned no pool.
            "issued-without-pool": {**refused, "fpl-delete-size-0x7fffffff": ("PASS", 0, [1])},
            # A delete cell that claims no pool although its create returned one.
            "no-object-after-create": {**self.POOL_CREATED,
                                       "fpl-delete-size-0x00": ("PASS", 0, [0])},
            # No pool, so no call was made, yet a result was recorded.
            "result-without-call": {**refused,
                                    "fpl-delete-size-0x00": ("PASS", 0x8002019D, [0])},
            "out0-not-a-flag": {**self.POOL_CREATED,
                                "fpl-create-size-0x00": ("PASS", 0x04300001, [2]),
                                "fpl-delete-size-0x00": ("PASS", 0, [2])},
        }
        for name, overrides in cases.items():
            with self.subTest(mutant=name):
                with self.assertRaisesRegex(ProtocolError, "fpl-delete-size"):
                    parse_campaign_probe_output(self._pool_body(overrides), self.POOL_CASE)

    def test_the_pool_delete_check_compares_no_psp_value(self) -> None:
        """Any create word is accepted; only the probe's own create/delete flags must agree."""
        for create_word in (0x0, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x800201B7):
            with self.subTest(create=hex(create_word)):
                overrides = {
                    "fpl-create-size-0x00": ("PASS", create_word, [0]),
                    "fpl-delete-size-0x00": ("PASS", 0, [0]),
                    "fpl-create-size-0x7fffffff": ("PASS", create_word, [1]),
                    "fpl-delete-size-0x7fffffff": ("PASS", 0xDEADBEEF, [1]),
                }
                parse_campaign_probe_output(self._pool_body(overrides), self.POOL_CASE)

    def test_parser_refuses_every_status_other_than_pass_and_skip(self) -> None:
        """A check that names one status (FAIL) lets the others through; the refusal
        must cover every status the protocol knows, except the two machinery values."""
        for case, _cid, _stem, _block, _macro, _test in self.SINGLE:
            spec, counts = CAMPAIGN_PROBE_CASES[case]
            base = {name: ("PASS", 0, [0] * counts[name]) for name in spec.semantic_cases}
            first = spec.semantic_cases[0]
            for status in sorted(STATUSES - {"PASS", "SKIP"}):
                with self.subTest(case=case, status=status):
                    refused = dict(base)
                    refused[first] = (status, 0, None)
                    with self.assertRaises(ProtocolError) as raised:
                        parse_campaign_probe_output(
                            self.h._stream(case, refused).split("NAKAGAWA_PSP_COMPLETE")[0],
                            case)
                    self.assertTrue("machinery-only" in str(raised.exception)
                                    or status in str(raised.exception), str(raised.exception))

    def test_manifest_rows_record_the_2026_10_15_outcome_of_each_case(self) -> None:
        tests = self.h.manifest["tests"]
        anchor = "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize"
        for case, _cid, stem, _block, macro, test_id in self.SINGLE:
            with self.subTest(case=case):
                self.assertEqual(sum(1 for test in tests if test["id"] == test_id), 1)
                row = next(test for test in tests if test["id"] == test_id)
                spec, _counts = CAMPAIGN_PROBE_CASES[case]
                self.assertEqual(row["status"], "implemented")
                self.assertEqual(row["hardware_evidence"], self.HARDWARE[case])
                if self.HARDWARE[case] == "MEASURED":
                    # Every cell of the complete, acceptance-eligible stream is cited.
                    self.assertEqual(row["evidence_ref"], anchor)
                    self.assertEqual(row["evidence_cases"][:len(spec.ordered_cases)],
                                     list(spec.ordered_cases))
                    self.assertNotIn("uncited_cases", row)
                    self.assertNotIn("evidence_note", row)
                    for word in ("2026-10-15", self.RUN_COMMIT[case],
                                 "acceptance-eligible with 0 blockers"):
                        self.assertIn(word, row["measurement_note"])
                else:
                    # A capture that was not acceptance-eligible, or a revision with no
                    # capture, cites nothing.
                    self.assertNotIn("evidence_ref", row)
                    self.assertNotIn("evidence_cases", row)
                    self.assertNotIn("measurement_note", row)
                    self.assertIn("not in the campaign queue", row["evidence_note"])
                    if self.HARDWARE[case] == "CAPTURED":
                        for word in ("2026-10-15", "b5d6fc5b",
                                     "establishes nothing about PSP behaviour"):
                            self.assertIn(word, row["evidence_note"])
                    elif case == "hle-kernel-pool-bounds":
                        self.assertIn("No hardware capture of this revision exists",
                                      row["evidence_note"])
                    else:
                        self.assertIn("No hardware capture exists",
                                      row["evidence_note"])
                self.assertEqual(row["source"], "fixtures/psp_oracle/probe_hle_measure.c")
                self.assertEqual(row["prx"], f"{stem}.prx")
                self.assertEqual(row["case_ids"], list(spec.ordered_cases))
                self.assertEqual(row["diagnostic_case_ids"], [case])
                expected_issues = {
                    "hle-audio-src": [339],
                    "hle-timer-order": [325],
                }.get(case, [])
                self.assertEqual(row["issues"], expected_issues)
                self.assertEqual(row["reset"],
                                 "one PRX launch; PSPLink soft reset before the next launch")
                listed = {api for api in row["apis"] if api.startswith("sce")}
                self.assertEqual(sorted(listed), sorted(self.h._probe_calls(macro)))
                self.assertNotIn("until refusal", json.dumps(row))

    def test_vtimer_unit_row_does_not_claim_a_precise_rate(self) -> None:
        """The VTimer and system-clock intervals are offset by host0 writes, so the unit is
        recorded as consistent with microseconds, never as a measured rate."""
        row = next(test for test in self.h.manifest["tests"] if test["id"] == "PSP-HLE-VTIMER-002")
        hardware = (self.h.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        for text in (row["measurement_note"], hardware):
            self.assertIn("204,379", text)
            self.assertIn("220,608", text)
            self.assertIn("not a precise rate measurement", text)
        self.assertNotIn("time unit is measured", row["measurement_note"])

    def test_the_incomplete_null_capture_cites_no_cell(self) -> None:
        """The null-out-pointers stream stopped at its first NULL call's step: a run fact
        that no MEASURED row may cite."""
        cells = set(CAMPAIGN_PROBE_CASES[self.NULL_CASE][0].ordered_cases)
        for test in self.h.manifest["tests"]:
            if test.get("hardware_evidence") == "MEASURED":
                with self.subTest(test_id=test["id"]):
                    self.assertEqual(cells & set(test["evidence_cases"]), set())
        row = next(test for test in self.h.manifest["tests"]
                   if test["id"] == "PSP-HLE-NULL-OUT-001")
        self.assertEqual(row["hardware_evidence"], "CAPTURED")
        for word in ("null-sys-status", "0.9 s after launch", "240 s case timeout",
                     "power cycle", "does not show whether that call faulted"):
            self.assertIn(word, row["evidence_note"])

    def test_null_out_pointer_case_is_retired_and_never_scheduled(self) -> None:
        tests = self.h.manifest["tests"]
        row = next(test for test in tests if test["id"] == "PSP-HLE-NULL-OUT-001")
        self.assertEqual(row["risk"], self.NULL_RISK)
        hardware = (self.h.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        readme = (self.h.fixture / "README.md").read_text(encoding="utf-8")
        hardware_row = next(line for line in hardware.splitlines()
                            if "| `hle-null-out-pointers` |" in line)
        readme_row = next(line for line in readme.splitlines()
                          if "| `hle-null-out-pointers` |" in line)
        self.assertIn("retired and must never be scheduled", hardware)
        self.assertIn("Retired and never to be scheduled", readme)
        self.assertIn("writes through NULL and faults", hardware)
        self.assertIn("writes through NULL and faults", readme)
        self.assertNotIn("schedule last", hardware_row)
        self.assertNotIn("schedule last", readme_row)
        for text in (row["risk"], row["evidence_note"]):
            self.assertNotIn("NULL is the only invalid pointer", text)
        for case, _cid, _stem, _block, _macro, _test in self.SINGLE:
            if case != self.NULL_CASE:
                other = next(test for test in tests if test.get("diagnostic_case_ids") == [case])
                if case != "hle-audio-src":
                    self.assertNotIn("high:", other["risk"])
                self.assertNotIn("NULL is the only invalid pointer", other["risk"])

    def test_manual_campaign_refuses_retired_null_out_case(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(self.h.root / "tools" / "psp_oracle" / "run_psplink.py"),
                "--campaign-case", f"{self.NULL_CASE}=not-present.prx",
                "--source-commit", "0" * 40,
                "--model", "PSP-3000",
                "--firmware", "6.6.1",
                "--host0-root", str(self.h.fixture),
                "--session-id", "test-retired-case",
            ],
            cwd=self.h.root,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("retired and cannot be scheduled", completed.stderr)
        self.assertNotIn("campaign PRX is missing", completed.stderr)

    def test_docs_list_each_case_with_its_outcome_outside_the_queue(self) -> None:
        hardware = (self.h.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        readme = (self.h.fixture / "README.md").read_text(encoding="utf-8")
        for case, _cid, _stem, _block, _macro, test_id in self.SINGLE:
            with self.subTest(case=case):
                self.assertIn(f"| `{case}` | `{test_id}` | {self.DOC_STATUS[case]} |", hardware)
                self.assertIn(f"| `{case}` | `{test_id}` |", readme)
        self.assertNotIn("until refusal", hardware)
        self.assertNotIn("until refusal", readme)
        self.assertIn("up to 64 creates; out1 records whether a refusal occurred", hardware)
        self.assertIn("up to 64 creates; out1 records whether a refusal occurred", readme)

# ---- probe_hle_issues.c: the HLE issue cells (106-107) --------------------------------

def _c_function_bodies(code: str) -> dict[str, str]:
    """Top-level function bodies of comment-free C text, by name (braces included)."""
    functions: dict[str, str] = {}
    for match in re.finditer(r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}]*)\)\s*\{",
                             code, re.MULTILINE):
        name = match.group(1)
        if name in _C_NOT_CALLS:
            continue
        brace = code.index("{", match.end() - 1)
        functions[name] = code[brace:_c_close(code, brace) + 1]
    return functions


#: Install and remove calls of the interrupt-cell case, and what removes each one.
_HLE_INTR_REMOVERS = {
    "sceKernelCreateVTimer": "sceKernelDeleteVTimer",
    "sceKernelStartVTimer": "sceKernelStopVTimer",
    "sceKernelSetAlarm": "sceKernelCancelAlarm",
    "sceKernelRegisterSubIntrHandler": "sceKernelReleaseSubIntrHandler",
    "sceKernelEnableSubIntr": "sceKernelDisableSubIntr",
    "sceKernelCpuSuspendIntr": "sceKernelCpuResumeIntr",
}


def _hle_intr_cleanup_problems(probe: str) -> list[str]:
    """Paths of the hle-intr-cells section that leave a handler, timer, alarm or suspension.

    Checked on the comment-free section text: each function that installs something also
    removes it, after the install, with no `return` between the two; the VTimer delete
    is reached whenever its create succeeded; an alarm is cancelled whenever SetAlarm
    returned a UID; and the sub-interrupt release sits in its own `if (slot >= 0)` block,
    not inside the block that also needs Enable to have succeeded.
    """
    start = probe.rindex("#if PSP_ORACLE_CASE == HLE_CASE_INTR_CELLS\n")
    section = _c_code(probe[start:probe.index("#endif /* HLE_CASE_INTR_CELLS */", start)])
    functions = _c_function_bodies(section)
    problems: list[str] = []
    for install, remove in _HLE_INTR_REMOVERS.items():
        owners = [name for name, body in functions.items() if f"{install}(" in body]
        if len(owners) != 1:
            problems.append(f"{install} is called from {owners}, not from exactly one function")
            continue
        body = functions[owners[0]]
        if body.count(f"{install}(") != 1:
            problems.append(f"{install} is called more than once in {owners[0]}")
            continue
        at = body.index(f"{install}(")
        if f"{remove}(" not in body[at:]:
            problems.append(f"{owners[0]} calls {install} without a later {remove}")
            continue
        between = body[at:body.index(f"{remove}(", at)]
        # The install's own failure branch (`if (vt < 0) { ...; return; }`) installed nothing.
        between = re.sub(r"if \(\w+ < 0\) \{[^{}]*\}", "", between)
        if re.search(r"\breturn\b", between):
            problems.append(f"{owners[0]} can return between {install} and {remove}")
    vtimer = functions.get("ic_vtimer_series", "")
    if not re.search(r"if \(vt < 0\) \{[^{}]*return;\s*\}", vtimer):
        problems.append("ic_vtimer_series has no early return for a failed create only")
    alarm = functions.get("ic_alarm_series", "")
    if not re.search(r"if \(alarm >= 0\) \{\s*\(void\)sceKernelCancelAlarm\(alarm\);\s*\}", alarm):
        problems.append("ic_alarm_series does not cancel every alarm UID SetAlarm returned")
    run = functions.get("hle_run", "")
    release = run.find("sceKernelReleaseSubIntrHandler(")
    guard = run.rfind("if (slot >= 0) {", 0, release) if release >= 0 else -1
    if guard < 0 or _c_close(run, guard + len("if (slot >= 0) ")) < release:
        problems.append("the sub-interrupt release is not in its own if (slot >= 0) block")
    else:
        enabled = run.rfind("if (slot >= 0 && en >= 0) {", 0, release)
        if enabled >= 0 and _c_close(run, enabled + len("if (slot >= 0 && en >= 0) ")) > release:
            problems.append("the sub-interrupt release needs Enable to have succeeded")
    return problems


class HleIssuesProbeTests(unittest.TestCase):
    """The HLE issue cells in probe_hle_issues.c: hle-issues-batch (106) and hle-intr-cells (107).

    These cases do what the probe_hle_measure.c families may not: 107 installs VTimer,
    alarm and VBLANK sub-interrupt handlers and suspends interrupts, and 106 calls a kernel
    getter through the kernel bridge. The tests here hold them to the same safety rules
    in spirit, adapted to what they do: every loop and wait bounded, a durable step marker
    before each cell's calls (never inside a timed window), every handler, timer, alarm and
    suspension removed on every path, import tables pinned exactly at link time, manifest
    apis naming every import, and a strict parser for each stream.
    """

    # campaign case, CASE_ID, PRX stem, section macro, test id, host0 log
    CASES = (
        ("hle-issues-batch", 106, "hle_issues_batch", "HLE_CASE_ISSUES_BATCH",
         "PSP-HLE-ISSUES-001", "hle_issues_batch_log.txt"),
        ("hle-intr-cells", 107, "hle_intr_cells", "HLE_CASE_INTR_CELLS",
         "PSP-HLE-INTR-001", "hle_intr_cells_log.txt"),
    )
    # The libraries each case imports beyond ThreadManForUser and the PSPSDK IO/CRT glue,
    # exactly: library -> {NID: name}. `GLUE` names are imported by the PSPSDK libc glue,
    # not called by the probe.
    EXACT_IMPORTS = {
        "hle-issues-batch": {
            "KUBridge": {0x24331850: "kuKernelGetModel", 0x9060F69D: "kuKernelCall"},
            "SystemCtrlForUser": {0x159AF5CC: "sctrlHENFindFunction"},
            "SysMemUserForUser": {
                0x6373995D: "sceKernelGetModel", 0x3FC9AE6A: "sceKernelDevkitVersion",
                0xA291F107: "sceKernelMaxFreeMemSize", 0x237DBD4F: "sceKernelAllocPartitionMemory",
                0xB6D61D02: "sceKernelFreePartitionMemory",
                0x9D9A5BA1: "sceKernelGetBlockHeadAddr",
            },
        },
        "hle-intr-cells": {
            "InterruptManager": {
                0xCA04A2B9: "sceKernelRegisterSubIntrHandler",
                0xD61E6961: "sceKernelReleaseSubIntrHandler",
                0xFB8E22EC: "sceKernelEnableSubIntr", 0x8A389411: "sceKernelDisableSubIntr",
            },
            "Kernel_Library": {
                0x092968F4: "sceKernelCpuSuspendIntr", 0x5F10D406: "sceKernelCpuResumeIntr",
                0xBEA46419: "sceKernelLockLwMutex", 0x15B6446B: "sceKernelUnlockLwMutex",
            },
        },
    }
    FORBIDDEN = {
        "hle-issues-batch": ("InterruptManager",),
        "hle-intr-cells": ("KUBridge", "SystemCtrlForUser"),
    }
    GLUE = frozenset({"sceKernelMaxFreeMemSize", "sceKernelAllocPartitionMemory",
                      "sceKernelFreePartitionMemory", "sceKernelGetBlockHeadAddr",
                      "sceKernelLockLwMutex", "sceKernelUnlockLwMutex"})
    # The shared writer, round trip and module lifecycle, repeated from probe_hle_measure.c.
    SHARED_FUNCTIONS = (
        "static void hle_write(const char *line, int truncate)",
        "static void hle_begin(void)",
        "static void hle_step(const char *step)",
        "static void hle_emit(const char *case_id, const char *status, uint32_t result,",
        "static void hle_record(const char *case_id, const char *status, uint32_t result,",
        "static int hle_host0_roundtrip(void)",
        "static void hle_finish(int ok)",
        "static void hle_park_until_stop(void)",
        "int module_stop(SceSize args, void *argp)",
        "int main(int argc, char **argv)",
    )
    # Calls that sit inside a cell and could hang or fault: each is made after the cell's
    # own step marker (see test_each_cell_writes_its_step_before_its_calls).
    MEASURED_CALL_RE = re.compile(
        r"\b(sceKernel(?:CreateVTimer|StartVTimer|StopVTimer|DeleteVTimer|SetVTimerHandler|"
        r"SetAlarm|CancelAlarm|DelayThread|RegisterSubIntrHandler|EnableSubIntr|"
        r"DisableSubIntr|ReleaseSubIntrHandler|CpuSuspendIntr|CpuResumeIntr|DevkitVersion)|"
        r"sctrlHENFindFunction|kuKernelCall|call)\s*\(")
    HANDLERS = ("ic_vbl_handler", "ic_vtimer_handler", "ic_alarm_handler")

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[1]
        cls.fixture = cls.root / "fixtures" / "psp_oracle"
        cls.probe = (cls.fixture / "probe_hle_issues.c").read_text(encoding="utf-8")
        cls.measure = (cls.fixture / "probe_hle_measure.c").read_text(encoding="utf-8")
        cls.makefile = (cls.fixture / "Makefile").read_text(encoding="utf-8")
        cls.threadman = (cls.fixture / "threadman_user_imports.S").read_text(encoding="utf-8")
        cls.manifest = json.loads((cls.root / "tools" / "psp_oracle" / "manifest.json")
                                  .read_text(encoding="utf-8"))
        cls.code = _c_code(cls.probe)

    def _section(self, macro: str) -> str:
        # The section, not the #if chain at the top that names the same macro.
        start = self.probe.rindex(f"#if PSP_ORACLE_CASE == {macro}\n")
        end = self.probe.index(f"#endif /* {macro} */", start)
        return self.probe[start:end]

    def _shared(self) -> str:
        text = self.probe
        for *_head, macro, _test, _log in reversed(self.CASES):
            start = text.rindex(f"#if PSP_ORACLE_CASE == {macro}\n")
            end = text.index(f"#endif /* {macro} */", start) + len(f"#endif /* {macro} */")
            text = text[:start] + text[end:]
        return text

    def _case_block(self, case: str) -> str:
        start = re.search(rf"^ifeq \(\$\(CASE\),{case}\)\n", self.makefile, re.MULTILINE).start()
        return self.makefile[start:self.makefile.index("\nendif\n", start)]

    # ---- Makefile wiring and import tables -------------------------------------------

    def test_routes_build_each_case_from_the_issues_source_and_threadman_block(self) -> None:
        routes = dict(re.findall(r"^else ifeq \(\$\(CASE\),([^\)]+)\)\nCASE_ID = (\d+)$",
                                 self.makefile, re.MULTILINE))
        for case, case_id, stem, *_rest in self.CASES:
            with self.subTest(case=case):
                self.assertEqual(routes.get(case), str(case_id))
                match = re.search(rf"else ifeq \(\$\(CASE\),{case}\)\nTARGET = \$\(BUILD_DIR\)/"
                                  rf"{stem}\nOBJS = (.*)\n", self.makefile)
                self.assertIsNotNone(match, case)
                self.assertEqual(match.group(1).split(), ["$(BUILD_DIR)/probe_hle_issues.o",
                                                          "$(BUILD_DIR)/threadman_user_imports.o"])
        self.assertIn("$(BUILD_DIR)/probe_hle_issues.o: probe_hle_issues.c hle_vfpu_cells.h\n"
                      "\t$(CC) $(CFLAGS) -c $< -o $@\n", self.makefile)
        self.assertIn("ifneq ($(filter $(BUILD_DIR)/probe_hle_issues.o,$(OBJS)),)\n"
                      "PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp\nendif\n", self.makefile)
        self.assertIn("int module_stop(SceSize args, void *argp)", self.probe)
        # The bridge libraries are linked by the model-getter case only.
        self.assertEqual(re.findall(r"^LIBS \+= .*pspsystemctrl_user.*$", self.makefile,
                                    re.MULTILINE), ["LIBS += -lpspkubridge -lpspsystemctrl_user"])
        self.assertIn("LIBS += -lpspkubridge -lpspsystemctrl_user",
                      self._case_block("hle-issues-batch"))

    def test_link_gate_pins_each_cases_extra_libraries_exactly(self) -> None:
        nid_names = dict(
            (int(nid, 16), name) for nid, name in re.findall(
                r'\{0x([0-9a-f]{8})u, "(\w+)"\}',
                (self.root / "src" / "rt" / "nid_names.h").read_text(encoding="utf-8")))
        for case, *_rest in self.CASES:
            with self.subTest(case=case):
                block = self._case_block(case)
                pins = {library: {int(nid, 16) for nid in nids.split(",")}
                        for library, nids in re.findall(
                            r"--exact-library (\w+)=((?:0x[0-9A-F]{8},?)+)", block)}
                self.assertEqual(pins, {library: set(table) for library, table
                                        in self.EXACT_IMPORTS[case].items()})
                self.assertEqual(sorted(re.findall(r"--forbid-library (\w+)", block)),
                                 sorted(self.FORBIDDEN[case]))
                for table in self.EXACT_IMPORTS[case].values():
                    for nid, name in table.items():
                        if nid in nid_names:
                            self.assertEqual(nid_names[nid], name, hex(nid))
        # The two bridge NIDs nid_names.h does not carry, as PSPSDK's archives declare them.
        self.assertEqual(self._nid_free_names(), {0x9060F69D, 0x159AF5CC})

    def _nid_free_names(self) -> set[int]:
        names = (self.root / "src" / "rt" / "nid_names.h").read_text(encoding="utf-8").lower()
        return {nid for table in self.EXACT_IMPORTS.values() for library in table.values()
                for nid in library if f"0x{nid:08x}u" not in names}

    def test_every_called_import_is_pinned_or_declared_by_the_threadman_block(self) -> None:
        threadman = {name for _lib, _nid, name in HleMeasureProbeTests._imports(self.threadman)}
        sdk_io = HleMeasureProbeTests.SDK_DEFAULT_CALLS
        for case, _cid, _stem, macro, _test, _log in self.CASES:
            with self.subTest(case=case):
                called = self._calls(macro)
                pinned = {name for table in self.EXACT_IMPORTS[case].values()
                          for name in table.values()}
                self.assertEqual(sorted(called - threadman - sdk_io - pinned), [])
                self.assertEqual(sorted(pinned - called - self.GLUE), [])

    def test_built_import_tables_match_the_pins_when_a_local_build_exists(self) -> None:
        build = self.fixture / "build"
        present = [(case, build / f"{stem}.elf") for case, _i, stem, *_r in self.CASES
                   if (build / f"{stem}.elf").is_file()]
        if not present:
            self.skipTest("no local hle_issues_batch/hle_intr_cells ELF; make CASE=<case> builds one")
        threadman = {nid for _lib, nid, _name in HleMeasureProbeTests._imports(self.threadman)}
        for case, path in present:
            with self.subTest(elf=path.name):
                data = path.read_bytes()
                result = user_mode_imports.check_module(
                    data,
                    {library: frozenset(table) for library, table
                     in self.EXACT_IMPORTS[case].items()},
                    frozenset(self.FORBIDDEN[case]))
                self.assertTrue(result.passed, result)
                table = user_mode_imports.parse_import_table(data)
                self.assertLessEqual({f.nid for f in table.funcs
                                      if f.library == "ThreadManForUser"}, threadman)

    # ---- shared code ------------------------------------------------------------------

    def test_writer_round_trip_and_lifecycle_are_the_probe_hle_measure_copies(self) -> None:
        for signature in self.SHARED_FUNCTIONS:
            with self.subTest(function=signature):
                self.assertEqual(HleMeasureProbeTests._function_body(self.probe, signature),
                                 HleMeasureProbeTests._function_body(self.measure, signature))
        for define in ('#define HLE_DONE_ID HLE_CAMPAIGN_ID "-done"',
                       "#define HLE_SENTINEL 0xA5A5A5A5u",
                       '#define HLE_SENTINEL_HEX "0xA5A5A5A5"',
                       '#define HLE_ROUNDTRIP_PATH "host0:/nakagawa_transport_write.bin"',
                       "#define HLE_ROUNDTRIP_BYTES 64u",
                       "#define HLE_MAIN_STOP_TIMEOUT_US 1000000u",
                       'PSP_MODULE_INFO("PSP_HLE_MEASURE", 0, 1, 0);'):
            with self.subTest(define=define):
                self.assertIn(define, self.probe)
                self.assertIn(define, self.measure)
        # Only the issue cases are built from this file.
        self.assertIn("#error probe_hle_issues.c has no case for this PSP_ORACLE_CASE", self.probe)

    def test_registrations_name_each_cases_test_id_and_log(self) -> None:
        for case, case_id, _stem, macro, test_id, log in self.CASES:
            with self.subTest(case=case):
                self.assertIn(f"#define {macro} {case_id}\n", self.probe)
                self.assertIn(f'#define HLE_CAMPAIGN_ID "{case}"\n#define HLE_TEST_ID "{test_id}"\n'
                              f'#define HLE_LOG "host0:/{log}"', self.probe)
                self.assertEqual(_campaign_host0_log_path(Path("/scratch"), case).name, log)
                self.assertEqual(_campaign_completeness_contract(case), "strict-golden-sequence")
                self.assertNotIn(case, CAMPAIGN_QUEUE_CASES)
        self.assertNotIn("hle-intr-cells", CAMPAIGN_PROBE_CASES)
        spec, _counts = CAMPAIGN_PROBE_CASES["hle-issues-batch"]
        self.assertEqual(spec.test_id, "PSP-HLE-ISSUES-001")

    def test_only_the_vfpu_cases_run_their_main_thread_with_the_vfpu_attribute(self) -> None:
        head = "#if PSP_ORACLE_CASE == 106 || PSP_ORACLE_CASE == 108 || PSP_ORACLE_CASE == 118\n"
        self.assertIn(head, self.probe)
        attr = self.probe[self.probe.index(head):]
        attr = attr[:attr.index("#endif") + len("#endif")]
        self.assertIn("PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);\n#else\n"
                      "PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);\n#endif", attr)
        self.assertNotIn("lv.q", self._section("HLE_CASE_INTR_CELLS"))

    # ---- source safety rules ----------------------------------------------------------

    def test_every_loop_and_wait_is_bounded(self) -> None:
        code = self.code
        self.assertEqual(sorted(re.findall(r"while \(([^{]*)\) \{", code)), sorted([
            "!s_hle_stop_requested", "offset < length",
            "(uint32_t)(sceKernelGetSystemTimeLow() - t) < hold",
            "(uint32_t)(sceKernelGetSystemTimeLow() - t0) < us",
        ]))
        for banned in ("for (;;)", "do {", "goto ", "malloc(", "sceKernelAllocPartitionMemory"):
            self.assertNotIn(banned, code)
        bound = r"(?:\d+u?|[A-Z][A-Z0-9_]*|sizeof\(\w+\)|first \+ n && k < 16u|count && .*|n)"
        # The one variable bound: the stored handler calls, capped at IC_TCALLS.
        self.assertIn("const uint32_t n = s_ic_tcount < IC_TCALLS ? s_ic_tcount : IC_TCALLS;", code)
        headers = [code[loop.end():_c_close(code, loop.end() - 1)]
                   for loop in re.finditer(r"\bfor \(", code)]
        self.assertGreaterEqual(len(headers), 15)
        for header in headers:
            with self.subTest(loop=header):
                self.assertRegex(header, rf"^(?:unsigned|int|uint32_t|size_t) (\w+) = [^;]+; "
                                         rf"(?:\1 < {bound}|\w+ < 100u && s_ic_vbl_count == c); "
                                         rf"\w+\+\+$")
        # The spins: the handler hold is 0 or 25 ms, a suspension is at most 50 ms, and
        # ic_spin_us runs only those windows and a 200 us settle.
        intr = _c_code(self._section("HLE_CASE_INTR_CELLS"))
        self.assertEqual(sorted(re.findall(r"s_ic_vbl_hold_us = (\w+);", intr)),
                         ["0u", "25000u"])
        self.assertEqual(sorted(int(us[:-1]) for us in
                                re.findall(r'ic_vbl_masked\("[\w-]+", (\d+u)\)', intr)),
                         [20000, 20000, 50000])
        self.assertEqual(sorted(re.findall(r"\bic_spin_us\((\w+)\);", intr)), ["200u", "us"])
        for delay in re.findall(r"sceKernelDelayThread\((\w+)\)", intr):
            with self.subTest(delay=delay):
                self.assertRegex(delay, r"^\d+$")
                self.assertLessEqual(int(delay), 250000)
        self.assertNotIn("sceKernelSleepThread", _c_code(self._section("HLE_CASE_INTR_CELLS")))
        self.assertNotIn("sceKernelSleepThread", _c_code(self._section("HLE_CASE_ISSUES_BATCH")))

    def test_each_cell_writes_its_step_before_its_calls(self) -> None:
        """Scanning each function in order, a measured call never follows a record (the end
        of a cell) without a new step; a helper with measured calls but no step of its own
        counts as a measured call where it is called. The VFPU instruction cells, which make
        no system call, write one step per group: per vscmp operand set and per C2 row of a
        carry grid (at most 56 cells)."""
        functions = _c_function_bodies(self.code)
        helpers = {name for name, body in functions.items()
                   if self.MEASURED_CALL_RE.search(body) and "hle_step(" not in body
                   and name not in self.HANDLERS}
        self.assertEqual(helpers, {"ic_vbl_align"})
        event_re = re.compile(r"\bhle_step\(|\bhle_record\(|" + self.MEASURED_CALL_RE.pattern
                              + r"|\b(?:" + "|".join(sorted(helpers)) + r")\(")
        checked = 0
        for name, body in functions.items():
            if name in self.HANDLERS or name in helpers:
                continue
            stepped = False
            for event in event_re.finditer(body):
                token = event.group(0)
                if token.startswith("hle_step("):
                    stepped = True
                elif token.startswith("hle_record("):
                    # A record ends its cell, unless it is a failure record and the
                    # function returns right after it.
                    rest = body[body.index(";", event.end()) + 1:].lstrip()
                    stepped = stepped and rest.startswith("return")
                else:
                    checked += 1
                    self.assertTrue(stepped, f"{name}: {token} is not after its cell's step")
        self.assertGreaterEqual(checked, 20)
        carry = functions["ic_rnd_carry"]
        self.assertLess(carry.index("for (unsigned i = 0; i < 8u; i++) {"), carry.index("hle_step(name);"))
        self.assertLess(carry.index("hle_step(name);"), carry.index("for (unsigned j = 0; j < 8u; j++) {"))
        self.assertLess(carry.index("for (unsigned k = 0; k < 7u; k++) {"), carry.index("ic_rnd_step("))
        vscmp = functions["ic_vscmp"]
        self.assertLess(vscmp.index("hle_step(name);"), vscmp.index("for (unsigned f = 0; f < 5u; f++) {"))

    def test_no_host_io_runs_inside_a_timed_window_or_a_handler(self) -> None:
        functions = _c_function_bodies(_c_code(self._section("HLE_CASE_INTR_CELLS")))
        for handler in self.HANDLERS:
            with self.subTest(handler=handler):
                body = functions[handler]
                self.assertNotRegex(body, r"\bhle_\w+\(")
                self.assertEqual(set(re.findall(r"\b(sce\w+)\s*\(", body)),
                                 {"sceKernelGetSystemTimeLow"})
        masked = functions["ic_vbl_masked"]
        window = masked[masked.index("sceKernelCpuSuspendIntr()"):
                        masked.index("sceKernelCpuResumeIntr(flags)")]
        self.assertNotRegex(window, r"\bhle_\w+\(")
        run = functions["hle_run"]
        hold = run[run.index("s_ic_vbl_hold_us = 25000u;"):]
        self.assertNotRegex(hold[:hold.index("ic_vbl_times(")], r"\bhle_\w+\(")
        for series in ("ic_vtimer_series", "ic_alarm_series"):
            with self.subTest(series=series):
                body = functions[series]
                timed = body[body.index("const uint32_t ref = "):body.index("ic_tcalls_rows(")]
                self.assertNotRegex(timed, r"\bhle_\w+\(")

    def test_every_handler_timer_alarm_and_suspension_is_removed_on_every_path(self) -> None:
        self.assertEqual(_hle_intr_cleanup_problems(self.probe), [])
        section = _c_code(self._section("HLE_CASE_INTR_CELLS"))
        self.assertIn("sceKernelRegisterSubIntrHandler(PSP_VBLANK_INT, s, (void *)ic_vbl_handler, NULL)",
                      section)
        self.assertIn("for (int s = 0; s < 16; s++) {", section)
        self.assertIn("if (slot < 0) {", section)

    def test_the_cleanup_check_refuses_each_leak(self) -> None:
        release = ("    if (slot >= 0) {\n        hle_step(\"vbl-release\");\n"
                   "        const int dis = sceKernelDisableSubIntr(PSP_VBLANK_INT, slot);\n"
                   "        const int rel = sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, slot);\n")
        self.assertIn(release, self.probe)
        # The measured 2026-10-24 revision released the handler only when Enable succeeded.
        measured_shape = re.sub(r'(ic_vbl_times\("vbl-hold-25ms", ref2, 0u, 8u, 1\);\n)    \}\n',
                                r"\1", self.probe, count=1)
        measured_shape = measured_shape.replace(
            '        hle_record("vbl-release", "PASS", (uint32_t)rel, w, 2);\n    }\n',
            '        hle_record("vbl-release", "PASS", (uint32_t)rel, w, 2);\n    }\n    }\n', 1)
        mutants = {
            "release-needs-enable": measured_shape,
            "no-release": self.probe.replace(
                "const int rel = sceKernelReleaseSubIntrHandler(PSP_VBLANK_INT, slot);",
                "const int rel = 0;", 1),
            "no-cancel": self.probe.replace("(void)sceKernelCancelAlarm(alarm);", "", 1),
            "no-delete": self.probe.replace("(void)sceKernelDeleteVTimer(vt);", "", 1),
            "early-return": self.probe.replace(
                "(void)sceKernelStartVTimer(vt);\n",
                "(void)sceKernelStartVTimer(vt);\n    if (first_us == 0u) {\n        return;\n    }\n", 1),
            "no-resume": self.probe.replace("sceKernelCpuResumeIntr(flags);", "", 1),
        }
        for name, mutant in mutants.items():
            with self.subTest(mutant=name):
                self.assertNotEqual(mutant, self.probe)
                self.assertNotEqual(_hle_intr_cleanup_problems(mutant), [], name)

    def test_only_the_interrupt_case_names_state_changing_stubs(self) -> None:
        forbidden = re.compile(r"sce\w*(?:Set|Install|Remove|Flush|Register|Reset)\w*")
        for macro, expected in (("HLE_CASE_INTR_CELLS", {"sceKernelSetAlarm",
                                                          "sceKernelSetVTimerHandler",
                                                          "sceKernelRegisterSubIntrHandler"}),
                                ("HLE_CASE_ISSUES_BATCH", set())):
            with self.subTest(section=macro):
                names = set(re.findall(r"\b(sce\w+)\b", _c_code(self._section(macro))))
                self.assertEqual({name for name in names if forbidden.fullmatch(name)}, expected)
        shared = set(re.findall(r"\b(sce\w+)\b", _c_code(self._shared())))
        self.assertEqual({name for name in shared if forbidden.fullmatch(name)}, set())

    def test_the_kernel_getter_call_is_pinned_and_guarded(self) -> None:
        section = _c_code(self._section("HLE_CASE_ISSUES_BATCH"))
        self.assertEqual(section.count("kuKernelCall("), 1)
        self.assertIn("static const uint32_t nids[2] = {0x07c586a1u, 0x6373995du};", section)
        self.assertIn('sctrlHENFindFunction("sceSystemMemoryManager", "SysMemForKernel", nids[n])',
                      section)
        guard = section.index("if (addr >= 0x88000000u && addr < 0x8c000000u) {")
        self.assertLess(guard, section.index("kuKernelCall("))
        self.assertLess(section.index("kuKernelCall("), _c_close(section, section.index("{", guard)))
        self.assertIn("arg.ret1 = HLE_SENTINEL;", section)
        # The hand-written user import: SysMemUserForUser, NID 0x6373995d, PSPSDK stub flags.
        for fragment in ('".word 0x40010000\\n\\t"', '".word 0x6373995d\\n\\t"',
                         '".asciz \\"SysMemUserForUser\\"\\n\\t"'):
            self.assertIn(fragment, section)
        self.assertEqual(HleMeasureProbeTests._nid("sceKernelGetModel"), 0x6373995D)

    def test_record_shapes_and_grids_match_the_registry(self) -> None:
        section = _c_code(self._section("HLE_CASE_ISSUES_BATCH"))
        _spec, counts = CAMPAIGN_PROBE_CASES["hle-issues-batch"]
        self.assertIn('hle_record(case_id, "PASS", v0, w, 3);', section)
        self.assertIn('hle_record(name, "PASS", arg.ret1, w, 3);', section)
        self.assertIn('hle_record(name, "PASS", out[0], w, 17);', section)
        self.assertIn('hle_record(name, "PASS", s_ic_vscmp_forms[f].word, w, 16);', section)
        self.assertEqual({counts[case] for case in parse_golden.HLE_ISSUES_MODEL_CASES}, {3})
        self.assertEqual(counts["vscmp-p1-nan-mixed-vscmp.q-same"], 16)
        self.assertEqual(counts["rcbig-p1-7-7-6"], 17)
        c2s, c3s, carries = parse_golden.HLE_ISSUES_CARRY_GRID
        for fragment in (f"s_ic_c2[{c2s}]", f"s_ic_c3[{c3s}]", f"s_ic_carry[{carries}]",
                         f"s_ic_carry_big[{carries}]",
                         f"s_ic_vscmp_sets[{len(parse_golden.HLE_ISSUES_VSCMP_SETS)}]",
                         f"s_ic_vscmp_forms[{len(parse_golden.HLE_ISSUES_VSCMP_FORMS)}]",
                         f"pass < {parse_golden.HLE_ISSUES_PASSES}u"):
            self.assertIn(fragment, section)
        for name in parse_golden.HLE_ISSUES_VSCMP_SETS:
            self.assertIn(f'{{"{name}", {{', section)
        for form in parse_golden.HLE_ISSUES_VSCMP_FORMS:
            self.assertIn(f'{{"{form}", 0x', section)
        for name in parse_golden.HLE_ISSUES_MODEL_CASES[:4]:
            self.assertIn(f'ic_model_row("{name}", ', section)
        self.assertIn('"model-kernel-get-model-%08x"', section)
        self.assertIn('"%s-p%u-%u-%u-%u", stem, pass, i, j, k', section)
        self.assertIn('ic_rnd_carry(pass, "rcar", s_ic_carry);\n        '
                      'ic_rnd_carry(pass, "rcbig", s_ic_carry_big);', section)
        self.assertLess(section.index("ic_vscmp(pass);"), section.index('ic_rnd_carry(pass, "rcar"'))
        self.assertEqual(len(parse_golden.HLE_ISSUES_BATCH_CASES), 1858)

    def test_interrupt_cell_names_and_out_counts_match_the_parser(self) -> None:
        section = _c_code(self._section("HLE_CASE_INTR_CELLS"))
        self.assertEqual(tuple(re.findall(r'ic_(?:vtimer|alarm)_series\("([\w-]+)"', section)),
                         parse_golden.HLE_INTR_SERIES)
        self.assertIn(f"#define IC_TCALLS {parse_golden.HLE_INTR_TCALLS}u", self.probe)
        self.assertEqual(tuple(re.findall(r'hle_step\("(vbl-[\w-]+)"\)|ic_vbl_masked\("([\w-]+)"',
                                          section)),
                         (("vbl-register", ""), ("vbl-free", ""), ("", "vbl-masked-20ms"),
                          ("", "vbl-masked-50ms"), ("", "vbl-masked-20ms-again"),
                          ("vbl-hold-25ms", ""), ("vbl-release", "")))
        counts = parse_golden.HLE_INTR_OUT_COUNTS
        self.assertIn('hle_record("vbl-register", slot >= 0 && en >= 0 ? "PASS" : "FAIL", '
                      f'(uint32_t)slot, w, {counts["vbl-register"]});', section)
        self.assertIn(f'hle_record("vbl-release", "PASS", (uint32_t)rel, w, {counts["vbl-release"]});',
                      section)
        self.assertIn(f"uint32_t w[{counts['vbl-masked-20ms']}] = ", section)
        self.assertIn(f'ic_vbl_times("vbl-free", ref, 0u, {counts["vbl-free"]}u, 0);', section)
        self.assertIn(f'ic_vbl_times("vbl-hold-25ms", ref2, 0u, {counts["vbl-hold-25ms"] // 2}u, 1);',
                      section)
        self.assertIn('hle_record(row, "PASS", i + 1u, w, 4);', section)
        self.assertIn('hle_record(name, "PASS", calls_at_end, w, 3);', section)
        self.assertIn('hle_record(row, "PASS", later - at_end, w, 1);', section)
        self.assertIn('hle_record(name, "FAIL", (uint32_t)vt, NULL, 0);', section)

    # ---- manifest -----------------------------------------------------------------------

    WRAPPER_RE = re.compile(r"^IC_V0_WRAPPER\(\w+, (\w+)\);$", re.MULTILINE)

    def _calls(self, macro: str) -> set[str]:
        text = _c_code(self._shared() + self._section(macro))
        calls = set(re.findall(r"\b((?:sce|ku|sctrl)\w+)\s*\(", text))
        calls |= {target[3:] if target.startswith("ic_") else target
                  for target in self.WRAPPER_RE.findall(text)}
        return calls

    def test_manifest_rows_are_not_run_and_name_every_import(self) -> None:
        by_id = {test["id"]: test for test in self.manifest["tests"]}
        for case, _cid, stem, macro, test_id, _log in self.CASES:
            with self.subTest(case=case):
                self.assertEqual(sum(1 for test in self.manifest["tests"] if test["id"] == test_id), 1)
                row = by_id[test_id]
                self.assertEqual(row["status"], "implemented")
                self.assertEqual(row["hardware_evidence"], "NOT_RUN")
                for absent in ("evidence_ref", "evidence_cases", "measurement_note"):
                    self.assertNotIn(absent, row)
                self.assertIn("No acceptance-eligible capture exists",
                              row["evidence_note"])
                self.assertIn("not in the campaign queue", row["evidence_note"])
                self.assertEqual(row["source"], "fixtures/psp_oracle/probe_hle_issues.c")
                self.assertEqual(row["prx"], f"{stem}.prx")
                self.assertEqual(row["diagnostic_case_ids"], [case])
                self.assertEqual(row["reset"], "one PRX launch; PSPLink soft reset before the next launch")
                listed = {api for api in row["apis"] if not api.startswith("Allegrex ")}
                self.assertEqual(sorted(listed), sorted(self._calls(macro)))
                self.assertEqual(row["case_ids"][-1], f"{case}-done")
        self.assertEqual(by_id["PSP-HLE-ISSUES-001"]["issues"], [326, 356])
        self.assertEqual(by_id["PSP-HLE-INTR-001"]["issues"], [325])

    def test_manifest_case_patterns_cover_each_stream_in_order(self) -> None:
        by_id = {test["id"]: test for test in self.manifest["tests"]}
        spec, _counts = CAMPAIGN_PROBE_CASES["hle-issues-batch"]
        streams = {
            "PSP-HLE-ISSUES-001": list(spec.ordered_cases),
            "PSP-HLE-INTR-001": [case_id for case_id, *_r in self._intr_records(calls=(8, 8, 8, 8, 8))],
        }
        for test_id, cases in streams.items():
            with self.subTest(test_id=test_id):
                patterns = [re.compile(re.escape(p).replace(r"\*", ".+"))
                            for p in by_id[test_id]["case_ids"]]
                position = 0
                for case_id in cases:
                    while position < len(patterns) and not patterns[position].fullmatch(case_id):
                        position += 1
                    self.assertLess(position, len(patterns), f"{case_id} is not covered in order")

    # ---- parsers ------------------------------------------------------------------------

    def test_issues_batch_stream_is_strict_and_pass_only(self) -> None:
        helper = HleMeasureProbeTests("test_case_ids_are_registered_once_and_match_the_families")
        text = helper._stream("hle-issues-batch")
        body = helper._body(text)
        self.assertTrue(parse_campaign_probe_output(body, "hle-issues-batch").complete)
        self.assertTrue(_campaign_stream_complete(text, "hle-issues-batch"))
        spec, counts = CAMPAIGN_PROBE_CASES["hle-issues-batch"]
        self.assertEqual(len(spec.semantic_cases), 1858)
        skipped = helper._stream("hle-issues-batch", {"rcar-p0-3-4-5": ("SKIP", 0, [0] * 17)})
        with self.assertRaisesRegex(ProtocolError, "unconditional observation"):
            parse_campaign_probe_output(helper._body(skipped), "hle-issues-batch")
        short = helper._stream("hle-issues-batch", {"vscmp-p0-inf-inf-vscmp.t": ("PASS", 0, [0] * 15)})
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(helper._body(short), "hle-issues-batch")
        truncated = "\n".join(text.splitlines()[:-200]) + "\n"
        self.assertFalse(_campaign_stream_complete(truncated, "hle-issues-batch"))

    @staticmethod
    def _intr_records(calls=(6, 5, 4, 6, 5), failed_vtimers=(), register="enabled"):
        records = []
        for name, count in zip(parse_golden.HLE_INTR_SERIES, calls, strict=True):
            if name in failed_vtimers:
                records.append((name, "FAIL", 0x80020190, []))
                continue
            for i in range(min(count, parse_golden.HLE_INTR_TCALLS)):
                records.append((f"{name}-call{i}", "PASS", i + 1, [0x1000 * i, 0, 0, 0]))
            records.append((name, "PASS", count, [count, 0, 0]))
            records.append((f"{name}-after-zero", "PASS", 0, [count]))
        tries = [0x80020067, 0] + [0] * 14
        if register == "enabled":
            records.append(("vbl-register", "PASS", 1, [0] + tries))
            for cell in parse_golden.HLE_INTR_VBL_CELLS:
                records.append((cell, "PASS", 0, [0] * parse_golden.HLE_INTR_OUT_COUNTS[cell]))
            records.append(("vbl-release", "PASS", 0, [0, 0]))
        elif register == "enable-refused":
            records.append(("vbl-register", "FAIL", 1, [0x80020065] + tries))
            records.append(("vbl-release", "PASS", 0, [0, 0]))
        else:  # no slot
            records.append(("vbl-register", "FAIL", 0xFFFFFFFF, [0xFFFFFFFF] + [0x80020067] * 16))
        return records

    @staticmethod
    def _intr_text(records, done=None) -> str:
        lines = ["NAKAGAWA_PSP_META schema=1 source=psp model=unknown firmware=unknown "
                 "binary_sha256=" + "0" * 64 + " source_commit=" + "b" * 40 + " fixture=hle-measure"]
        for case_id, status, result, outs in records:
            lines.append(f"NAKAGAWA_PSP_STEP schema=1 case_id=hle-intr-cells step={case_id}")
            fields = " ".join(f"out{i}=0x{value:x}" for i, value in enumerate(outs))
            lines.append(f"NAKAGAWA_PSP_TEST schema=1 test_id=PSP-HLE-INTR-001 case_id={case_id} "
                         f"status={status} result=0x{result:x} {fields}".rstrip())
        count = len(records) if done is None else done
        lines.append("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-HLE-INTR-001 case_id=hle-intr-cells-done "
                     f"status=PASS result=0x0 out0=0x{count:x}")
        return "\n".join(lines) + "\n"

    def test_intr_parser_accepts_every_shape_the_probe_can_emit(self) -> None:
        shapes = {
            "measured-like": self._intr_records(),
            "no-calls": self._intr_records(calls=(0, 0, 0, 0, 0)),
            "more-calls-than-stored": self._intr_records(calls=(12, 9, 8, 30, 8)),
            "vtimer-create-failed": self._intr_records(failed_vtimers=("vtimer-ret-1us",)),
            "enable-refused": self._intr_records(register="enable-refused"),
            "no-slot": self._intr_records(register="no-slot"),
        }
        for name, records in shapes.items():
            with self.subTest(shape=name):
                text = self._intr_text(records)
                self.assertTrue(parse_hle_intr_cells_output(text).complete)
                full = text + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
                self.assertTrue(_campaign_stream_complete(full, "hle-intr-cells"))

    def test_intr_parser_refuses_bookkeeping_damage(self) -> None:
        good = self._intr_records()

        def edit(case_id, **change):
            records = []
            for record in good:
                if record[0] == case_id:
                    record = (change.get("case_id", record[0]), change.get("status", record[1]),
                              change.get("result", record[2]), change.get("outs", record[3]))
                records.append(record)
            return records

        mutants = {
            "call-count": edit("vtimer-ret-20ms", outs=[7, 0, 0]),
            "sequence": edit("alarm-ret-1us-call2", result=7),
            "short-row": edit("vbl-masked-50ms", outs=[0] * 13),
            "failed-cell": edit("vbl-free", status="FAIL"),
            "alarm-fail": edit("alarm-ret-20ms", status="FAIL"),
            "register-status": edit("vbl-register", status="FAIL"),
            "slot-range": edit("vbl-register", result=16),
            "missing-release": [r for r in good if r[0] != "vbl-release"],
            "missing-after-zero": [r for r in good if r[0] != "vtimer-ret-mixed-after-zero"],
            "cells-without-slot": [r for r in self._intr_records(register="no-slot")]
            + [("vbl-free", "PASS", 0, [0] * 12)],
            "reordered": good[:3] + [good[4], good[3]] + good[5:],
        }
        for name, records in mutants.items():
            with self.subTest(mutant=name):
                with self.assertRaises(ProtocolError):
                    parse_hle_intr_cells_output(self._intr_text(records))
        with self.assertRaises(ProtocolError):
            parse_hle_intr_cells_output(self._intr_text(good, done=len(good) - 1))
        text = self._intr_text(good)
        truncated = "\n".join(text.splitlines()[:-6]) + "\n"
        self.assertFalse(parse_hle_intr_cells_output(truncated, require_complete=False).complete)
        with self.assertRaises(ProtocolError):
            parse_hle_intr_cells_output(truncated)

    # ---- docs -----------------------------------------------------------------------------

    def test_docs_list_both_cases_as_observed_but_not_acceptance_eligible(self) -> None:
        hardware = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        readme = (self.fixture / "README.md").read_text(encoding="utf-8")
        for case, _cid, _stem, _macro, test_id, _log in self.CASES:
            with self.subTest(case=case):
                self.assertIn(f"| `{case}` | `{test_id}` | `NOT_RUN` (observed 2026-10-24 from "
                              "`da97b2c0` without a runner envelope, so not acceptance-eligible; "
                              "single launch, not in the campaign queue) |", hardware)
                self.assertIn(f"| `{case}` | `{test_id}` |", readme)
        self.assertIn("probe_hle_issues.c", readme)


class VramMirrorParserTests(unittest.TestCase):
    TEST_ID = "PSP-VRAM-MIRROR-001"
    BASES = parse_golden.VRAM_MIRROR_BASES
    WRITE_BASES = parse_golden.VRAM_MIRROR_WRITE_BASES

    def test_vram_mirror_parser_is_registered(self) -> None:
        self.assertTrue(callable(getattr(parse_golden, "parse_vram_mirror_output", None)))

    def _record(self, case_id: str, outputs: tuple[int, ...] = (), *,
                status: str = "PASS", result: int = 0) -> str:
        fields = " ".join(
            f"out{index}=0x{value:08x}" for index, value in enumerate(outputs)
        )
        suffix = f" {fields}" if fields else ""
        return (
            f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} "
            f"case_id={case_id} status={status} result=0x{result:08x}{suffix}\n"
        )

    def _address_pairs(
        self, offsets: tuple[int, ...], value: int,
        aliases: tuple[int, ...] | None = None,
    ) -> tuple[int, ...]:
        selected_aliases = self.BASES if aliases is None else aliases
        return tuple(
            field
            for offset in offsets
            for base in selected_aliases
            for field in (base + offset, value)
        )

    def _stream(
        self,
        *,
        observed_word: int = 0xDEADBEEF,
        skipped: bool = False,
        measured_rules: bool = False,
        depth_storage_xor: int | None = None,
        wrong_write_landing: bool = False,
    ) -> str:
        spec = parse_golden.VRAM_MIRROR_SPEC
        color_frame_bytes = parse_golden.VRAM_MIRROR_FRAME_BYTES
        display_bytes = color_frame_bytes * (2 if measured_rules else 1)
        color_offset = display_bytes
        depth_offset = color_offset + color_frame_bytes
        if measured_rules and depth_storage_xor is None:
            depth_storage_xor = MEASURED_VRAM_DEPTH_BYTE_XOR
        info_values = {
            "cached_base": self.BASES[0],
            "uncached_base": self.BASES[4],
            "display_offset": 0,
            "display_bytes": display_bytes,
            "display_stride": 512,
            "display_format": 3 if measured_rules else 0,
            "color_offset": color_offset,
            "color_frame_bytes": parse_golden.VRAM_MIRROR_FRAME_BYTES,
            "depth_offset": depth_offset,
            "depth_frame_bytes": parse_golden.VRAM_MIRROR_FRAME_BYTES,
            "vram_bytes": parse_golden.VRAM_MIRROR_BYTES,
            "cpu_sample_word_count": parse_golden.VRAM_MIRROR_CPU_SAMPLE_WORD_COUNT,
            "cpu_dense_offset": parse_golden.VRAM_MIRROR_DENSE_OFFSET,
            "cpu_dense_rows": parse_golden.VRAM_MIRROR_DENSE_ROWS,
            "cpu_row_bytes": parse_golden.VRAM_MIRROR_ROW_BYTES,
            "cpu_pair_region_count": len(parse_golden.VRAM_MIRROR_PAIR_BASES),
            "cpu_pair_rows": parse_golden.VRAM_MIRROR_PAIR_ROWS,
            "cpu_sweep_first_bit": parse_golden.VRAM_MIRROR_SWEEP_FIRST_BIT,
            "cpu_sweep_last_bit": parse_golden.VRAM_MIRROR_SWEEP_LAST_BIT,
            "write_alias_count": len(self.WRITE_BASES),
            "write_sample_count": len(parse_golden.VRAM_MIRROR_WRITE_OFFSETS),
            "write_offset_0": parse_golden.VRAM_MIRROR_WRITE_OFFSETS[0],
            "write_offset_1": parse_golden.VRAM_MIRROR_WRITE_OFFSETS[1],
            "write_offset_2": parse_golden.VRAM_MIRROR_WRITE_OFFSETS[2],
            "write_scan_bytes": parse_golden.VRAM_MIRROR_BYTES,
            "depth_stride": parse_golden.VRAM_MIRROR_DEPTH_STRIDE,
            "depth_row_count": len(parse_golden.VRAM_MIRROR_DEPTH_ROWS),
            **{
                f"depth_row_y{row}": y
                for row, y in enumerate(parse_golden.VRAM_MIRROR_DEPTH_ROWS)
            },
            "depth_alias_count": len(parse_golden.VRAM_MIRROR_DEPTH_ALIASES),
            "depth_chunk_pixels": parse_golden.VRAM_MIRROR_DEPTH_CHUNK_PIXELS,
            "color_row_count": len(parse_golden.VRAM_MIRROR_COLOR_ROWS),
            **{
                f"color_row_y{row}": y
                for row, y in enumerate(parse_golden.VRAM_MIRROR_COLOR_ROWS)
            },
            "color_chunk_pixels": parse_golden.VRAM_MIRROR_COLOR_CHUNK_PIXELS,
            "depth_test_enabled": 1,
            "depth_function": parse_golden.VRAM_MIRROR_DEPTH_FUNCTION_ALWAYS,
            "depth_mask": parse_golden.VRAM_MIRROR_DEPTH_MASK_WRITE_ENABLE,
            "depth_range_near": parse_golden.VRAM_MIRROR_DEPTH_RANGE_NEAR,
            "depth_range_far": parse_golden.VRAM_MIRROR_DEPTH_RANGE_FAR,
            "clear_depth": parse_golden.VRAM_MIRROR_CLEAR_DEPTH,
            "pattern_prefix": parse_golden.VRAM_MIRROR_PATTERN_PREFIX,
            "marker_prefix": parse_golden.VRAM_MIRROR_MARKER_PREFIX,
            "cached_alias_count": parse_golden.VRAM_MIRROR_CACHED_ALIAS_COUNT,
        }
        info = info_values
        depth_writes = None
        if depth_storage_xor is not None:
            depth_writes = {}
            for sprite_case in parse_golden.VRAM_MIRROR_GE_SYNC_CASES:
                if sprite_case[1] == 0:
                    continue
                x0, y0, z0, x1, y1, z1 = sprite_case[2:8]
                if z0 != z1:
                    raise AssertionError("synthetic sprite Z endpoints must agree")
                for source_y in range(y0, y1):
                    for source_x in range(x0, x1):
                        linear = source_y * 1024 + 2 * source_x
                        depth_writes[depth_offset + (linear ^ depth_storage_xor)] = z0
        rows = [META.format(
            source="psp", model="PSP-3000", firmware="6.61-ARK",
            binary=MEASURED_SHA, commit=MEASURED_COMMIT,
        )]
        for case_id in spec.ordered_cases:
            if skipped:
                terminal = case_id == spec.terminal_case
                rows.append(self._record(
                    case_id,
                    (len(spec.semantic_cases),) if terminal else (),
                    status="SKIP", result=4,
                ))
                continue
            if case_id == "vram-mirror-info":
                outputs = tuple(info_values[field]
                                for field in parse_golden.VRAM_MIRROR_INFO_FIELDS)
            elif match := re.fullmatch(r"vram-cpu-pattern-(\d{3})", case_id):
                chunk = int(match.group(1))
                first = chunk * parse_golden.VRAM_MIRROR_SAMPLE_CHUNK_WORDS
                offsets = parse_golden._vram_mirror_sample_offsets()[
                    first:first + parse_golden.VRAM_MIRROR_SAMPLE_CHUNK_WORDS
                ]
                if measured_rules:
                    outputs = tuple(
                        field
                        for offset in offsets
                        for alias in self.BASES
                        for field in (
                            alias + offset,
                            parse_golden.VRAM_MIRROR_PATTERN_PREFIX
                            | _apply_measured_vram_alias_rule(alias, offset),
                        )
                    )
                else:
                    outputs = self._address_pairs(offsets, observed_word)
            elif match := re.fullmatch(r"vram-cpu-write-mirror-(\d{2})-(\d{2})", case_id):
                alias, sample = (int(part) for part in match.groups())
                offset = parse_golden.VRAM_MIRROR_WRITE_OFFSETS[sample]
                if measured_rules:
                    found_offset = _apply_measured_vram_alias_rule(
                        self.WRITE_BASES[alias], offset
                    )
                    if wrong_write_landing and alias == 0 and sample == 0:
                        found_offset ^= 4
                    found_count = 1
                else:
                    found_offset = (
                        0xFFFFFFFF if alias == 0 and sample == 0
                        else 0x12000 + alias * 0x100 + sample * 4
                    )
                    found_count = 0 if found_offset == 0xFFFFFFFF else 1
                outputs = (
                    self.WRITE_BASES[alias] + offset,
                    parse_golden.VRAM_MIRROR_MARKER_PREFIX | (alias << 20) | offset,
                    found_offset,
                    found_count,
                )
            elif case_id in {item[0] for item in parse_golden.VRAM_MIRROR_GE_SYNC_CASES}:
                sync_case = next(
                    item for item in parse_golden.VRAM_MIRROR_GE_SYNC_CASES
                    if item[0] == case_id
                )
                outputs = (
                    0, 0, 4, 123, 1, 0, 1,
                    parse_golden.VRAM_MIRROR_CACHED_ALIAS_COUNT,
                    info["depth_offset"], parse_golden.VRAM_MIRROR_DEPTH_STRIDE,
                    parse_golden.VRAM_MIRROR_DEPTH_RANGE_NEAR,
                    parse_golden.VRAM_MIRROR_DEPTH_RANGE_FAR,
                    parse_golden.VRAM_MIRROR_DEPTH_FUNCTION_ALWAYS,
                    parse_golden.VRAM_MIRROR_DEPTH_MASK_WRITE_ENABLE,
                    1, parse_golden.VRAM_MIRROR_CLEAR_DEPTH,
                    *sync_case[1:],
                )
            elif match := re.fullmatch(r"vram-ge-depth-row(\d+)-(\d{2})", case_id):
                row, chunk = (int(part) for part in match.groups())
                y = parse_golden.VRAM_MIRROR_DEPTH_ROWS[row]
                offsets = tuple(
                    info["depth_offset"] + (
                        y * parse_golden.VRAM_MIRROR_DEPTH_STRIDE
                        + chunk * parse_golden.VRAM_MIRROR_DEPTH_CHUNK_PIXELS + pixel
                    ) * 2
                    for pixel in range(parse_golden.VRAM_MIRROR_DEPTH_CHUNK_PIXELS)
                )
                if depth_storage_xor is None:
                    outputs = self._address_pairs(
                        offsets, observed_word & 0xFFFF,
                        aliases=parse_golden.VRAM_MIRROR_DEPTH_ALIASES,
                    )
                else:
                    output_fields = []
                    for offset in offsets:
                        for alias in parse_golden.VRAM_MIRROR_DEPTH_ALIASES:
                            physical = (
                                _apply_measured_vram_alias_rule(alias, offset)
                                if measured_rules else offset
                            )
                            output_fields.extend((
                                alias + offset,
                                depth_writes.get(
                                    physical, parse_golden.VRAM_MIRROR_CLEAR_DEPTH
                                ),
                            ))
                    outputs = tuple(output_fields)
            elif match := re.fullmatch(r"vram-ge-color-row(\d+)-(\d{2})", case_id):
                row, chunk = (int(part) for part in match.groups())
                y = parse_golden.VRAM_MIRROR_COLOR_ROWS[row]
                offsets = tuple(
                    info["color_offset"] + (
                        y * parse_golden.VRAM_MIRROR_DEPTH_STRIDE
                        + chunk * parse_golden.VRAM_MIRROR_COLOR_CHUNK_PIXELS + pixel
                    ) * 2
                    for pixel in range(parse_golden.VRAM_MIRROR_COLOR_CHUNK_PIXELS)
                )
                outputs = self._address_pairs(
                    offsets, observed_word & 0xFFFF,
                    aliases=(parse_golden.VRAM_MIRROR_COLOR_ALIAS,),
                )
            elif case_id == "vram-restore":
                outputs = (
                    0x44000000, 0x200000,
                    2 * parse_golden.VRAM_MIRROR_CACHED_ALIAS_COUNT, 0, 1,
                )
            else:
                outputs = (len(spec.semantic_cases),)
            rows.append(self._record(case_id, outputs))
        return "".join(rows)

    def test_complete_stream_checks_shape_and_keeps_hardware_words_unpinned(self) -> None:
        parse = parse_golden.parse_vram_mirror_output
        report = parse(self._stream(), require_complete=True)
        self.assertTrue(report.complete)
        self.assertTrue(report.all_passed)
        changed = parse(self._stream(observed_word=0x13579BDF), require_complete=True)
        self.assertTrue(changed.all_passed)

    def test_analysis_derives_measured_alias_maps_write_landings_and_depth_xor(self) -> None:
        parsed = parse_golden.parse_vram_mirror_output(
            self._stream(measured_rules=True), require_complete=True
        )
        analysis = parse_golden.analyze_vram_mirror_run(parsed)
        transforms = {item.alias_base: item for item in analysis.address_transforms}
        self.assertEqual(set(transforms), set(MEASURED_VRAM_ALIAS_RULES))
        for alias_base, (constant, columns) in MEASURED_VRAM_ALIAS_RULES.items():
            with self.subTest(alias=hex(alias_base)):
                self.assertEqual(transforms[alias_base].constant, constant)
                self.assertEqual(transforms[alias_base].column_map, columns)
        self.assertEqual(len(analysis.write_landings), 18)
        self.assertTrue(all(
            item.unique_match_agrees_with_read_transform is True
            for item in analysis.write_landings
        ))
        first_swizzled_write = next(
            item for item in analysis.write_landings
            if item.alias_base == 0x04200000 and item.requested_offset == 0x100
        )
        self.assertEqual(first_swizzled_write.first_found_offset, 0x2140)
        self.assertEqual(
            analysis.depth_storage.relative_xor_candidates,
            MEASURED_VRAM_DEPTH_XOR_CANDIDATES,
        )
        self.assertEqual(
            analysis.depth_storage.absolute_xor_candidates,
            MEASURED_VRAM_DEPTH_XOR_CANDIDATES,
        )
        self.assertEqual(
            analysis.depth_storage.relative_xor_common_bits,
            MEASURED_VRAM_DEPTH_BYTE_XOR,
        )
        self.assertEqual(
            analysis.depth_storage.absolute_xor_common_bits,
            MEASURED_VRAM_DEPTH_BYTE_XOR,
        )
        self.assertEqual(
            analysis.depth_storage.relative_xor_candidates[0]
            ^ analysis.depth_storage.relative_xor_candidates[-1],
            MEASURED_VRAM_DEPTH_PHASE_AMBIGUITY,
        )
        self.assertEqual(
            analysis.depth_storage.absolute_vs_relative_scope_ambiguous,
            MEASURED_VRAM_DEPTH_SCOPE_AMBIGUOUS,
        )
        self.assertGreater(analysis.depth_storage.alias_crosschecked_words, 0)

    def test_analysis_rejects_a_non_affine_alias_sample(self) -> None:
        lines = self._stream(measured_rules=True).splitlines()
        sample_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-cpu-pattern-000 " in line
        )
        mapped = _apply_measured_vram_alias_rule(0x04200000, 0x088000)
        wrong_word = parse_golden.VRAM_MIRROR_PATTERN_PREFIX | (mapped ^ 4)
        lines[sample_index] = re.sub(
            r"out3=0x[0-9a-f]+", f"out3=0x{wrong_word:08x}",
            lines[sample_index], count=1,
        )
        parsed = parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")
        with self.assertRaisesRegex(ProtocolError, "not affine over GF\\(2\\)"):
            parse_golden.analyze_vram_mirror_run(parsed)

    def test_analysis_reports_wrong_write_and_depth_rules_differently(self) -> None:
        write_run = parse_golden.parse_vram_mirror_output(
            self._stream(measured_rules=True, wrong_write_landing=True)
        )
        write_analysis = parse_golden.analyze_vram_mirror_run(write_run)
        mismatched_write = next(
            item for item in write_analysis.write_landings
            if item.alias_base == 0x04200000 and item.requested_offset == 0x100
        )
        self.assertIs(mismatched_write.unique_match_agrees_with_read_transform, False)

        depth_run = parse_golden.parse_vram_mirror_output(
            self._stream(measured_rules=True, depth_storage_xor=0x40)
        )
        depth_analysis = parse_golden.analyze_vram_mirror_run(depth_run)
        self.assertNotEqual(
            depth_analysis.depth_storage.relative_xor_candidates,
            (MEASURED_VRAM_DEPTH_BYTE_XOR,),
        )

    def test_position_sample_plan_includes_dense_pair_and_bit_sweep_words(self) -> None:
        offsets = parse_golden._vram_mirror_sample_offsets()
        self.assertEqual(len(offsets), 4134)
        self.assertEqual(offsets[0], 0x088000)
        self.assertEqual(offsets[2047], 0x089FFC)
        self.assertIn(0x1FFFFC, offsets)
        for bit in range(2, 21):
            self.assertIn(0x088000 ^ (1 << bit), offsets)
            self.assertIn(1 << bit, offsets)

    def test_wrong_addresses_and_truncated_chunks_are_rejected(self) -> None:
        stream = self._stream()
        lines = stream.splitlines()
        sample_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-cpu-pattern-000 " in line
        )
        lines[sample_index] = lines[sample_index].replace(
            "out0=0x04088000", "out0=0x04088004", 1
        )
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

        lines = stream.splitlines()
        sample_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-cpu-pattern-000 " in line
        )
        lines[sample_index] = re.sub(r" out127=0x[0-9a-f]+$", "", lines[sample_index])
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

        lines = self._stream().splitlines()
        info_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-mirror-info " in line
        )
        lines[info_index] = lines[info_index].replace(
            "out3=0x00044000", "out3=0x00044100", 1
        )
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

    def test_write_scan_outcomes_are_not_pinned(self) -> None:
        lines = self._stream().splitlines()
        write_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-cpu-write-mirror-00-00 " in line
        )
        lines[write_index] = lines[write_index].replace(
            "out2=0xffffffff out3=0x00000000",
            "out2=0x00012340 out3=0x00000002",
            1,
        )
        self.assertTrue(
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n").all_passed
        )

        lines[write_index] = lines[write_index].replace(
            "out2=0x00012340 out3=0x00000002",
            "out2=0xffffffff out3=0x00000001",
            1,
        )
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

    def test_ge_state_and_depth_row_addresses_are_checked_without_pinning_values(self) -> None:
        lines = self._stream().splitlines()
        sync_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-ge-clear-sync " in line
        )
        lines[sync_index] = lines[sync_index].replace(
            "out12=0x00000001", "out12=0x00000000", 1
        )
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

        lines = self._stream().splitlines()
        depth_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-ge-depth-row0-00 " in line
        )
        lines[depth_index] = re.sub(
            r"out0=0x[0-9a-f]+", "out0=0x44088002", lines[depth_index], count=1
        )
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

        lines = self._stream().splitlines()
        color_index = next(
            index for index, line in enumerate(lines)
            if "case_id=vram-ge-color-row0-00 " in line
        )
        lines[color_index] = re.sub(
            r"out0=0x[0-9a-f]+", "out0=0x44044002", lines[color_index], count=1
        )
        with self.assertRaises(ProtocolError):
            parse_golden.parse_vram_mirror_output("\n".join(lines) + "\n")

        changed = self._stream(observed_word=0x1BADB002)
        self.assertTrue(parse_golden.parse_vram_mirror_output(changed).all_passed)

    def test_setup_skip_is_complete_but_never_reports_a_pass(self) -> None:
        report = parse_golden.parse_vram_mirror_output(
            self._stream(skipped=True), require_complete=True
        )
        self.assertTrue(report.complete)
        self.assertFalse(report.all_passed)

    def test_probe_case_is_in_the_build_and_runner_registry(self) -> None:
        root = Path(__file__).resolve().parents[1]
        makefile = (root / "fixtures" / "psp_oracle" / "Makefile").read_text(
            encoding="utf-8"
        )
        probe = (root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("else ifeq ($(CASE),vram-mirror)\nCASE_ID = 76\n", makefile)
        self.assertIn("#define PSP_ORACLE_CASE_VRAM_MIRROR 76", probe)
        self.assertIn('#define PROBE_HOST0_LOG "host0:/vram_mirror_log.txt"', probe)
        self.assertIn("vram-mirror", CAMPAIGN_QUEUE_CASES)
        from psp_oracle.run_psplink import CAMPAIGN_CASE_ESTIMATE_SECONDS

        self.assertIn("vram-mirror", CAMPAIGN_CASE_ESTIMATE_SECONDS)
        self.assertEqual(_campaign_completeness_contract("vram-mirror"),
                         "strict-golden-sequence")
        self.assertEqual(_campaign_host0_log_path(Path("host0"), "vram-mirror").name,
                         "vram_mirror_log.txt")
        self.assertTrue(_campaign_stream_complete(self._stream(), "vram-mirror"))

    def test_probe_case_ids_are_registered_in_the_strict_protocol_table(self) -> None:
        root = Path(__file__).resolve().parents[1]
        probe = (root / "fixtures" / "psp_oracle" / "probe.c").read_text(
            encoding="utf-8"
        )
        self.assertEqual(probe.count('"vram-ge-depth-row%u-%02u"'), 2)
        self.assertEqual(probe.count('"vram-ge-color-row%u-%02u"'), 2)
        emitted_case_ids = (
            ("vram-mirror-info",)
            + tuple(
                f"vram-cpu-pattern-{chunk:03d}"
                for chunk in range(parse_golden.VRAM_MIRROR_CPU_SAMPLE_CHUNK_COUNT)
            )
            + tuple(
                f"vram-cpu-write-mirror-{alias:02d}-{sample:02d}"
                for alias in range(len(self.WRITE_BASES))
                for sample in range(len(parse_golden.VRAM_MIRROR_WRITE_OFFSETS))
            )
            + tuple(case[0] for case in parse_golden.VRAM_MIRROR_GE_SYNC_CASES)
            + tuple(
                f"vram-ge-depth-row{row}-{chunk:02d}"
                for row in range(len(parse_golden.VRAM_MIRROR_DEPTH_ROWS))
                for chunk in range(parse_golden.VRAM_MIRROR_DEPTH_ROW_CHUNK_COUNT)
            )
            + tuple(
                f"vram-ge-color-row{row}-{chunk:02d}"
                for row in range(len(parse_golden.VRAM_MIRROR_COLOR_ROWS))
                for chunk in range(parse_golden.VRAM_MIRROR_COLOR_ROW_CHUNK_COUNT)
            )
            + ("vram-restore", "vram-mirror-done")
        )
        self.assertEqual(emitted_case_ids, parse_golden.VRAM_MIRROR_SPEC.ordered_cases)
        parsed = _parse_campaign_records(self._stream(), "vram-mirror")
        self.assertEqual(
            tuple(record.case_id for record in parsed.results), emitted_case_ids
        )

    def test_manifest_and_docs_record_measured_hardware_result(self) -> None:
        root = Path(__file__).resolve().parents[1]
        manifest = json.loads(
            (root / "tools" / "psp_oracle" / "manifest.json").read_text(encoding="utf-8")
        )
        row = next(test for test in manifest["tests"]
                   if test["id"] == self.TEST_ID)
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(
            row["evidence_ref"],
            "docs/HARDWARE_ORACLE.md#psp-vram-mirror-001-measured-run-2026-10-16",
        )
        self.assertEqual(row["case_ids"], list(parse_golden.VRAM_MIRROR_SPEC.ordered_cases))
        self.assertEqual(set(row["evidence_cases"]), set(row["apis"]) | set(row["case_ids"]))
        self.assertIn("acceptance-eligible", row["evidence_note"])
        self.assertIn("acceptance-eligible", row["measurement_note"])
        readme = (root / "fixtures" / "psp_oracle" / "README.md").read_text(
            encoding="utf-8"
        )
        doc = (root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        self.assertIn("| `vram-mirror` | `PSP-VRAM-MIRROR-001` |", readme)
        self.assertIn("**MEASURED** on 2026-10-16", readme)
        self.assertIn(
            "| `vram-mirror` | `PSP-VRAM-MIRROR-001` | `MEASURED` (2026-10-16",
            doc,
        )
        self.assertIn("PSP-VRAM-MIRROR-001 measured run 2026-10-16", doc)
        self.assertIn("had zero mismatches across 4,134 sampled words", doc)
        self.assertIn("Address bits 0 and 1 are not independently", doc)
        self.assertIn("(y * 1024 + 2x) ^ 0x2040", doc)
        self.assertIn("Sub-word address bits were not independently measured", readme)

    def test_probe_snapshots_and_verifies_full_edram_restore(self) -> None:
        probe = (Path(__file__).resolve().parents[1] / "fixtures" / "psp_oracle"
                 / "probe.c").read_text(encoding="utf-8")
        self.assertIn("malloc(VRAM_MIRROR_SIZE)", probe)
        self.assertIn("memcpy((void *)(uintptr_t)0x44000000u, backup, VRAM_MIRROR_SIZE)", probe)
        self.assertIn("restore_mismatches += restored[byte] != backup[byte]", probe)
        self.assertIn("VRAM_MIRROR_PATTERN_PREFIX | offset", probe)
        self.assertIn("sceGuClearDepth(VRAM_MIRROR_CLEAR_DEPTH)", probe)
        self.assertIn("sceGuClear(GU_DEPTH_BUFFER_BIT)", probe)
        self.assertIn("sceGuEnable(GU_DEPTH_TEST)", probe)
        self.assertIn("sceGuDepthFunc(GU_ALWAYS)", probe)
        self.assertIn("sceGuDepthMask(GU_FALSE)", probe)
        self.assertIn("GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D", probe)
        self.assertIn("sceGuDrawArray(GU_SPRITES, vertex_type, 2", probe)
        self.assertIn("0x1111u, 32u, 16u, 0x1111u", probe)
        self.assertIn("0x2222u, 96u, 12u, 0x2222u", probe)
        self.assertIn("0x3333u, 208u, 101u, 0x3333u", probe)
        self.assertIn("sceGeDrawSync(1)", probe)
        self.assertIn("base_words[scan_offset / sizeof(uint32_t)] == marker", probe)


class VfpuFormsProbeTests(unittest.TestCase):
    """CASE=vfpu-forms (108, PSP-VFPU-FORMS-001) in probe_hle_issues.c: the #326 VFPU form
    words. Register-only: no system call beyond the shared writer and lifecycle, bounded
    loops, one step per group, and a fixed-shape stream that matches the registry."""

    @classmethod
    def setUpClass(cls) -> None:
        root = Path(__file__).resolve().parents[1]
        fixture = root / "fixtures" / "psp_oracle"
        cls.probe = (fixture / "probe_hle_issues.c").read_text(encoding="utf-8")
        cls.makefile = (fixture / "Makefile").read_text(encoding="utf-8")
        manifest = json.loads((root / "tools" / "psp_oracle" / "manifest.json")
                              .read_text(encoding="utf-8"))
        cls.rows = []
        stack = [manifest]
        for _ in range(100000):
            if not stack:
                break
            node = stack.pop()
            if isinstance(node, dict):
                if node.get("id") == "PSP-VFPU-FORMS-001":
                    cls.rows.append(node)
                stack.extend(node.values())
            elif isinstance(node, list):
                stack.extend(node)
        start = cls.probe.rindex("#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FORMS\n")
        end = cls.probe.index("#endif /* HLE_CASE_VFPU_FORMS */", start)
        cls.section = _c_code(cls.probe[start:end])

    def test_route_registration_and_log(self) -> None:
        self.assertIn("else ifeq ($(CASE),vfpu-forms)\nCASE_ID = 108\n", self.makefile)
        self.assertIn("else ifeq ($(CASE),vfpu-forms)\nTARGET = $(BUILD_DIR)/vfpu_forms\n"
                      "OBJS = $(BUILD_DIR)/probe_hle_issues.o $(BUILD_DIR)/threadman_user_imports.o\n",
                      self.makefile)
        self.assertIn("#define HLE_CASE_VFPU_FORMS 108\n", self.probe)
        self.assertIn('#define HLE_CAMPAIGN_ID "vfpu-forms"\n#define HLE_TEST_ID "PSP-VFPU-FORMS-001"\n'
                      '#define HLE_LOG "host0:/vfpu_forms_log.txt"', self.probe)
        self.assertEqual(_campaign_host0_log_path(Path("/scratch"), "vfpu-forms").name,
                         "vfpu_forms_log.txt")
        self.assertEqual(_campaign_completeness_contract("vfpu-forms"), "strict-golden-sequence")
        self.assertNotIn("vfpu-forms", CAMPAIGN_QUEUE_CASES)
        self.assertIn("vfpu-forms", parse_golden.HLE_OBSERVATION_CASES)

    def test_section_is_register_only_and_bounded(self) -> None:
        self.assertEqual(set(re.findall(r"\b(sce\w+)\s*\(", self.section)), set())
        self.assertNotIn("while (", self.section)
        for banned in ("for (;;)", "do {", "goto ", "malloc(", "kuKernel", "sctrl"):
            self.assertNotIn(banned, self.section)
        headers = re.findall(r"\bfor \(([^)]*)\)", self.section)
        self.assertGreaterEqual(len(headers), 15)
        for header in headers:
            with self.subTest(loop=header):
                self.assertRegex(header, r"^unsigned (\w+) = 0; \1 < (?:\d+u|TN_IMAGE_WORDS); \1\+\+$")

    def test_each_group_writes_its_step_before_its_cells(self) -> None:
        functions = _c_function_bodies(self.section)
        for name in ("vf_h2f", "vf_uc2i", "vf_socp", "vf_wbn", "vf_rndf"):
            with self.subTest(function=name):
                body = functions[name]
                cell = "vf_cell(" if name != "vf_rndf" else "vf_rndf1_step("
                self.assertLess(body.index("hle_step(name);"), body.index(cell))

    def test_stream_shape_matches_the_registry(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES["vfpu-forms"]
        self.assertEqual(spec.test_id, "PSP-VFPU-FORMS-001")
        self.assertEqual(spec.terminal_case, "vfpu-forms-done")
        self.assertEqual(len(parse_golden.VFPU_FORMS_CASES), 950)
        self.assertEqual(counts["h2f-p0-s-0-none"], 13)
        self.assertEqual(counts["rndf2-p1-16"], 17)
        self.assertIn('hle_record(name, "PASS", word, w, 13);', self.section)
        self.assertIn('hle_record(name, "PASS", out[0], w, 17);', self.section)
        grid = parse_golden.VFPU_FORMS_GRID
        for fragment in (f"s_vf_halves[{grid['h2f-s']}]", f"i < {grid['h2f-p']}u",
                         f"s_vf_bytes[{grid['uc2i']}]", f"s_vf_socp[{grid['socp-s']}]",
                         f"i < {grid['socp-p']}u", f"s_vf_wbn[{grid['wbn']}]",
                         f"k < {grid['rndf']}u",
                         f"s_vf_wbn_forms[{len(parse_golden.VFPU_FORMS_WBN_FORMS)}]",
                         f"pass < {parse_golden.VFPU_FORMS_PASSES}u"):
            self.assertIn(fragment, self.section)
        for form in parse_golden.VFPU_FORMS_WBN_FORMS:
            self.assertIn(f'{{"{form}", vf_wbn_', self.section)

    def test_manifest_row_is_a_single_launch_case(self) -> None:
        self.assertEqual(len(self.rows), 1)
        row = self.rows[0]
        self.assertEqual(row["prx"], "vfpu_forms.prx")
        self.assertEqual(row["source"], "fixtures/psp_oracle/probe_hle_issues.c")
        self.assertEqual(row["diagnostic_case_ids"], ["vfpu-forms"])
        self.assertEqual(row["issues"], [326])


class VfpuFunnelProbeTests(unittest.TestCase):
    """CASE=vfpu-funnel (118, PSP-VFPU-FUNNEL-001) in probe_hle_issues.c: the #326 vfad and
    vavg words. Register-only: no system call beyond the shared writer and lifecycle,
    bounded loops, one step per group, and a fixed-shape stream that matches the registry."""

    @classmethod
    def setUpClass(cls) -> None:
        root = Path(__file__).resolve().parents[1]
        fixture = root / "fixtures" / "psp_oracle"
        cls.probe = (fixture / "probe_hle_issues.c").read_text(encoding="utf-8")
        cls.makefile = (fixture / "Makefile").read_text(encoding="utf-8")
        manifest = json.loads((root / "tools" / "psp_oracle" / "manifest.json")
                              .read_text(encoding="utf-8"))
        cls.rows = []
        stack = [manifest]
        for _ in range(100000):
            if not stack:
                break
            node = stack.pop()
            if isinstance(node, dict):
                if node.get("id") == "PSP-VFPU-FUNNEL-001":
                    cls.rows.append(node)
                stack.extend(node.values())
            elif isinstance(node, list):
                stack.extend(node)
        start = cls.probe.rindex("#if PSP_ORACLE_CASE == HLE_CASE_VFPU_FUNNEL\n")
        end = cls.probe.index("#endif /* HLE_CASE_VFPU_FUNNEL */", start)
        cls.section = _c_code(cls.probe[start:end])

    def test_route_registration_and_log(self) -> None:
        self.assertIn("else ifeq ($(CASE),vfpu-funnel)\nCASE_ID = 118\n", self.makefile)
        self.assertIn("else ifeq ($(CASE),vfpu-funnel)\nTARGET = $(BUILD_DIR)/vfpu_funnel\n"
                      "OBJS = $(BUILD_DIR)/probe_hle_issues.o $(BUILD_DIR)/threadman_user_imports.o\n",
                      self.makefile)
        self.assertIn("#define HLE_CASE_VFPU_FUNNEL 118\n", self.probe)
        self.assertIn('#define HLE_CAMPAIGN_ID "vfpu-funnel"\n#define HLE_TEST_ID "PSP-VFPU-FUNNEL-001"\n'
                      '#define HLE_LOG "host0:/vfpu_funnel_log.txt"', self.probe)
        self.assertEqual(_campaign_host0_log_path(Path("/scratch"), "vfpu-funnel").name,
                         "vfpu_funnel_log.txt")
        self.assertEqual(_campaign_completeness_contract("vfpu-funnel"), "strict-golden-sequence")
        self.assertNotIn("vfpu-funnel", CAMPAIGN_QUEUE_CASES)
        self.assertIn("vfpu-funnel", parse_golden.HLE_OBSERVATION_CASES)

    def test_section_is_register_only_and_bounded(self) -> None:
        self.assertEqual(set(re.findall(r"\b(sce\w+)\s*\(", self.section)), set())
        self.assertNotIn("while (", self.section)
        for banned in ("for (;;)", "do {", "goto ", "malloc(", "kuKernel", "sctrl"):
            self.assertNotIn(banned, self.section)
        headers = re.findall(r"\bfor \(([^)]*)\)", self.section)
        self.assertGreaterEqual(len(headers), 6)
        for header in headers:
            with self.subTest(loop=header):
                self.assertRegex(header, r"^unsigned (\w+) = 0; \1 < (?:\d+u|TN_IMAGE_WORDS); \1\+\+$")

    def test_the_group_writes_its_step_before_its_cells(self) -> None:
        body = _c_function_bodies(self.section)["fn_group"]
        self.assertLess(body.index("hle_step(name);"), body.index("fn_cell("))

    def test_encodings_follow_the_vfpu_field_layout(self) -> None:
        # VFPU op 0x34, jump field (bits 21-25) 2, op field (bits 16-20) 6 = vfad and
        # 7 = vavg, vd S200 (0x08), vs C000 (0), size from bit 7 (.p), bit 15 (.t), both (.q).
        for op, form in ((6, "fad"), (7, "avg")):
            for size, bits in (("p", 0x0080), ("t", 0x8000), ("q", 0x8080)):
                word = (0x34 << 26) | (2 << 21) | (op << 16) | bits | 0x08
                with self.subTest(form=form, size=size):
                    self.assertIn(f'TN_CELL_SET(fn_{form}_{size}, "0x{word:08x}")', self.section)
                    self.assertIn(f"0x{word:08x}u", self.section)

    def test_stream_shape_matches_the_registry(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES["vfpu-funnel"]
        self.assertEqual(spec.test_id, "PSP-VFPU-FUNNEL-001")
        self.assertEqual(spec.terminal_case, "vfpu-funnel-done")
        sets = parse_golden.VFPU_FUNNEL_OPERAND_SETS
        self.assertEqual(len(parse_golden.VFPU_FUNNEL_CASES),
                         parse_golden.VFPU_FUNNEL_PASSES * len(parse_golden.VFPU_FUNNEL_FORMS)
                         * len(parse_golden.VFPU_FUNNEL_SIZES) * sets
                         * len(parse_golden.VFPU_FUNNEL_PREFIXES))
        self.assertEqual(len(parse_golden.VFPU_FUNNEL_CASES), 936)
        self.assertEqual(counts["fad-p0-p-0-none"], 13)
        self.assertEqual(counts["avg-p1-q-25-p2"], 13)
        self.assertEqual(len(parse_golden.VFPU_FUNNEL_CASES),
                         len(set(parse_golden.VFPU_FUNNEL_CASES)))
        self.assertIn('hle_record(name, "PASS", word, w, 13);', self.section)
        for fragment in (f"s_fn_ops[{sets}][4]", f"i < {sets}u", "s_fn_forms[2]", "s_fn_sizes[3]",
                         "s_fn_pfx_names[3]", f"pass < {parse_golden.VFPU_FUNNEL_PASSES}u"):
            self.assertIn(fragment, self.section)
        self.assertEqual(self.section.count("TN_CELL_SET("), 6)
        for form in parse_golden.VFPU_FUNNEL_FORMS:
            self.assertIn(f'"{form}"', self.section)
        for size in parse_golden.VFPU_FUNNEL_SIZES:
            self.assertIn(f'"{size}"', self.section)

    def test_the_operand_sets_cover_the_summation_rules(self) -> None:
        # Sets 0-1 separate sequential, pairwise and exact-then-round sums; sets 6-10 are the
        # x / 3 against x * (1 / 3) cases; the specials follow.
        ops = re.search(r"s_fn_ops\[26\]\[4\] = \{(.*?)\n\};", self.section, re.DOTALL)
        self.assertIsNotNone(ops)
        sets = re.findall(r"\{(0x[0-9a-f]{8}u(?:, 0x[0-9a-f]{8}u){3})\}", ops.group(1))
        self.assertEqual(len(sets), parse_golden.VFPU_FUNNEL_OPERAND_SETS)
        words = [[int(w[:-1], 16) for w in item.split(", ")] for item in sets]
        self.assertEqual(words[0], [0x4b800000, 0x3f800000, 0x3f800000, 0x3f800000])
        self.assertEqual(words[1], [0x3f800000, 0x3f800000, 0x3f800000, 0x4b800000])
        self.assertEqual(words[10], [0x3f800000, 0x3f800000, 0x3f800000, 0])
        self.assertEqual(len({tuple(item) for item in words}), len(words))

    def test_manifest_row_is_a_single_launch_measured_case(self) -> None:
        self.assertEqual(len(self.rows), 1)
        row = self.rows[0]
        self.assertEqual(row["prx"], "vfpu_funnel.prx")
        self.assertEqual(row["source"], "fixtures/psp_oracle/probe_hle_issues.c")
        self.assertEqual(row["diagnostic_case_ids"], ["vfpu-funnel"])
        self.assertEqual(row["issues"], [326])
        # The 2026-10-29 acceptance-eligible PSP-3000 run of 8d0f75ad cites every cell.
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(row["evidence_cases"], row["case_ids"])


class GeSampleProbeTests(unittest.TestCase):
    """CASE=ge-sample (109, PSP-GE-SAMPLE-001) in probe_ge_sample.c: the #282 GE texture
    sample-point cells. Raw display lists, bounded GE waits, one step per draw, the shared
    writer and lifecycle, and a fixed-shape stream that matches the registry."""

    @classmethod
    def setUpClass(cls) -> None:
        root = Path(__file__).resolve().parents[1]
        fixture = root / "fixtures" / "psp_oracle"
        cls.probe = (fixture / "probe_ge_sample.c").read_text(encoding="utf-8")
        cls.issues = (fixture / "probe_hle_issues.c").read_text(encoding="utf-8")
        cls.makefile = (fixture / "Makefile").read_text(encoding="utf-8")
        start = cls.probe.index("#if PSP_ORACLE_CASE == GE_CASE_SAMPLE\n\n")
        end = cls.probe.index("#endif /* GE_CASE_SAMPLE */", start)
        cls.section = _c_code(cls.probe[start:end])

    def test_route_registration_and_log(self) -> None:
        self.assertIn("else ifeq ($(CASE),ge-sample)\nCASE_ID = 109\n", self.makefile)
        self.assertIn("else ifeq ($(CASE),ge-sample)\nTARGET = $(BUILD_DIR)/ge_sample\n"
                      "OBJS = $(BUILD_DIR)/probe_ge_sample.o $(BUILD_DIR)/threadman_user_imports.o\n",
                      self.makefile)
        self.assertIn("ifneq ($(filter $(BUILD_DIR)/probe_ge_sample.o,$(OBJS)),)\n"
                      "PRX_EXPORTS = $(BUILD_DIR)/probe_exports.exp\n", self.makefile)
        self.assertIn("#define GE_CASE_SAMPLE 109\n", self.probe)
        self.assertIn('#define HLE_CAMPAIGN_ID "ge-sample"\n#define HLE_TEST_ID "PSP-GE-SAMPLE-001"\n'
                      '#define HLE_LOG "host0:/ge_sample_log.txt"', self.probe)
        self.assertEqual(_campaign_host0_log_path(Path("/scratch"), "ge-sample").name,
                         "ge_sample_log.txt")
        self.assertEqual(_campaign_completeness_contract("ge-sample"), "strict-golden-sequence")
        self.assertNotIn("ge-sample", CAMPAIGN_QUEUE_CASES)

    def test_writer_round_trip_and_lifecycle_are_the_hle_issues_copies(self) -> None:
        shared = self.issues[self.issues.index("static int s_log_failed;\n"):
                             self.issues.index("/* ---", self.issues.index("int module_stop("))]
        self.assertIn(shared, self.probe)

    def test_section_calls_only_the_ge_and_cache_imports_and_is_bounded(self) -> None:
        self.assertEqual(set(re.findall(r"\b(sce\w+)\s*\(", self.section)), {
            "sceGeEdramGetAddr", "sceGeListEnQueue", "sceGeListSync", "sceGeDrawSync",
            "sceGeBreak", "sceKernelDcacheWritebackAll", "sceKernelDelayThread",
            "sceKernelGetSystemTimeLow"})
        self.assertNotIn("while (", self.section)
        for banned in ("for (;;)", "do {", "goto ", "malloc(", "sceGu", "sceDisplay"):
            self.assertNotIn(banned, self.section)
        self.assertIn("sceGeListEnQueue(s_gs_list, NULL, -1, NULL)", self.section)
        self.assertEqual(self.section.count("i < GS_WAIT_POLLS; i++"), 2)
        self.assertIn("#define GS_WAIT_POLLS 5000u", self.probe)
        self.assertIn("#define GS_WAIT_POLL_US 100u", self.probe)
        # The framebuffer block the probe clears and draws into: 64 x 32 words at VRAM 0x100000.
        self.assertIn("#define GS_FB_OFFSET 0x00100000u", self.probe)
        self.assertIn("for (unsigned i = 0; i < GS_FB_STRIDE * GS_FB_ROWS; i++) {", self.section)
        self.assertIn("0x9C000000u | (GS_FB_OFFSET & 0x00ffffffu), 0x9D000000u | GS_FB_STRIDE,",
                      self.section)

    def test_each_draw_writes_its_step_before_its_list(self) -> None:
        body = _c_function_bodies(self.section)["gs_draw"]
        self.assertLess(body.index("hle_step(name);"), body.index("sceGeListEnQueue("))
        self.assertLess(body.index("sceGeListEnQueue("), body.index("hle_record("))

    def test_stream_shape_matches_the_registry(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES["ge-sample"]
        self.assertEqual(spec.test_id, "PSP-GE-SAMPLE-001")
        self.assertEqual(spec.terminal_case, "ge-sample-done")
        self.assertEqual(len(parse_golden.GE_SAMPLE_CASES), 48 * 17)
        self.assertEqual(counts["tri-near-1to1-u-draw"], 7)
        self.assertEqual(counts["spr-lin-s16min-v-r18"], 16)
        self.assertEqual(parse_golden.GE_SAMPLE_CASES[:2],
                         ("tri-near-1to1-u-draw", "tri-near-1to1-u-r03"))
        maps = re.findall(r'\{"(\w+)", [01]u, ', self.section)
        self.assertEqual(tuple(maps), parse_golden.GE_SAMPLE_MAPS)
        self.assertIn('s_gs_prims[2] = {"tri", "spr"}', self.section)
        self.assertIn('s_gs_filters[2] = {"near", "lin"}', self.section)
        self.assertIn('s_gs_texs[2] = {"u", "v"}', self.section)
        self.assertIn("#define GS_WIN_Y0 3u", self.probe)
        self.assertIn("#define GS_WIN_SIDE 16u", self.probe)
        self.assertEqual(parse_golden.GE_SAMPLE_ROWS, tuple(range(3, 19)))


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


REGISTRY_READ_CALLS = frozenset(user_mode_imports.REGISTRY_READ_NIDS.values())
_HOST_CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _hex(name: str | bytes) -> str:
    """Hex for the harness scenario; "-" stands for zero bytes."""
    raw = name.encode("ascii") if isinstance(name, str) else name
    return raw.hex() or "-"


def _node(kind: int, parent: str, name: str | bytes, value: bytes | None = None) -> str:
    line = f"node {kind} {parent} {_hex(name)}"
    return line + (f" {_hex(value)}" if value is not None else "")


def _font_tree(info_count: int = 2) -> list[str]:
    """A synthetic registry shaped like the one libfont reads, plus off-route neighbours."""

    nodes = [
        _node(1, "/", "REGISTRY"),
        _node(1, "/", "SYSPROFILE"),
        _node(1, "/", "DATA"),
        _node(1, "/", "CONFIG"),
        _node(2, "/REGISTRY", "category_version", b"\x01\x00\x00\x00"),
        _node(1, "/SYSPROFILE", "SOUND"),
        _node(2, "/SYSPROFILE/SOUND", "dynamic_normalizer", b"\x00\x00\x00\x00"),
        _node(1, "/SYSPROFILE", "RESOLUTION"),
        _node(2, "/SYSPROFILE/RESOLUTION", "horizontal", b"\x11\x00\x00\x00"),
        _node(2, "/SYSPROFILE/RESOLUTION", "vertical", b"\x22\x00\x00\x00"),
        _node(1, "/DATA", "COUNT"),
        _node(2, "/DATA/COUNT", "boot_count", b"\x05\x00\x00\x00"),
        _node(2, "/DATA", "data_version", b"\x01\x00\x00\x00"),
        _node(1, "/DATA", "FONT"),
        _node(3, "/DATA/FONT", "path_name", b"synthetic:/f/\x00"),
        _node(2, "/DATA/FONT", "num_fonts", info_count.to_bytes(4, "little")),
        _node(1, "/DATA/FONT", "PROPERTY"),
        _node(2, "/CONFIG", "language", b"\x01\x00\x00\x00"),
        _node(1, "/CONFIG", "SYSTEM"),
        _node(1, "/CONFIG", "__NAKAGAWA_ORACLE_UNKNOWN_"),
    ]
    for index in range(info_count):
        info = f"/DATA/FONT/PROPERTY/INFO{index}"
        nodes += [
            _node(1, "/DATA/FONT/PROPERTY", f"INFO{index}"),
            _node(2, info, "h_size", (0x288 + index).to_bytes(4, "little")),
            _node(3, info, "font_name", f"Synthetic {index}".encode() + b"\x00"),
            _node(3, info, "file_name", f"s{index}.bin".encode() + b"\x00"),
        ]
    return nodes


@unittest.skipUnless(_HOST_CC, "no host C compiler on PATH")
class RegistryFontWalkHostTests(unittest.TestCase):
    """registry_walk.h, the probe's font-registry walk, run against a fake registry.

    The fake follows the measured PSP-3000 behaviour: opening a missing category creates it
    (logged as CREATED), a missing key is 0x8008271D and an unknown handle 0x8008272E. Every
    test replays the call log and checks the walk's safety contract, not only its records.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[1]
        cls.fixture = cls.root / "fixtures" / "psp_oracle"
        cls.tmp = tempfile.mkdtemp(prefix="registry_walk_host_")
        cls.exe = str(Path(cls.tmp) / "registry_walk_host.exe")
        result = subprocess.run(
            [_HOST_CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
             f"-I{cls.fixture}", "-o", cls.exe, str(cls.fixture / "registry_walk_host.c")],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError("registry_walk_host.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, nodes: list[str], *, root_lists: bool = True,
             options: tuple[str, ...] = ()) -> SimpleNamespace:
        scenario = "\n".join([f"option root-lists {int(root_lists)}", *options, *nodes]) + "\n"
        result = subprocess.run([self.exe], input=scenario, capture_output=True, text=True,
                                check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        emitted = "".join(line[5:] + "\n" for line in lines if line.startswith("EMIT "))
        summary = dict(item.split("=", 1) for item in lines[-1].split()[1:])
        self.assertTrue(lines[-1].startswith("SUMMARY "), lines[-1])
        run = SimpleNamespace(
            lines=lines,
            stream=META.format(source="psp", model="PSP-3000", firmware="6.6.1",
                               binary=MEASURED_SHA, commit=MEASURED_COMMIT) + emitted,
            summary={key: int(value) for key, value in summary.items()},
            opens=[line.split()[2][5:] for line in lines
                   if line.startswith("CALL sceRegOpenCategory ")],
        )
        self._assert_safe(run)
        return run

    def _assert_safe(self, run: SimpleNamespace) -> None:
        """Replay the call log: the walk's safety contract, independent of its records."""

        calls = {line.split()[1] for line in run.lines if line.startswith("CALL ")}
        self.assertLessEqual(calls, REGISTRY_READ_CALLS)
        self.assertFalse([line for line in run.lines if line.startswith("CREATED ")])
        self.assertFalse([line for line in run.lines if line.startswith("OPEN-NOT-A-CATEGORY")])
        self.assertEqual(run.summary["created"], 0)
        self.assertEqual(run.summary["nodes_before"], run.summary["nodes_after"])
        self.assertEqual(run.summary["open_categories"], 0)
        self.assertEqual(run.summary["open_registries"], 0)
        listed: set[tuple[str, str]] = set()
        directories: set[str] = set()
        open_paths: list[str] = ["/"]
        anchor_checked = False
        for line in run.lines:
            fields = dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)
            if line.startswith("LISTED "):
                listed.add((fields["category"], bytes.fromhex(fields["name_hex"]).decode("latin-1")))
            elif line.startswith("INFO "):
                if fields["type"] == "1":
                    directories.add(fields["path"])
                    anchor_checked = anchor_checked or fields["path"] == "/CONFIG"
            elif line.startswith("CALL sceRegGetKeyInfo "):
                # A lookup only ever names something its handle just listed.
                self.assertIn((fields["category"], fields["name"]), listed, line)
            elif line.startswith("CALL sceRegOpenCategory "):
                path = fields["path"]
                parent, _, name = path.rpartition("/")
                parent = parent or "/"
                self.assertEqual(fields["mode"], "1", line)
                self.assertIn(parent, open_paths, line)
                self.assertIn((parent, name), listed, line)
                self.assertIn(path, directories, line)
                if parent == "/":
                    # The listing is the root only if it showed /CONFIG as a directory first.
                    self.assertTrue(anchor_checked, line)
                self.assertRegex(name, r"^[A-Za-z0-9_.-]{1,25}$", line)
                parts = path[1:].split("/")
                self.assertTrue(
                    any(tuple(parts[:2]) == route[:len(parts[:2])]
                        for route in (("DATA", "FONT"), ("SYSPROFILE", "RESOLUTION"))),
                    line,
                )
                self.assertLessEqual(len(parts), 6, line)
                open_paths.append(path)

    def _records(self, run: SimpleNamespace):
        return parse_output(run.stream).results

    def test_full_tree_opens_only_the_font_routes_and_reads_only_their_values(self) -> None:
        run = self._run(_font_tree())
        self.assertEqual(run.opens, [
            "/SYSPROFILE", "/SYSPROFILE/RESOLUTION", "/DATA", "/DATA/FONT",
            "/DATA/FONT/PROPERTY", "/DATA/FONT/PROPERTY/INFO0", "/DATA/FONT/PROPERTY/INFO1",
        ])
        report = parse_registry_font_output(run.stream)
        self.assertTrue(report.complete and report.registry_opened and report.root_enumerated)
        self.assertTrue(report.root_verified)
        self.assertEqual(report.opened, tuple(run.opens))
        values = {key.path: key.value for key in report.keys if key.value is not None}
        self.assertEqual(values["/DATA/FONT/path_name"], b"synthetic:/f/\x00")
        self.assertEqual(values["/DATA/FONT/num_fonts"], b"\x02\x00\x00\x00")
        self.assertEqual(values["/SYSPROFILE/RESOLUTION/vertical"], b"\x22\x00\x00\x00")
        self.assertEqual(values["/DATA/FONT/PROPERTY/INFO1/file_name"], b"s1.bin\x00")
        self.assertTrue(all(path.startswith(("/DATA/FONT/", "/SYSPROFILE/RESOLUTION/"))
                            for path in values))
        self.assertEqual(len(values), 2 + 2 + 3 * 2)
        # Enumeration order is recorded: every key carries its index in its parent's listing.
        info0 = [key for key in report.keys if key.path.startswith("/DATA/FONT/PROPERTY/INFO0/")]
        self.assertEqual([(key.path.rsplit("/", 1)[1], key.index) for key in info0],
                         [("h_size", 0), ("font_name", 1), ("file_name", 2)])
        dispositions = {key.path: key.disposition for key in report.keys}
        self.assertEqual(dispositions["/CONFIG"], 3)
        self.assertEqual(dispositions["/REGISTRY"], 3)
        self.assertEqual(dispositions["/DATA/COUNT"], 3)
        self.assertEqual(dispositions["/SYSPROFILE/SOUND"], 3)
        self.assertEqual(dispositions["/DATA/data_version"], 0)
        steps = [line for line in run.lines if line.startswith("STEP ")]
        self.assertEqual(steps[:4], ["STEP open-registry", "STEP root-count", "STEP root-keys",
                                     "STEP root-verify"])
        self.assertIn("STEP open:/DATA/FONT/PROPERTY/INFO1", steps)
        self.assertEqual(steps[-1], "STEP close-registry")

    def test_a_registry_handle_that_does_not_list_the_root_opens_nothing(self) -> None:
        run = self._run(_font_tree(), root_lists=False)
        self.assertEqual(run.opens, [])
        self.assertFalse([line for line in run.lines if line.startswith("CALL sceRegGetKeyInfo")])
        self.assertFalse([line for line in run.lines if line.startswith("CALL sceRegGetKeys ")])
        report = parse_registry_font_output(run.stream)
        self.assertFalse(report.root_enumerated)
        self.assertFalse(report.root_verified)
        self.assertEqual(report.categories, ("/",))
        root = self._records(run)[1]
        self.assertEqual(root.status, "FAIL")
        self.assertEqual(dict(root.values)["out0"], "0x8008272e")
        done = dict(self._records(run)[-1].values)
        self.assertEqual(done["out5"], "0x00000000")

    def test_a_listing_without_config_as_a_directory_is_not_taken_as_the_root(self) -> None:
        tree = [node for node in _font_tree()
                if not node.startswith(("node 1 / " + _hex("CONFIG"),
                                        "node 2 /CONFIG ", "node 1 /CONFIG "))]
        for extra in ([], [_node(2, "/", "CONFIG", bytes(4))]):
            run = self._run(tree + extra)
            self.assertEqual(run.opens, [])
            report = parse_registry_font_output(run.stream)
            self.assertTrue(report.root_enumerated)
            self.assertFalse(report.root_verified)
            dispositions = {key.path: key.disposition for key in report.keys}
            self.assertEqual(dispositions["/DATA"], 10)
            self.assertEqual(dispositions["/SYSPROFILE"], 10)
            done = dict(self._records(run)[-1].values)
            self.assertEqual(done["out6"], "0x00000000")
            self.assertEqual(int(done["out4"], 16), 2)

    def test_absent_or_non_directory_routes_are_never_opened(self) -> None:
        nodes = [
            _node(1, "/", "CONFIG"),
            _node(2, "/", "DATA", b"\x00\x00\x00\x00"),
            _node(1, "/", "SYSPROFILEX"),
            _node(1, "/SYSPROFILEX", "RESOLUTION"),
        ]
        run = self._run(nodes)
        self.assertEqual(run.opens, [])
        report = parse_registry_font_output(run.stream)
        self.assertEqual({key.path: key.disposition for key in report.keys},
                         {"/CONFIG": 3, "/DATA": 0, "/SYSPROFILEX": 3})

    def test_names_that_may_not_round_trip_are_never_opened_or_queried(self) -> None:
        nodes = _font_tree(info_count=0) + [
            _node(1, "/DATA/FONT/PROPERTY", "I" * 25),
            _node(1, "/DATA/FONT/PROPERTY", "J" * 26),
            _node(1, "/DATA/FONT/PROPERTY", b"BAD NAME"),
            _node(1, "/DATA/FONT/PROPERTY", b"a/b"),
            _node(1, "/DATA/FONT/PROPERTY", b"K" * 27),
            _node(1, "/DATA/FONT/PROPERTY", b"\x01ctl"),
            _node(1, "/DATA/FONT/PROPERTY", b""),
        ]
        run = self._run(nodes)
        self.assertIn("/DATA/FONT/PROPERTY/" + "I" * 25, run.opens)
        self.assertEqual([path for path in run.opens if "PROPERTY/" in path],
                         ["/DATA/FONT/PROPERTY/" + "I" * 25])
        queried = [line for line in run.lines if line.startswith("CALL sceRegGetKeyInfo ")
                   and "category=/DATA/FONT/PROPERTY " in line]
        self.assertEqual([line.split("name=", 1)[1] for line in queried], ["I" * 25, "J" * 26])
        report = parse_registry_font_output(run.stream)
        below = [key for key in report.keys
                 if key.path.startswith("/DATA/FONT/PROPERTY") and key.path != "/DATA/FONT/PROPERTY"]
        self.assertEqual([key.disposition for key in below if key.disposition != 9], [2, 4])
        self.assertEqual(sum(1 for key in report.keys if key.disposition == 9), 5)
        self.assertEqual(int(dict(self._records(run)[-1].values)["out4"], 16), 1)

    def test_depth_and_open_limits_bound_the_walk(self) -> None:
        chain = ["/DATA/FONT"]
        nodes = _font_tree(info_count=0)
        for name in ("D3", "D4", "D5", "D6", "D7"):
            nodes.append(_node(1, chain[-1], name))
            chain.append(f"{chain[-1]}/{name}")
        run = self._run(nodes)
        self.assertIn("/DATA/FONT/D3/D4/D5/D6", run.opens)
        self.assertNotIn("/DATA/FONT/D3/D4/D5/D6/D7", run.opens)
        report = parse_registry_font_output(run.stream)
        self.assertEqual({key.path: key.disposition for key in report.keys}[
            "/DATA/FONT/D3/D4/D5/D6/D7"], 5)

        nodes = _font_tree(info_count=0)
        for index in range(60):
            nodes.append(_node(1, "/DATA/FONT/PROPERTY", f"INFO{index}"))
            nodes.append(_node(1, f"/DATA/FONT/PROPERTY/INFO{index}", "SUB"))
        run = self._run(nodes)
        self.assertEqual(len(run.opens), 96)
        report = parse_registry_font_output(run.stream)
        # Five route opens, then INFO<k> and its SUB alternate until the 96th open (INFO45):
        # SUB of INFO45 and INFO46..INFO59 are refused, and nothing below them is listed.
        limited = [key for key in report.keys if key.disposition == 6]
        self.assertEqual([key.path for key in limited],
                         ["/DATA/FONT/PROPERTY/INFO45/SUB"]
                         + [f"/DATA/FONT/PROPERTY/INFO{index}" for index in range(46, 60)])
        self.assertEqual(int(dict(self._records(run)[-1].values)["out4"], 16), len(limited))

    def test_an_oversized_listing_is_refused_before_any_name_is_read(self) -> None:
        nodes = _font_tree(info_count=0)
        for index in range(65):
            nodes.append(_node(1, "/DATA/FONT/PROPERTY", f"INFO{index}"))
        run = self._run(nodes)
        self.assertFalse([path for path in run.opens if path.startswith("/DATA/FONT/PROPERTY/")])
        self.assertFalse([line for line in run.lines if line.startswith("CALL sceRegGetKeys ")
                          and "category=/DATA/FONT/PROPERTY " in line])
        report = parse_registry_font_output(run.stream)
        property_record = next(record for record in self._records(run)
                               if dict(record.values).get("detail") == "/DATA/FONT/PROPERTY"
                               and record.case_id.startswith("font-category-"))
        self.assertEqual(property_record.status, "FAIL")
        self.assertIn("/DATA/FONT/PROPERTY", report.opened)

    def test_failed_calls_are_recorded_and_the_walk_continues(self) -> None:
        nodes = _font_tree(info_count=3) + [
            _node(4, "/DATA/FONT", "oversized", bytes(1025)),
            _node(4, "/DATA/FONT", "empty", b""),
        ]
        run = self._run(nodes, options=(
            "option open-fails /DATA/FONT/PROPERTY/INFO1",
            "option info-fails /DATA/FONT/num_fonts",
            "option value-fails /DATA/FONT/path_name",
        ))
        self.assertIn("/DATA/FONT/PROPERTY/INFO2", run.opens)
        report = parse_registry_font_output(run.stream)
        self.assertNotIn("/DATA/FONT/PROPERTY/INFO1", report.opened)
        self.assertIn("/DATA/FONT/PROPERTY/INFO1", report.categories)
        keys = {key.path: key for key in report.keys}
        self.assertEqual(keys["/DATA/FONT/num_fonts"].disposition, 8)
        self.assertEqual(keys["/DATA/FONT/path_name"].disposition, 1)
        self.assertIsNone(keys["/DATA/FONT/path_name"].value)
        self.assertEqual(keys["/DATA/FONT/oversized"].disposition, 7)
        self.assertEqual(keys["/DATA/FONT/empty"].disposition, 7)
        self.assertFalse([line for line in run.lines if line.startswith("CALL sceRegGetKeyValue")
                          and ("oversized" in line or "empty" in line)])

    def test_parser_rejects_evidence_of_an_unsafe_or_incomplete_walk(self) -> None:
        run = self._run(_font_tree())
        lines = run.stream.splitlines(keepends=True)
        self.assertTrue(parse_registry_font_output(run.stream).complete)

        def mutate(old: str, new: str) -> str:
            self.assertIn(old, run.stream)
            return run.stream.replace(old, new, 1)

        for bad in (
            # A category whose listing key named something else was opened by a literal path.
            mutate("out3=0x00000002 out4=0xffffffff detail=/DATA/FONT/PROPERTY\n",
                   "out3=0x00000002 out4=0xffffffff detail=/DATA/FONT/PROPERTZ\n"),
            mutate("detail=/DATA\n", "detail=/CONFIG\n"),
            mutate("out3=0x00000003 out4=0xffffffff detail=/CONFIG",
                   "out3=0x00000003 out4=0xffffffff detail=/CONFIG value_hex=00"),
            # Root children were opened, but the listing never showed /CONFIG as a directory.
            mutate("out3=0x00000003 out4=0xffffffff detail=/CONFIG\n",
                   "out3=0x00000003 out4=0xffffffff detail=/CONFIX\n"),
            mutate("out5=0x00000001 out6=0x00000001", "out5=0x00000001 out6=0x00000000"),
            mutate("out3=0x00000000 out4=0xffffffff detail=/DATA/data_version",
                   "out3=0x00000001 out4=0x00000000 detail=/DATA/data_version value_hex=01000000"),
            mutate("test_id=PSP-REGISTRY-002 case_id=font-key-0003",
                   "test_id=PSP-REGISTRY-001 case_id=font-key-0003"),
            "".join(line for line in lines if "case_id=font-key-0005 " not in line),
        ):
            with self.assertRaises(ProtocolError):
                parse_registry_font_output(bad)
        truncated = "".join(lines[:-1])
        with self.assertRaises(ProtocolError):
            parse_registry_font_output(truncated)
        self.assertFalse(parse_registry_font_output(truncated, require_complete=False).complete)

    def test_runner_completion_contract_accepts_the_walk(self) -> None:
        run = self._run(_font_tree())
        self.assertEqual(_campaign_completeness_contract("registry-font"),
                         "strict-golden-sequence")
        self.assertEqual(_campaign_host0_log_path(Path("host0"), "registry-font").name,
                         "registry_font_log.txt")
        complete = run.stream + "NAKAGAWA_PSP_COMPLETE schema=1 status=PASS\n"
        self.assertTrue(_campaign_stream_complete(complete, "registry-font"))
        cut = run.stream.rsplit("NAKAGAWA_PSP_TEST", 1)[0]
        self.assertFalse(_campaign_stream_complete(cut, "registry-font"))
        self.assertIn("registry-font", CAMPAIGN_QUEUE_CASES)
        self.assertEqual(CAMPAIGN_QUEUE_CASES.index("registry-font"),
                         CAMPAIGN_QUEUE_CASES.index("fpu-cc") + 1)


class RegistryFontSourceTests(unittest.TestCase):
    """Source-level rules for the registry-font case and its read-only import set."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.walk = (self.fixture / "registry_walk.h").read_text(encoding="utf-8")
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")
        self.makefile = (self.fixture / "Makefile").read_text(encoding="utf-8")

    def test_walk_calls_only_the_read_set_and_opens_no_literal_path(self) -> None:
        code = re.sub(r"/\*.*?\*/|//[^\n]*", "", self.walk, flags=re.DOTALL)
        self.assertLessEqual(set(re.findall(r"\b(sceReg\w+)\s*\(", code)), REGISTRY_READ_CALLS)
        self.assertEqual(re.findall(r'sceRegOpenCategory\s*\([^,]+,\s*"', code), [])
        self.assertEqual(
            re.findall(r"sceRegOpenCategory\s*\(([^;]*)\);", code),
            ["registry, child->path, REGWALK_OPEN_MODE, &handle"],
        )
        self.assertIn("#define REGWALK_NAME_MAX 26u", self.walk)
        self.assertIn("#define REGWALK_OPEN_MODE 1", self.walk)
        self.assertNotRegex(code, r"\bmalloc\s*\(|\bcalloc\s*\(")
        block = self.probe[self.probe.index(
            "#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_REGISTRY_FONT\n"):]
        block = block[:block.index("\n#endif")]
        self.assertNotIn("sceReg", block)
        self.assertIn('#include "registry_walk.h"', block)

    def test_registry_cases_pin_exactly_the_read_nids(self) -> None:
        self.assertIn("REGISTRY_CASES = registry-readonly registry-font", self.makefile)
        line = next(line for line in self.makefile.splitlines()
                    if line.startswith("USER_MODE_IMPORT_GATE_ARGS += --exact-library sceReg="))
        nids = {int(item, 16) for item in line.split("sceReg=", 1)[1].split(",")}
        self.assertEqual(nids, set(user_mode_imports.REGISTRY_READ_NIDS))
        fixup = next(line for line in self.makefile.splitlines() if line.startswith("FIXUP = "))
        self.assertIn('"$(USER_MODE_IMPORT_GATE)" $(USER_MODE_IMPORT_GATE_ARGS) "$$1"', fixup)
        self.assertIn("$(BUILD_DIR)/probe.o: probe.c registry_walk.h", self.makefile)
        self.assertRegex(self.makefile, r"(?m)^else ifeq \(\$\(CASE\),registry-font\)\nCASE_ID = 72$")
        self.assertIn('#define PROBE_HOST0_LOG "host0:/registry_font_log.txt"', self.probe)

    def _gate(self, libs: list[tuple[str, list[int]]], *extra: str,
              module_attributes: int = 0) -> tuple[int, str, str]:
        data = build_import_elf(libs, module_attributes=module_attributes)
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp) / "probe.elf"
            module.write_bytes(data)
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                status = user_mode_imports.main([*extra, str(module)])
        return status, out.getvalue(), err.getvalue()

    def test_gate_accepts_the_read_set_and_refuses_every_other_registry_nid(self) -> None:
        read = sorted(user_mode_imports.REGISTRY_READ_NIDS)
        exact = "sceReg=" + ",".join(f"0x{nid:08X}" for nid in read)
        status, out, _err = self._gate([("IoFileMgrForUser", [0x109F50BC]), ("sceReg", read)],
                                       "--exact-library", exact)
        self.assertEqual(status, user_mode_imports.EXIT_OK, out)
        refused = sorted(user_mode_imports.REGISTRY_REFUSED_NIDS) + [0x12345678]
        for nid in refused:
            for attributes in (0, user_mode_imports.PSP_MODULE_KERNEL):
                status, _out, err = self._gate([("sceReg", read[:2] + [nid])],
                                               module_attributes=attributes)
                self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT, hex(nid))
                self.assertIn(f"0x{nid:08X}", err)
                self.assertIn(user_mode_imports.registry_call_name(nid), err)
        self.assertEqual(user_mode_imports.registry_call_name(0x17768E14), "sceRegSetKeyValue")

    def test_exact_library_set_must_match(self) -> None:
        read = sorted(user_mode_imports.REGISTRY_READ_NIDS)
        exact = "sceReg=" + ",".join(f"0x{nid:08X}" for nid in read)
        status, _out, err = self._gate([("sceReg", read[:-1])], "--exact-library", exact)
        self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
        self.assertIn(f"missing 0x{read[-1]:08X}", err)
        narrow = "sceReg=" + ",".join(f"0x{nid:08X}" for nid in read[:-1])
        status, _out, err = self._gate([("sceReg", read)], "--exact-library", narrow)
        self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
        self.assertIn(f"unexpected 0x{read[-1]:08X}", err)
        for bad in (["--exact-library"], ["--exact-library", "sceReg"],
                    ["--exact-library", "sceReg=0x1"],
                    ["--exact-library", "sceReg=0x92E41280,0x92E41280"],
                    ["--exact-library", exact, "--exact-library", exact]):
            status, _out, _err = self._gate([("sceReg", read)], *bad)
            self.assertEqual(status, user_mode_imports.EXIT_UNREADABLE, bad)


def _gen_bytes(size: int, seed: int) -> bytes:
    """The io_cases_host.c generated file content: byte[i] = (i * 31 + seed) ^ (i >> 7)."""
    return bytes(((index * 31 + seed) ^ (index >> 7)) & 0xFF for index in range(size))


_REGULAR = 0x21FF
_DIRECTORY = 0x11FF


def _entry(name: str | bytes, size: int = 0, seed: int = 0, *, mode: int = _REGULAR,
           listed: int | None = None) -> str:
    line = f"entry 0x{mode:x} 0x20 {_hex(name)} {size} {seed}"
    return line + (f" {listed}" if listed is not None else "")


@unittest.skipUnless(_HOST_CC, "no host C compiler on PATH")
class IoCasesHostTests(unittest.TestCase):
    """font_files.h (font-files) and umd_io.h (umd-io) run against a fake file system.

    Every test replays the call log: flash0 is only listed and opened read-only, writes reach
    only host0:/fonts/<listed safe name>, the device case opens only with its read flags, and
    every descriptor is closed.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[1]
        cls.fixture = cls.root / "fixtures" / "psp_oracle"
        cls.tmp = tempfile.mkdtemp(prefix="io_cases_host_")
        cls.exe = str(Path(cls.tmp) / "io_cases_host.exe")
        result = subprocess.run(
            [_HOST_CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
             f"-I{cls.fixture}", "-o", cls.exe, str(cls.fixture / "io_cases_host.c")],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError("io_cases_host.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, mode: str, lines: list[str]) -> SimpleNamespace:
        result = subprocess.run([self.exe, mode], input="\n".join(lines) + "\n",
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        out = result.stdout.splitlines()
        self.assertTrue(out[-1].startswith("SUMMARY "), out[-1])
        self.assertEqual(out[-1], "SUMMARY open_fds=0")
        calls = []
        for line in out:
            if line.startswith("CALL "):
                parts = line.split()
                calls.append((parts[1], dict(item.split("=", 1) for item in parts[2:])))
        host0 = {}
        for line in out:
            if line.startswith("HOST0 "):
                fields = dict(item.split("=", 1) for item in line.split()[1:])
                host0[fields["name"]] = (int(fields["bytes"]), fields["equal"] == "1")
        emitted = "".join(line[5:] + "\n" for line in out if line.startswith("EMIT "))
        stream = META.format(source="psp", model="PSP-3000", firmware="6.6.1",
                             binary=MEASURED_SHA, commit=MEASURED_COMMIT) + emitted
        return SimpleNamespace(lines=out, calls=calls, host0=host0, stream=stream)

    def _assert_font_calls_safe(self, run: SimpleNamespace) -> None:
        listed: set[str] = set()
        for name, fields in run.calls:
            path = fields.get("path", "")
            if name == "sceIoDopen":
                self.assertIn(path, ("flash0:/font", "flash0:/font/"))
            elif name == "sceIoMkdir":
                self.assertEqual(path, "host0:/fonts")
            elif name == "sceIoOpen":
                flags = int(fields["flags"], 16)
                if path.startswith("flash0:"):
                    self.assertEqual(flags, 0x1, path)
                else:
                    self.assertTrue(path.startswith("host0:/fonts/"), path)
                    self.assertIn(flags, (0x1, 0x602), path)
                    leaf = path[len("host0:/fonts/"):]
                    self.assertRegex(leaf, r"^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,62}$")
                    if flags == 0x602:
                        listed.add(leaf)
            elif name == "sceIoWrite":
                self.assertTrue(fields["target"].startswith("host0:/fonts/"), fields)
            else:
                self.assertIn(name, ("sceIoDread", "sceIoDclose", "sceIoRead", "sceIoClose"))
        self.assertNotIn("DREAD-PRIVATE-NOT-NULL", run.lines)

    def test_listing_is_copied_byte_for_byte_and_verified(self) -> None:
        files = {"a.bin": (100, 7), "large.bin": (140_000, 9), "empty.bin": (0, 3),
                 "chunk.bin": (65_536, 5)}
        lines = [_entry(name, size, seed) for name, (size, seed) in files.items()]
        lines += [
            _entry(".", mode=_DIRECTORY), _entry("sub", mode=_DIRECTORY),
            _entry("../escape", 10, 1), _entry("has space", 10, 1), _entry(".hidden", 10, 1),
            _entry("x" * 64, 10, 1), _entry("huge.bin", 10, 1, listed=16 * 1024 * 1024 + 1),
            _entry("neg.bin", 10, 1, listed=-1),
        ]
        run = self._run("fonts", lines)
        self._assert_font_calls_safe(run)
        report = parse_font_files_output(run.stream)
        self.assertTrue(report.complete and report.listed)
        self.assertEqual([entry.disposition for entry in report.entries],
                         [0, 0, 0, 0, 1, 1, 2, 2, 2, 2, 3, 3])
        self.assertEqual({copy.name for copy in report.copies}, set(files))
        for copy in report.copies:
            size, seed = files[copy.name]
            digest = hashlib.sha256(_gen_bytes(size, seed)).hexdigest()
            self.assertEqual((copy.sha256, copy.readback_sha256, copy.reread_sha256),
                             (digest, digest, digest), copy.name)
            self.assertTrue(copy.verified)
            self.assertEqual(run.host0[copy.name], (size, True))
        self.assertEqual(set(run.host0), set(files))
        opened_flash = [fields["path"] for name, fields in run.calls
                        if name == "sceIoOpen" and fields["path"].startswith("flash0:")]
        self.assertEqual(sorted(set(opened_flash)), sorted(f"flash0:/font/{name}" for name in files))

    def test_a_listing_that_cannot_open_stops_the_case_cleanly(self) -> None:
        run = self._run("fonts", ["fontdir 0x80010002 0x80010002", _entry("a.bin", 10, 1)])
        self.assertEqual([name for name, _ in run.calls], ["sceIoDopen", "sceIoDopen"])
        report = parse_font_files_output(run.stream)
        self.assertFalse(report.listed)
        self.assertEqual(report.entries, ())
        first = dict(parse_output(run.stream).results[0].values)
        self.assertEqual((first["out0"], first["out1"]), ("0x80010002", "0x80010002"))
        run = self._run("fonts", ["fontdir 0x80010002 0", _entry("a.bin", 10, 1)])
        self.assertTrue(parse_font_files_output(run.stream).copies[0].verified)

    def test_failures_are_recorded_per_file_and_the_case_continues(self) -> None:
        names = ["readfail", "corrupt", "unstable", "hostopen", "writefail", "short", "sized",
                 "ok"]
        lines = [_entry(name, 70_000, index + 1) for index, name in enumerate(names)]
        lines[names.index("sized")] = _entry("sized", 70_000, 7, listed=70_001)
        lines += [
            "option read-fails readfail 65536 0x80010005",
            "option host-corrupt corrupt",
            "option unstable unstable",
            "option host-open-fails hostopen 0x80010013",
            "option write-fails writefail 0x8001001c",
            "option short-writes short",
        ]
        run = self._run("fonts", lines)
        self._assert_font_calls_safe(run)
        report = parse_font_files_output(run.stream)
        copies = {copy.name: copy for copy in report.copies}
        self.assertEqual({name for name, copy in copies.items() if copy.verified}, {"short", "ok"})
        self.assertEqual(copies["corrupt"].match, 6)
        self.assertEqual(copies["unstable"].match, 5)
        self.assertEqual(copies["sized"].match, 3)
        records = {dict(record.values).get("detail"): record for record in
                   parse_output(run.stream).results if record.case_id.startswith("font-copy-")}
        self.assertEqual(dict(records["readfail"].values)["result"], "0x80010005")
        self.assertEqual(dict(records["hostopen"].values)["result"], "0x80010013")
        self.assertEqual(dict(records["writefail"].values)["result"], "0x8001001c")
        self.assertEqual(int(dict(parse_output(run.stream).results[-1].values)["out2"], 16), 2)

    def test_listing_errors_and_the_entry_bound_end_the_listing(self) -> None:
        run = self._run("fonts", [_entry(f"f{i}.bin", 4, i) for i in range(5)]
                        + ["option dread-fails 3 0x80010005"])
        report = parse_font_files_output(run.stream)
        self.assertEqual(len(report.entries), 3)
        self.assertEqual(len(report.copies), 3)
        close = next(record for record in parse_output(run.stream).results
                     if record.case_id == "font-dir-close")
        self.assertEqual(close.status, "FAIL")
        self.assertEqual(dict(close.values)["out1"], "0x80010005")
        run = self._run("fonts", [_entry(f"f{i}.bin", 1, i) for i in range(70)])
        report = parse_font_files_output(run.stream)
        self.assertEqual(len(report.entries), 64)
        dreads = [name for name, _ in run.calls if name == "sceIoDread"]
        self.assertEqual(len(dreads), 64)

    def test_font_parser_rejects_inconsistent_streams(self) -> None:
        run = self._run("fonts", [_entry("a.bin", 100, 7), _entry("b.bin", 100, 8)])
        self.assertTrue(parse_font_files_output(run.stream).complete)
        bad = (
            run.stream.replace("out8=0x00000007 detail=a.bin", "out8=0x00000006 detail=a.bin", 1),
            run.stream.replace("detail=b.bin sha256", "detail=c.bin sha256", 1),
            run.stream.replace("out4=0x00000000 detail=a.bin", "out4=0x00000002 detail=a.bin", 1),
            "".join(line + "\n" for line in run.stream.splitlines()
                    if "case_id=font-copy-0001 " not in line),
        )
        for stream in bad:
            self.assertNotEqual(stream, run.stream)
            with self.assertRaises(ProtocolError):
                parse_font_files_output(stream)
        truncated = run.stream.rsplit("NAKAGAWA_PSP_TEST", 1)[0]
        with self.assertRaises(ProtocolError):
            parse_font_files_output(truncated)
        self.assertFalse(parse_font_files_output(truncated, require_complete=False).complete)

    def test_device_case_opens_read_only_and_closes_everything(self) -> None:
        for umd_inserted in (False, True):
            lines = ["umd 0x0 0x1"]
            if umd_inserted:
                lines = ["umd 0x1 0x12", f"open {_hex('umd0:')} 0x0",
                         f"open {_hex('disc0:/UMD_DATA.BIN')} 0x0",
                         f"dopen {_hex('disc0:/')} 0x0", f"dopen {_hex('umd0:')} 0x80010014"]
            run = self._run("umd", lines)
            names = [name for name, _ in run.calls]
            self.assertLessEqual(set(names), {"sceIoOpen", "sceIoClose", "sceIoDopen",
                                              "sceIoDclose", "sceUmdCheckMedium",
                                              "sceUmdGetDriveStat"})
            for name, fields in run.calls:
                if name == "sceIoOpen":
                    self.assertIn(int(fields["flags"], 16), (0x1, 0xF0001))
            opens = [(fields["path"], int(fields["flags"], 16)) for name, fields in run.calls
                     if name == "sceIoOpen"]
            self.assertEqual(opens, list(UMDIO_OPENS))
            dopens = [fields["path"] for name, fields in run.calls if name == "sceIoDopen"]
            self.assertEqual(dopens, list(UMDIO_DOPENS))
            report = parse_umd_io_output(run.stream)
            self.assertTrue(report.complete)
            succeeded = [item for item in report.opens if item[2] < 0x80000000]
            self.assertEqual(len(succeeded), 3 if umd_inserted else 0)
            self.assertTrue(all(item[3] == 0 for item in succeeded))
            self.assertEqual(report.before, (1, 0x12) if umd_inserted else (0, 1))
            self.assertEqual(names.count("sceIoClose"), len(succeeded))
            self.assertEqual(names.count("sceIoDclose"), 1 if umd_inserted else 0)

    def test_device_parser_rejects_a_stream_that_differs_from_the_source(self) -> None:
        run = self._run("umd", ["umd 0x0 0x1"])
        for bad in (
            run.stream.replace("detail=umd1:\n", "detail=umd2:\n", 1),
            run.stream.replace("out0=0x000f0001", "out0=0x00000001", 1),
            run.stream.replace("result=0x80010002 out0=0x00000001 out1=0xffffffff",
                               "result=0x80010002 out0=0x00000001 out1=0x00000000", 1),
            "".join(line + "\n" for line in run.stream.splitlines()
                    if "case_id=io-dopen-0002 " not in line),
        ):
            self.assertNotEqual(bad, run.stream)
            with self.assertRaises(ProtocolError):
                parse_umd_io_output(bad)

    def test_raw_device_case_is_bounded_read_only_and_waits_before_close(self) -> None:
        run = self._run("umdraw", [
            "umd 0x1 0x32",
            f"open {_hex('umd0:')} 0x0",
        ])
        names = [name for name, _ in run.calls]
        allowed = {
            "sceUmdCheckMedium", "sceUmdGetDriveStat", "sceIoOpen", "sceIoClose",
            "sceIoLseek", "sceIoLseek32", "sceIoRead", "sceIoReadAsync",
            "sceIoPollAsync", "sceIoWaitAsync", "sceIoGetstat",
        }
        self.assertLessEqual(set(names), allowed)
        opens = [(fields["path"], int(fields["flags"], 16))
                 for name, fields in run.calls if name == "sceIoOpen"]
        self.assertEqual(opens, [("umd0:", UMDRAW_RAW_FLAGS),
                                  ("umd0:", UMDRAW_READONLY_FLAGS)])
        self.assertEqual(names.count("sceIoClose"), 2)
        requests = [int(fields["size"], 0) for name, fields in run.calls
                    if name in {"sceIoRead", "sceIoReadAsync"}]
        self.assertTrue(requests)
        self.assertLessEqual(max(requests), 32768)
        async_index = names.index("sceIoReadAsync")
        self.assertEqual(names[async_index + 1:async_index + 3],
                         ["sceIoPollAsync", "sceIoWaitAsync"])
        self.assertGreater(names.index("sceIoClose"), async_index + 2)

        report = parse_umd_raw_read_output(run.stream)
        self.assertTrue(report.complete)
        self.assertEqual(len(report.parsed.results), len(UMDRAW_CASES))
        records = {record.case_id: dict(record.values) for record in report.parsed.results}
        self.assertEqual(records["byte-sector16-read"]["out0"], "0x30444301")
        self.assertEqual(records["byte-sector16-read"]["out1"], "0x00013130")
        self.assertEqual(int(records["pos-advance-cur"]["result"], 0), 4112)
        self.assertEqual(records["size-0-read"]["out2"], "0x00000001")
        self.assertEqual(int(records["far-read"]["result"], 0), 0)
        self.assertEqual(report.before, (1, 0x32))
        self.assertEqual(report.after, (1, 0x32))

        payload = run.stream.replace(
            "case_id=byte-sector16-read status=PASS ",
            "case_id=byte-sector16-read status=PASS payload=0x12345678 ", 1,
        )
        with self.assertRaises(ProtocolError):
            parse_umd_raw_read_output(payload)

    def test_raw_device_open_failure_does_not_touch_an_invalid_descriptor(self) -> None:
        run = self._run("umdraw", [
            "umd 0x1 0x32",
            f"open {_hex('umd0:')} 0x80010002",
        ])
        names = [name for name, _ in run.calls]
        self.assertEqual(names.count("sceIoOpen"), 1)
        self.assertEqual(names.count("sceIoClose"), 0)
        self.assertEqual(names.count("sceIoRead"), 0)
        self.assertEqual(names.count("sceIoReadAsync"), 0)
        self.assertEqual(names.count("sceIoLseek"), 0)
        self.assertEqual(names.count("sceUmdCheckMedium"), 2)
        self.assertEqual(names.count("sceUmdGetDriveStat"), 2)
        with self.assertRaises(ProtocolError):
            parse_umd_raw_read_output(run.stream)

    def test_measured_device_results_match_the_console_table(self) -> None:
        tid = parse_golden.UMDIO_TEST_ID

        def record(case_id: str, fields: str) -> str:
            return (f"NAKAGAWA_PSP_TEST schema=1 test_id={tid} case_id={case_id} "
                    f"status=PASS {fields}\n")

        records = [record("umd-state-before", "result=0x00000001 out0=0x00000032")]
        for index, ((path, flags), (result, close_result)) in enumerate(
                zip(UMDIO_OPENS, parse_golden.UMDIO_MEASURED_OPEN_RESULTS, strict=True)):
            records.append(record(
                f"io-open-{index:04d}",
                f"result=0x{result:08x} out0=0x{flags:08x} "
                f"out1=0x{close_result:08x} detail={path}",
            ))
        for index, (path, (result, close_result)) in enumerate(
                zip(UMDIO_DOPENS, parse_golden.UMDIO_MEASURED_DOPEN_RESULTS, strict=True)):
            records.append(record(
                f"io-dopen-{index:04d}",
                f"result=0x{result:08x} out0=0x{close_result:08x} detail={path}",
            ))
        records.append(record("umd-state-after", "result=0x00000001 out0=0x00000032"))
        pre_terminal_count = len(records)
        open_successes = sum(result < 0x80000000
                             for result, _ in parse_golden.UMDIO_MEASURED_OPEN_RESULTS)
        dopen_successes = sum(result < 0x80000000
                              for result, _ in parse_golden.UMDIO_MEASURED_DOPEN_RESULTS)
        records.append(record(
            "umdio-done",
            f"result=0x00000000 out0=0x{len(UMDIO_OPENS):08x} "
            f"out1=0x{open_successes:08x} out2=0x{len(UMDIO_DOPENS):08x} "
            f"out3=0x{dopen_successes:08x} out4=0x{pre_terminal_count:08x}",
        ))
        stream = META.format(source="psp", model="PSP-3000", firmware="6.61-ARK",
                             binary=MEASURED_SHA, commit=MEASURED_COMMIT) + "".join(records)
        report = parse_umd_io_output(stream, require_measured=True)
        self.assertEqual(report.before, parse_golden.UMDIO_MEASURED_STATE)
        self.assertEqual(report.after, parse_golden.UMDIO_MEASURED_STATE)

        mismatched = stream.replace(
            "case_id=io-open-0000 status=PASS result=0x00000003",
            "case_id=io-open-0000 status=PASS result=0x00000004", 1,
        )
        with self.assertRaisesRegex(ProtocolError, "sceIoOpen results differ"):
            parse_umd_io_output(mismatched, require_measured=True)

    def test_sha256_matches_the_standard_vectors(self) -> None:
        run = self._run("fonts", [_entry("abc", 0, 0)])
        empty = hashlib.sha256(b"").hexdigest()
        self.assertEqual(parse_font_files_output(run.stream).copies[0].sha256, empty)
        for size in (55, 56, 63, 64, 65, 119, 120, 128, 1000):
            run = self._run("fonts", [_entry("v", size, 11)])
            self.assertEqual(parse_font_files_output(run.stream).copies[0].sha256,
                             hashlib.sha256(_gen_bytes(size, 11)).hexdigest(), size)


class IoCasesSourceTests(unittest.TestCase):
    """Source-level rules for the font-files and umd-io cases."""

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.fonts = (self.fixture / "font_files.h").read_text(encoding="utf-8")
        self.umd = (self.fixture / "umd_io.h").read_text(encoding="utf-8")
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")
        self.makefile = (self.fixture / "Makefile").read_text(encoding="utf-8")

    @staticmethod
    def _code(text: str) -> str:
        return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)

    def test_flash0_is_only_listed_and_opened_read_only(self) -> None:
        code = self._code(self.fonts)
        self.assertEqual(set(re.findall(r"\b(sce\w+)\s*\(", code)),
                         {"sceIoDopen", "sceIoDread", "sceIoDclose", "sceIoOpen", "sceIoRead",
                          "sceIoWrite", "sceIoClose", "sceIoMkdir"})
        opens = re.findall(r"sceIoOpen\(([^;]*?)\);", code, re.DOTALL)
        self.assertEqual([" ".join(item.split()) for item in opens], [
            "path, PSP_O_RDONLY, 0",
            "s_fontfiles_path, PSP_O_RDONLY, 0",
            "s_fontfiles_host, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0644",
        ])
        self.assertIn('#define FONTFILES_HOST_DIR "host0:/fonts"', self.fonts)
        self.assertIn('snprintf(s_fontfiles_host, sizeof(s_fontfiles_host), "%s/%.63s", '
                      'FONTFILES_HOST_DIR, name);', self.fonts)
        self.assertEqual(re.findall(r"sceIoMkdir\(([^)]*)\)", code), ["FONTFILES_HOST_DIR, 0777"])
        self.assertNotRegex(code, r"\bmalloc\s*\(|\bcalloc\s*\(|sceIoRemove|sceIoRename")

    def test_device_case_reads_state_only_and_its_lists_match_the_parser(self) -> None:
        code = self._code(self.umd[:self.umd.index("#define UMDRAW_TEST_ID")])
        self.assertEqual(set(re.findall(r"\b(sce\w+)\s*\(", code)),
                         {"sceIoOpen", "sceIoClose", "sceIoDopen", "sceIoDclose",
                          "sceUmdCheckMedium", "sceUmdGetDriveStat"})
        flags = {"PSP_O_RDONLY": 0x1, "UMDIO_RAW_FLAGS": 0xF0001}
        opens = [(path, flags[flag]) for path, flag in
                 re.findall(r'\{"([^"]+)", (PSP_O_RDONLY|UMDIO_RAW_FLAGS)\}', code)]
        self.assertEqual(tuple(opens), UMDIO_OPENS)
        block = code[code.index("s_umdio_dopens[] = {"):]
        block = block[:block.index("};")]
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', block)), UMDIO_DOPENS)
        line = next(line for line in self.makefile.splitlines()
                    if "--exact-library sceUmdUser=" in line)
        self.assertEqual(line.split("sceUmdUser=", 1)[1], "0x46EBB729,0x6B4A146C")

    def test_raw_device_case_is_read_only_and_pins_exact_user_io_imports(self) -> None:
        raw = self.umd[self.umd.index("#define UMDRAW_TEST_ID"):]
        code = self._code(raw)
        calls = set(re.findall(r"\b(sce\w+)\s*\(", code))
        self.assertEqual(calls, {
            "sceIoOpen", "sceIoClose", "sceIoLseek", "sceIoLseek32", "sceIoRead",
            "sceIoReadAsync", "sceIoPollAsync", "sceIoWaitAsync", "sceIoGetstat",
            "sceUmdCheckMedium", "sceUmdGetDriveStat",
        })
        self.assertNotRegex(code, r"\bsceIo(?:Write|Devctl|Ioctl|Chstat)\s*\(")
        self.assertNotRegex(code, r"\bsceUmd(?:Activate|Deactivate)\s*\(")
        self.assertIn("#define UMDRAW_BUFFER_BYTES 32768u", raw)
        opens = re.findall(r"sceIoOpen\(([^;]*?)\);", code, re.DOTALL)
        self.assertEqual([" ".join(item.split()) for item in opens], [
            '"umd0:", UMDIO_RAW_FLAGS, 0',
            '"umd0:", PSP_O_RDONLY, 0',
        ])
        body = code[code.index("static void umdraw_run("):]
        body = body[:body.index("\n}")]
        self.assertLess(body.index("sceIoReadAsync"), body.index("sceIoPollAsync"))
        self.assertLess(body.index("sceIoPollAsync"), body.index("sceIoWaitAsync"))
        self.assertLess(body.index("sceIoWaitAsync"), body.index("sceIoClose"))
        self.assertEqual(UMDRAW_READ_SIZES, (1, 512, 2047, 2048, 2049, 4096, 32768, 0))
        self.assertEqual(set(UMDRAW_HEADER_READ_SIZES), {
            "pos-advance-first", "pos-advance-second", "byte-sector16-read",
            "size-1-read", "size-512-read", "size-2047-read", "size-2048-read",
            "size-2049-read", "size-4096-read", "size-32768-read", "size-0-read",
            "async-data",
        })
        pin = next(line for line in self.makefile.splitlines()
                   if line.startswith("USER_MODE_IMPORT_GATE_ARGS += --exact-library IoFileMgrForUser="))
        actual_nids = tuple(int(value, 16)
                            for value in pin.split("IoFileMgrForUser=", 1)[1].split(","))
        self.assertEqual(actual_nids, UMDRAW_IOFILEMGR_NIDS)

    def test_raw_read_cells_mark_seek_and_read_separately(self) -> None:
        raw = self._code(self.umd[self.umd.index("#define UMDRAW_TEST_ID"):])
        read_at = raw[raw.index("static void umdraw_read_at("):]
        read_at = read_at[:read_at.index("\nstatic void umdraw_skip_primary")]
        self.assertLess(read_at.index("umdraw_step(u, seek_case);"),
                        read_at.index("sceIoLseek("))
        self.assertLess(read_at.index("umdraw_step(u, read_case);"),
                        read_at.index("sceIoRead("))

        run = raw[raw.index("static void umdraw_run("):]
        run = run[:run.index("\n}")]
        self.assertIn('umdraw_step(u, "pos-advance-seek");', run)
        self.assertIn('umdraw_step(u, "pos-advance-read-pair");', run)
        self.assertIn('umdraw_step(u, "pos-advance-cur");', run)
        read_async = run.index("sceIoReadAsync(")
        poll_async = run.index("sceIoPollAsync(")
        wait_async = run.index("sceIoWaitAsync(")
        self.assertLess(read_async, poll_async)
        self.assertNotIn("umdraw_step(", run[read_async:poll_async])
        self.assertLess(run.index('umdraw_step(u, "async-wait");'), wait_async)

    def test_raw_device_import_pin_rejects_unexpected_io_and_umd_imports(self) -> None:
        io_pin = "IoFileMgrForUser=" + ",".join(
            f"0x{nid:08X}" for nid in UMDRAW_IOFILEMGR_NIDS)
        umd_pin = "sceUmdUser=0x46EBB729,0x6B4A146C"
        allowed = [
            ("IoFileMgrForUser", list(UMDRAW_IOFILEMGR_NIDS)),
            ("sceUmdUser", [0x46EBB729, 0x6B4A146C]),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp) / "probe.elf"
            module.write_bytes(build_import_elf(allowed))
            with redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()) as err:
                status = user_mode_imports.main([
                    "--exact-library", io_pin, "--exact-library", umd_pin, str(module),
                ])
            self.assertEqual(status, user_mode_imports.EXIT_OK, out.getvalue() + err.getvalue())

            extra = [
                ("IoFileMgrForUser", [*UMDRAW_IOFILEMGR_NIDS, 0x12345678]),
                ("sceUmdUser", [0x46EBB729, 0x6B4A146C]),
            ]
            module.write_bytes(build_import_elf(extra))
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
                status = user_mode_imports.main([
                    "--exact-library", io_pin, "--exact-library", umd_pin, str(module),
                ])
            self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
            self.assertIn("unexpected 0x12345678", err.getvalue())

    def test_runtime_umd_expectations_match_the_measured_parser_table(self) -> None:
        source = (self.root / "src" / "rt" / "hle_thread_selftest.c").read_text(
            encoding="utf-8")

        def c_int(value: str) -> int:
            return int(value.rstrip("uU"), 0)

        open_block = re.search(
            r"static const UmdOpenCase opens\[\] = \{(.*?)\n    \};", source, re.DOTALL)
        self.assertIsNotNone(open_block)
        actual_opens = tuple(
            (path, c_int(flags), c_int(result))
            for path, flags, result in re.findall(
                r'\{"([^"]+)",\s*(0x[0-9a-fA-F]+u?),\s*(0x[0-9a-fA-F]+u?|[0-9]+u?)\}',
                open_block.group(1),
            )
        )
        expected_opens = tuple(
            (path, flags, result[0])
            for (path, flags), result in zip(
                UMDIO_OPENS, parse_golden.UMDIO_MEASURED_OPEN_RESULTS, strict=True)
        )
        self.assertEqual(actual_opens, expected_opens)

        dopen_block = re.search(
            r"static const UmdDopenCase dopens\[\] = \{(.*?)\n    \};", source, re.DOTALL)
        self.assertIsNotNone(dopen_block)
        actual_dopens = tuple(
            (path, c_int(result))
            for path, result in re.findall(
                r'\{"([^"]+)",\s*(0x[0-9a-fA-F]+u?|[0-9]+u?)\}',
                dopen_block.group(1),
            )
        )
        expected_dopens = tuple(
            (path, result[0])
            for path, result in zip(
                UMDIO_DOPENS, parse_golden.UMDIO_MEASURED_DOPEN_RESULTS, strict=True)
        )
        self.assertEqual(actual_dopens, expected_dopens)

    def test_cases_are_wired_like_the_others(self) -> None:
        for case, case_id, log in (("font-files", 73, "font_files_log.txt"),
                                   ("umd-io", 74, "umd_io_log.txt"),
                                   ("umd-raw-read", 77, "umd_raw_read_log.txt")):
            self.assertRegex(self.makefile,
                             rf"(?m)^else ifeq \(\$\(CASE\),{case}\)\nCASE_ID = {case_id}$")
            self.assertIn(f'#define PROBE_HOST0_LOG "host0:/{log}"', self.probe)
            self.assertEqual(_campaign_host0_log_path(Path("host0"), case).name, log)
            self.assertEqual(_campaign_completeness_contract(case), "strict-golden-sequence")
        self.assertIn("umd-io", CAMPAIGN_QUEUE_CASES)
        self.assertIn("umd-raw-read", CAMPAIGN_QUEUE_CASES)
        self.assertNotIn("font-files", CAMPAIGN_QUEUE_CASES)
        self.assertIn("$(BUILD_DIR)/probe.o: probe.c registry_walk.h font_files.h umd_io.h",
                      self.makefile)
        for macro in ("FONT_FILES", "UMD_IO", "UMD_RAW_READ"):
            if macro == "UMD_RAW_READ":
                marker = "/* Read-only raw UMD descriptor observations"
                marker_at = self.probe.index(marker)
                start = self.probe.rfind(
                    f"#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_{macro}\n", 0, marker_at
                )
            else:
                start = self.probe.index(f"#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_{macro}\n")
            block = self.probe[start:]
            block = block[:block.index("\n#endif")]
            self.assertNotRegex(block, r"\bsce\w+\s*\(")

    def test_raw_device_case_manifest_matches_its_captured_record_contract(self) -> None:
        manifest = json.loads((self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
            encoding="utf-8"))
        entry = next(item for item in manifest["tests"] if item["id"] == "PSP-UMDRAW-001")
        self.assertEqual(entry["status"], "implemented")
        self.assertEqual(entry["hardware_evidence"], "CAPTURED")
        self.assertEqual(entry["case_ids"], list(UMDRAW_CASES))
        captured = (
            "umd-state-before", "raw-open-extra", "seek-set-0", "seek-set-16",
            "seek-set-32768", "seek-cur-0", "seek-end-0", "seek-set-1",
            "seek-set-neg1", "seek32-set-0", "seek32-set-16", "seek32-set-32768",
            "seek32-end-0", "pos-advance-seek", "pos-advance-first",
            "pos-advance-second", "pos-advance-cur", "byte-sector16-seek",
            "byte-sector16-read", "size-1-seek", "size-1-read", "size-512-seek",
            "size-512-read", "size-2047-seek", "size-2047-read", "size-2048-seek",
            "size-2048-read", "size-2049-seek", "size-2049-read", "size-4096-seek",
            "size-4096-read",
        )
        self.assertEqual(entry["evidence_cases"], list(captured))
        oracle_docs = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        self.assertIn("PSP-UMDRAW-001", oracle_docs)
        self.assertIn("| `umd-raw-read` | `PSP-UMDRAW-001` | `CAPTURED`", oracle_docs)
        self.assertIn("size-32768-read", oracle_docs)
        self.assertIn("900-second host case timeout", entry["measurement_note"])
        self.assertIn("27.2 seconds after launch", entry["measurement_note"])
        self.assertIn("does not establish whether the PSP call hung", entry["measurement_note"])
        self.assertIn("1800-second host case timeout", entry["measurement_note"])
        self.assertIn("usbipd listed the PSP as absent at 3m46 after launch", entry["measurement_note"])
        self.assertIn("L1 reattach found the PSP absent", entry["measurement_note"])
        self.assertIn("at 3m46, before the 1800-second deadline", oracle_docs)
        self.assertIn("fresh physical power-cycle", oracle_docs)

    def test_umd_pin_refuses_an_activation_import(self) -> None:
        pin = ["--exact-library", "sceUmdUser=0x46EBB729,0x6B4A146C"]
        for nids, expected in (([0x46EBB729, 0x6B4A146C], user_mode_imports.EXIT_OK),
                               ([0x46EBB729, 0x6B4A146C, 0xC6183D47],
                                user_mode_imports.EXIT_KERNEL_IMPORT)):
            data = build_import_elf([("sceUmdUser", nids)])
            with tempfile.TemporaryDirectory() as tmp:
                module = Path(tmp) / "probe.elf"
                module.write_bytes(data)
                with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as err:
                    status = user_mode_imports.main([*pin, str(module)])
            self.assertEqual(status, expected, err.getvalue())


FONT_LIST_NIDS = [0x67F17ED7, 0x574B6FBC, 0x27F6E642, 0xBC75D85B, 0x5333322D]


def _font_style_hex(*, h: float = 10.0, v: float = 10.0, hres: float = 128.0, vres: float = 128.0,
                    weight: float = 0.0, family: int = 1, style: int = 1, sub: int = 0,
                    language: int = 2, region: int = 0, country: int = 0, name: str = "FONT",
                    file: str = "font.pgf", attributes: int = 0, expire: int = 0) -> str:
    """168 style bytes in the public layout, as the fake library serves them."""
    import struct

    raw = struct.pack("<5f6H", h, v, hres, vres, weight, family, style, sub, language, region,
                      country)
    raw += name.encode("ascii").ljust(64, b"\0") + file.encode("ascii").ljust(64, b"\0")
    raw += struct.pack("<II", attributes, expire)
    assert len(raw) == 168
    return raw.hex()


@unittest.skipIf(_HOST_CC is None, "no host C compiler for the font-list harness")
class FontListHostTests(unittest.TestCase):
    """font_list.h against io_cases_host.c's fake font library: call order, bounds and records."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(__file__).resolve().parents[1]
        cls.fixture = cls.root / "fixtures" / "psp_oracle"
        cls.tmp = tempfile.mkdtemp(prefix="font_list_host_")
        cls.exe = str(Path(cls.tmp) / "io_cases_host.exe")
        result = subprocess.run(
            [_HOST_CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
             f"-I{cls.fixture}", "-o", cls.exe, str(cls.fixture / "io_cases_host.c")],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError("io_cases_host.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _run(self, lines: list[str]) -> SimpleNamespace:
        result = subprocess.run([self.exe, "fontlist"], input="\n".join(lines) + "\n",
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        out = result.stdout.splitlines()
        self.assertEqual(out[-1], "SUMMARY open_fds=0")
        calls = [line.split()[1] for line in out if line.startswith("CALL ")]
        allocs = [line for line in out if line.startswith("ALLOC ")]
        steps = [line[5:] for line in out if line.startswith("STEP ")]
        emitted = "".join(line[5:] + "\n" for line in out if line.startswith("EMIT "))
        stream = META.format(source="psp", model="PSP-3000", firmware="6.6.1",
                             binary=MEASURED_SHA, commit=MEASURED_COMMIT) + emitted
        return SimpleNamespace(lines=out, calls=calls, allocs=allocs, steps=steps, stream=stream)

    def test_three_fonts_are_listed_with_their_raw_bytes_and_nothing_else_is_called(self) -> None:
        styles = [_font_style_hex(name=f"F{i}", file=f"f{i}.pgf", language=i + 1, family=i)
                  for i in range(3)]
        lines = ["fontlib 0x1234 0 2 4096", "fontcount 3 0"]
        lines += [f"fontentry {i} {style}" for i, style in enumerate(styles)]
        run = self._run(lines)
        self.assertEqual(run.calls, ["sceFontNewLib", "sceFontGetNumFontList", "sceFontGetFontList",
                                     "sceFontGetFontInfoByIndexNumber",
                                     "sceFontGetFontInfoByIndexNumber",
                                     "sceFontGetFontInfoByIndexNumber", "sceFontDoneLib"])
        self.assertEqual(run.steps, ["newlib", "count", "list", "info:0000", "info:0001",
                                     "info:0002", "donelib"])
        self.assertIn("CALL sceFontNewLib numFonts=1 alloc=1 free=1 open=0 close=0 read=0 seek=0",
                      run.lines)
        report = parse_font_list_output(run.stream)
        self.assertTrue(report.complete)
        self.assertTrue(report.library_ok)
        self.assertEqual((report.library, report.count, report.requested, report.failures),
                         (0x1234, 3, 3, 0))
        self.assertEqual([entry.raw.hex() for entry in report.entries], styles)
        self.assertEqual([info.raw.hex() for info in report.infos], styles)
        # word 5 packs family (low half) and style (high half): entry 2 was built with family 2.
        self.assertEqual(report.entries[2].words[5], (1 << 16) | 2)
        # The library allocated twice through the arena and freed both blocks in DoneLib.
        self.assertEqual(len(run.allocs), 2)
        done = dict(report.parsed.results[-1].values)
        self.assertEqual([int(done[f"out{i}"], 0) for i in range(7)], [3, 3, 8192, 2, 2, 0, 0])

    def test_a_library_that_does_not_open_ends_the_case_after_one_record(self) -> None:
        run = self._run(["fontlib 0 0x80460001 1 4096", "fontcount 3 0"])
        self.assertEqual(run.calls, ["sceFontNewLib"])
        self.assertEqual(run.steps, ["newlib"])
        report = parse_font_list_output(run.stream)
        self.assertFalse(report.library_ok)
        self.assertEqual((report.library, report.count, report.requested, report.failures),
                         (0, None, 0, 1))
        self.assertEqual(int(dict(report.parsed.results[0].values)["out0"], 0), 0x80460001)

    def test_an_allocation_the_arena_cannot_hold_is_refused_not_grown(self) -> None:
        too_large = 512 * 1024 + 16
        run = self._run([f"fontlib 0x1234 0 1 {too_large}", "fontcount 3 0"])
        self.assertEqual(run.calls, ["sceFontNewLib"])
        self.assertIn(f"ALLOC bytes={too_large} ok=0", run.lines)
        report = parse_font_list_output(run.stream)
        self.assertEqual(report.library, 0)
        newlib = dict(report.parsed.results[0].values)
        self.assertEqual((int(newlib["out3"], 0), int(newlib["out4"], 0)), (1, 1))

    def test_a_count_above_the_bound_is_recorded_and_the_listing_is_clamped(self) -> None:
        styles = [_font_style_hex(name=f"F{i}") for i in range(70)]
        lines = ["fontlib 0x1234 0 1 4096", "fontcount 70 0"]
        lines += [f"fontentry {i} {style}" for i, style in enumerate(styles)]
        run = self._run(lines)
        report = parse_font_list_output(run.stream)
        self.assertEqual((report.count, report.requested, report.failures), (70, 64, 1))
        self.assertEqual(len(report.entries), 64)
        self.assertEqual(len(report.infos), 64)
        self.assertEqual(run.calls.count("sceFontGetFontInfoByIndexNumber"), 64)

    def test_an_info_error_is_a_failed_record_and_the_case_still_closes_the_library(self) -> None:
        styles = [_font_style_hex(name=f"F{i}") for i in range(2)]
        lines = ["fontlib 0x1234 - 1 4096", "fontcount 2 -", "fontinfo-rc 1 0x80460006"]
        lines += [f"fontentry {i} {style}" for i, style in enumerate(styles)]
        run = self._run(lines)
        report = parse_font_list_output(run.stream)
        self.assertTrue(report.library_ok)  # error words the library never wrote stay clean
        self.assertEqual([info.ok for info in report.infos], [True, False])
        self.assertEqual(report.failures, 1)
        self.assertEqual(run.calls[-1], "sceFontDoneLib")

    def test_a_zero_count_skips_the_list_and_still_closes_the_library(self) -> None:
        run = self._run(["fontlib 0x1234 0 1 4096", "fontcount 0 0"])
        self.assertEqual(run.calls, ["sceFontNewLib", "sceFontGetNumFontList", "sceFontDoneLib"])
        report = parse_font_list_output(run.stream)
        self.assertEqual((report.count, report.requested, report.failures), (0, 0, 0))

    def test_parser_refuses_truncated_or_reordered_streams(self) -> None:
        styles = [_font_style_hex(name=f"F{i}") for i in range(2)]
        lines = ["fontlib 0x1234 0 1 4096", "fontcount 2 0"]
        lines += [f"fontentry {i} {style}" for i, style in enumerate(styles)]
        body = self._run(lines).stream.splitlines()
        done = [line for line in body if "case_id=font-list-done " in line]
        self.assertEqual(len(done), 1)
        truncated = "\n".join(line for line in body if line != done[0]) + "\n"
        with self.assertRaisesRegex(ProtocolError, "missing font-list-done"):
            parse_font_list_output(truncated)
        self.assertFalse(parse_font_list_output(truncated, require_complete=False).complete)
        entries = [line for line in body if "case_id=font-list-entry-" in line]
        swapped = []
        for line in body:
            if line == entries[0]:
                swapped.append(entries[1])
            elif line == entries[1]:
                swapped.append(entries[0])
            else:
                swapped.append(line)
        with self.assertRaisesRegex(ProtocolError, "out of sequence"):
            parse_font_list_output("\n".join(swapped) + "\n")
        bad_raw = "\n".join(line.replace(" raw_hex=", " raw_hex=00", 1) if line == entries[0] else line
                            for line in body) + "\n"
        with self.assertRaisesRegex(ProtocolError, "raw_hex"):
            parse_font_list_output(bad_raw)


class FontListImportGateTests(unittest.TestCase):
    """The font-list case may import exactly five sceLibFont calls and nothing from sceReg."""

    PIN = ["--exact-library", "sceLibFont=0x67F17ED7,0x574B6FBC,0x27F6E642,0xBC75D85B,0x5333322D",
           "--forbid-library", "sceReg"]

    def _main(self, args: list[str], data: bytes) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp) / "probe.elf"
            module.write_bytes(data)
            out = io.StringIO()
            err = io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                status = user_mode_imports.main([*args, str(module)])
        return status, out.getvalue(), err.getvalue()

    def test_forbid_library_refuses_even_a_read_only_registry_import(self) -> None:
        data = build_import_elf([("sceLibFont", FONT_LIST_NIDS), ("sceReg", [0x92E41280])])
        self.assertTrue(user_mode_imports.check_module(data).passed)
        result = user_mode_imports.check_module(data, None, frozenset({"sceReg"}))
        self.assertFalse(result.passed)
        self.assertEqual([(v.library, v.nids) for v in result.violations],
                         [("sceReg", (0x92E41280,))])
        self.assertIn("--forbid-library", result.violations[0].reason)
        status, out, err = self._main(["--forbid-library", "sceReg"], data)
        self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
        self.assertEqual(out, "")
        self.assertIn("sceReg (imports from a library this case forbids", err)

    def test_the_font_list_pin_accepts_exactly_the_five_calls(self) -> None:
        exact = build_import_elf([("sceLibFont", FONT_LIST_NIDS)])
        status, out, err = self._main(self.PIN, exact)
        self.assertEqual(status, user_mode_imports.EXIT_OK, err)
        self.assertIn("OK", out)
        extra = build_import_elf([("sceLibFont", FONT_LIST_NIDS + [0x099EF33C])])
        status, _out, err = self._main(self.PIN, extra)
        self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
        self.assertIn("unexpected 0x099EF33C", err)
        with_registry = build_import_elf([("sceLibFont", FONT_LIST_NIDS), ("sceReg", [0x1D8A762E])])
        status, _out, err = self._main(self.PIN, with_registry)
        self.assertEqual(status, user_mode_imports.EXIT_KERNEL_IMPORT)
        self.assertIn("sceReg", err)

    def test_forbid_library_needs_a_library_name(self) -> None:
        with redirect_stderr(io.StringIO()):
            self.assertEqual(user_mode_imports.main(["--forbid-library"]),
                             user_mode_imports.EXIT_UNREADABLE)
            self.assertEqual(user_mode_imports.main(["--forbid-library", "--exact-library"]),
                             user_mode_imports.EXIT_UNREADABLE)

    def test_makefile_pins_the_font_list_imports_and_forbids_the_registry(self) -> None:
        root = Path(__file__).resolve().parents[1]
        fixture = root / "fixtures" / "psp_oracle"
        makefile = (fixture / "Makefile").read_text(encoding="utf-8")
        # The leading newline skips the `else ifeq` line of the case-id table.
        block = makefile.split("\nifeq ($(CASE),font-list)\n", 1)[1].split("endif", 1)[0]
        self.assertIn("OBJS += $(BUILD_DIR)/libfont_imports.o", block)
        self.assertIn("--exact-library sceLibFont=" + ",".join(f"0x{nid:08X}" for nid in FONT_LIST_NIDS),
                      block)
        self.assertIn("--forbid-library sceReg", block)
        self.assertIn("else ifeq ($(CASE),font-list)\nCASE_ID = 75\n", makefile)
        stubs = (fixture / "libfont_imports.S").read_text(encoding="utf-8")
        # The font-userpgf case (#1055) adds eight imports under #ifdef LIBFONT_USERPGF; the
        # font-list build never defines it, so the font-list block is the unconditional part.
        unconditional = stubs.split("#ifdef LIBFONT_USERPGF", 1)[0]
        declared = [int(nid, 16) for nid in re.findall(r'IMPORT_FUNC\s+"sceLibFont",\s*(0x[0-9A-Fa-f]{8})', unconditional)]
        self.assertEqual(declared, FONT_LIST_NIDS)
        self.assertEqual(stubs.count("IMPORT_START"), 1)
        probe = (fixture / "probe.c").read_text(encoding="utf-8")
        self.assertIn("#define PSP_ORACLE_CASE_FONT_LIST 75", probe)
        self.assertIn('#define PROBE_HOST0_LOG "host0:/font_list_log.txt"', probe)

    def test_makefile_pins_the_font_userpgf_imports_to_thirteen_calls(self) -> None:
        root = Path(__file__).resolve().parents[1]
        fixture = root / "fixtures" / "psp_oracle"
        makefile = (fixture / "Makefile").read_text(encoding="utf-8")
        block = makefile.split("\nifeq ($(CASE),font-userpgf)\n", 1)[1].split("endif", 1)[0]
        self.assertIn("OBJS += $(BUILD_DIR)/libfont_imports.o", block)
        self.assertIn("LIBFONT_ASFLAGS = -DLIBFONT_USERPGF", block)
        self.assertIn("--forbid-library sceReg", block)
        stubs = (fixture / "libfont_imports.S").read_text(encoding="utf-8")
        conditional = stubs.split("#ifdef LIBFONT_USERPGF", 1)[1].split("#endif", 1)[0]
        extra = [int(nid, 16) for nid in re.findall(r'IMPORT_FUNC\s+"sceLibFont",\s*(0x[0-9A-Fa-f]{8})', conditional)]
        self.assertEqual(len(extra), 8)
        self.assertIn("--exact-library sceLibFont=" + ",".join(f"0x{nid:08X}" for nid in FONT_LIST_NIDS + extra),
                      block)
        self.assertIn("else ifeq ($(CASE),font-userpgf)\nCASE_ID = 113\n", makefile)
        probe = (fixture / "probe.c").read_text(encoding="utf-8")
        self.assertIn("#define PSP_ORACLE_CASE_FONT_USERPGF 113", probe)
        self.assertIn('#define PROBE_HOST0_LOG "host0:/font_userpgf_log.txt"', probe)

    def test_font_list_header_makes_no_registry_or_file_call(self) -> None:
        root = Path(__file__).resolve().parents[1]
        header = (root / "fixtures" / "psp_oracle" / "font_list.h").read_text(encoding="utf-8")
        code = "\n".join(line for line in header.splitlines()
                         if not line.lstrip().startswith(("*", "/*", "//")))
        self.assertNotRegex(code, r"\bsceReg\w*\s*\(")
        self.assertNotRegex(code, r"\bsceIo\w*\s*\(")
        self.assertNotRegex(code, r"\bsceKernel\w*\s*\(")
        for name in ("sceFontNewLib", "sceFontGetNumFontList", "sceFontGetFontList",
                     "sceFontGetFontInfoByIndexNumber", "sceFontDoneLib"):
            self.assertIn(f"{name}(", code)


class LlscLinkProbeTests(unittest.TestCase):
    """PSP-LLSC-001: the two sc behaviours PR #812 named as unmeasured on the console.

    A second sc after a successful sc also stores, and an sc does not compare its address
    with the one ll linked. Both follow the MIPS32 operation text and the project's model
    (src/rt/guest_interp.c), and a PSP-3000 measured both on 2026-10-15 (build e45e667b):
    every sc reported 1 and stored. The probe is single-threaded register and memory state, so
    these tests pin its cells, the shape of each link window, the model words, the fixed
    record shape the parser registry expects, and the case's wiring.
    """

    CASE = "llsc-link"
    TEST_ID = "PSP-LLSC-001"
    CELLS = ("llsc-control", "llsc-second-sc", "llsc-other-address")

    @staticmethod
    def model_words(case_id: str) -> tuple[int, int]:
        """The model's (word A, word B) after the cell; A starts at 0x11111111, B at 0x44444444."""
        a_init, b_init = 0x11111111, 0x44444444
        if case_id == "llsc-control":
            return 0x22222222, b_init
        if case_id == "llsc-second-sc":
            return 0x33333333, b_init
        return a_init, 0x55555555

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.fixture = self.root / "fixtures" / "psp_oracle"
        self.probe = (self.fixture / "probe.c").read_text(encoding="utf-8")
        self.makefile = (self.fixture / "Makefile").read_text(encoding="utf-8")

    def _block(self) -> str:
        start = self.probe.index("#if PSP_ORACLE_CASE == PSP_ORACLE_CASE_LLSC_LINK\n")
        return self.probe[start:self.probe.index("\n#endif", start)]

    def _run_body(self) -> str:
        start = self.probe.index("static void run_llsc_link(int emulated) {")
        return self.probe[start:self.probe.index("\n}\n", start)]

    def _stream(self, rows: list[str]) -> str:
        return META.format(
            source="psp", model="PSP-3000", firmware="6.61-ARK",
            binary=MEASURED_SHA, commit=MEASURED_COMMIT,
        ) + "".join(rows)

    def _rows(self, statuses: dict[str, str] | None = None) -> list[str]:
        spec, _counts = CAMPAIGN_PROBE_CASES[self.CASE]
        rows = []
        for index, case_id in enumerate(spec.semantic_cases):
            status = (statuses or {}).get(case_id, "PASS" if index % 2 else "FAIL")
            outs = "".join(f" out{i}=0x{i:08x}" for i in range(6))
            rows.append(
                f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} case_id={case_id} "
                f"status={status} result=0x{index:08x}{outs}\n"
            )
        rows.append(
            f"NAKAGAWA_PSP_TEST schema=1 test_id={self.TEST_ID} case_id={spec.terminal_case} "
            f"status=PASS result=0x0 out0=0x{spec.terminal_count:x}\n"
        )
        return rows

    def test_probe_model_words_match_the_project_model(self) -> None:
        constants = {name: int(value, 16) for name, value in re.findall(
            r"^#define LLSC_LINK_(\w+) (0x[0-9a-fA-F]+)u$", self.probe, re.MULTILINE)}
        model_a = re.search(r"const uint32_t model_a\[LLSC_LINK_CELLS\] = \{\s*([^}]*)\}",
                            self.probe).group(1)
        model_b = re.search(r"const uint32_t model_b\[LLSC_LINK_CELLS\] = \{\s*([^}]*)\}",
                            self.probe).group(1)
        names = {"FIRST": constants["FIRST"], "SECOND": constants["SECOND"],
                 "A_INIT": constants["A_INIT"], "B_INIT": constants["B_INIT"],
                 "OTHER": constants["OTHER"]}
        def words(text: str) -> list[int]:
            return [names[token.strip().removeprefix("LLSC_LINK_")]
                    for token in text.split(",") if token.strip()]
        self.assertEqual(words(model_a), [self.model_words(c)[0] for c in self.CELLS])
        self.assertEqual(words(model_b), [self.model_words(c)[1] for c in self.CELLS])
        self.assertEqual(constants["A_INIT"], 0x11111111)
        self.assertEqual(constants["B_INIT"], 0x44444444)

    def test_status_says_the_window_ran_not_that_the_console_matched_the_model(self) -> None:
        # The runner accepts a capture only when every record is PASS. Only the control is
        # judged against the model (a plain pair must store, or nothing else means anything);
        # the two measured cells pass once their window ran as designed, and what their sc did
        # is read from the record's words against out4/out5.
        block = self._block()
        status = block[block.index("int pass = loaded[c] == LLSC_LINK_A_INIT;"):]
        status = status[:status.index("const uint32_t out[LLSC_LINK_OUTS]")]
        self.assertIn(
            "if (c == 0u) {\n"
            "            pass = pass && first[0] == 1u && a_after[0] == model_a[0] &&\n"
            "                   b_after[0] == model_b[0];\n"
            "        } else if (c == 1u) {\n"
            "            pass = pass && first[1] == 1u;\n"
            "        }",
            status,
        )
        self.assertNotIn("last[c]", status)
        self.assertNotIn("model_a[c]", status)
        self.assertNotIn("model_b[c]", status)

    def test_each_link_window_is_one_asm_block_of_ll_and_sc_only(self) -> None:
        block = self._block()
        windows = [body for body in re.findall(r"__asm__ volatile\((.*?)\);", block, re.DOTALL)
                   if '"ll %' in body]
        self.assertEqual(len(windows), 3)
        expected = [["ll", "sc"], ["ll", "sc", "sc"], ["ll", "sc"]]
        for body, want in zip(windows, expected, strict=True):
            template = body.split(': "', 1)[0]
            mnemonics = [m for m in re.findall(r'"(\.?[a-z]+)', template) if m != ".set"]
            with self.subTest(window=want):
                self.assertEqual(mnemonics, want)
                self.assertNotIn("syscall", template)
                self.assertNotIn("jal", template)
                self.assertNotIn("break", template)

    def test_every_window_is_bracketed_by_suspend_and_resume_with_no_host_io_inside(self) -> None:
        body = self._run_body()
        suspends = [m.start() for m in re.finditer(r"sceKernelCpuSuspendIntr\(\)", body)]
        resumes = [m.start() for m in re.finditer(r"sceKernelCpuResumeIntr\(token\)", body)]
        calls = [body.index(name) for name in (
            "llsc_window_one(&", "llsc_window_twice(&", "llsc_window_other(&")]
        self.assertEqual(len(suspends), 3)
        self.assertEqual(len(resumes), 3)
        for suspend, call, resume in zip(suspends, calls, resumes, strict=True):
            self.assertLess(suspend, call)
            self.assertLess(call, resume)
        self.assertLess(resumes[-1], body.index("emit_record_extended("))
        self.assertNotIn("for (;;)", self._block())
        self.assertNotIn("while (", self._block().split("static void run_llsc_link")[1])

    def test_each_cell_writes_its_sentinels_and_steps_before_its_window(self) -> None:
        body = self._run_body()
        window = [body.index(name) for name in (
            "llsc_window_one(&", "llsc_window_twice(&", "llsc_window_other(&")]
        for index, case_id in enumerate(self.CELLS):
            with self.subTest(cell=case_id):
                step = body.index(f'probe_step(emulated, "llsc-link", "{case_id}");')
                sentinel = body.index(f"s_llsc_mem[{index}][0] = LLSC_LINK_A_INIT;")
                suspend = body.index("sceKernelCpuSuspendIntr()", step)
                self.assertLess(step, sentinel)
                self.assertLess(sentinel, suspend)
                self.assertLess(suspend, window[index])

    def test_record_names_follow_the_registered_cell_order(self) -> None:
        self.assertRegex(
            self._run_body(),
            r'static const char \*const names\[LLSC_LINK_CELLS\] = \{\s*'
            r'"llsc-control", "llsc-second-sc", "llsc-other-address",\s*\};',
        )
        self.assertIn('emit_record_extended(emulated, "PSP-LLSC-001", "llsc-link-done", "PASS",',
                      self._run_body())

    def test_registered_spec_is_three_cells_with_six_words_each(self) -> None:
        spec, counts = CAMPAIGN_PROBE_CASES[self.CASE]
        self.assertEqual(spec.test_id, self.TEST_ID)
        self.assertEqual(spec.semantic_cases, self.CELLS)
        self.assertEqual(spec.terminal_case, "llsc-link-done")
        self.assertEqual(spec.terminal_count, 3)
        self.assertEqual({counts[case] for case in self.CELLS}, {6})
        self.assertEqual(counts[spec.terminal_case], 1)
        self.assertEqual(_campaign_completeness_contract(self.CASE), "strict-golden-sequence")

    def test_complete_stream_parses_with_every_cell_pass_fail_or_skip(self) -> None:
        report = parse_campaign_probe_output(
            self._stream(self._rows({"llsc-second-sc": "SKIP"})), self.CASE)
        self.assertTrue(report.complete)
        self.assertTrue(report.terminal_present)
        self.assertEqual(report.record_count, 4)
        self.assertEqual(report.results["llsc-second-sc"].status, "SKIP")
        self.assertEqual(report.results["llsc-control"].status, "FAIL")

    def test_missing_extra_out_of_order_and_unknown_cells_are_refused(self) -> None:
        rows = self._rows()
        duplicate = rows[:1] + [rows[0]] + rows[1:]
        swapped = [rows[1], rows[0]] + rows[2:]
        unknown = [rows[0], rows[1].replace("llsc-second-sc", "llsc-bogus")] + rows[2:]
        foreign = [rows[0].replace(self.TEST_ID, "PSP-LLSC-002")] + rows[1:]
        short_terminal = rows[:-1] + [rows[-1].replace("out0=0x3", "out0=0x2")]
        for name, body in (("duplicate", duplicate), ("swapped", swapped),
                           ("unknown", unknown), ("foreign", foreign),
                           ("short-terminal", short_terminal)):
            with self.subTest(shape=name):
                with self.assertRaises(ProtocolError):
                    parse_campaign_probe_output(self._stream(body), self.CASE)
                with self.assertRaises(ProtocolError):
                    parse_campaign_probe_output(self._stream(body), self.CASE,
                                                require_complete=False)
        missing = rows[:1] + rows[2:]
        with self.assertRaises(ProtocolError):
            parse_campaign_probe_output(self._stream(missing), self.CASE)
        self.assertFalse(parse_campaign_probe_output(
            self._stream(missing), self.CASE, require_complete=False).complete)

    def test_manifest_row_is_measured_and_cites_every_cell(self) -> None:
        manifest = json.loads((self.root / "tools" / "psp_oracle" / "manifest.json").read_text(
            encoding="utf-8"))
        row = next(test for test in manifest["tests"] if test["id"] == self.TEST_ID)
        spec, _counts = CAMPAIGN_PROBE_CASES[self.CASE]
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertEqual(row["status"], "implemented")
        self.assertEqual(row["case_ids"], list(spec.ordered_cases))
        self.assertEqual(row["evidence_cases"], row["case_ids"])
        self.assertEqual(
            row["evidence_ref"],
            "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize",
        )
        self.assertIn("e45e667b", row["measurement_note"])
        self.assertNotIn("evidence_note", row)
        # The citation must name every cell verbatim; instruction rows grant no HLE tier.
        section = hle_manifest.measured_evidence_section(row)
        self.assertIn(f"`{self.TEST_ID}`", section)

    def test_case_is_wired_into_the_makefile_queue_and_documentation(self) -> None:
        from psp_oracle.run_psplink import CAMPAIGN_CASE_ESTIMATE_SECONDS

        self.assertIn("#define PSP_ORACLE_CASE_LLSC_LINK 69", self.probe)
        self.assertIn("else ifeq ($(CASE),llsc-link)\nCASE_ID = 69\n", self.makefile)
        self.assertIn('#define PROBE_HOST0_LOG "host0:/llsc_link_log.txt"', self.probe)
        self.assertEqual(_campaign_host0_log_path(Path("host0"), self.CASE).name,
                         "llsc_link_log.txt")
        queue = list(CAMPAIGN_QUEUE_CASES)
        self.assertEqual(queue[queue.index("registry-readonly") + 1], self.CASE)
        self.assertEqual(queue[queue.index(self.CASE) + 1], "kernel-misc")
        self.assertIn(self.CASE, CAMPAIGN_CASE_ESTIMATE_SECONDS)
        oracle_doc = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        self.assertIn("| `llsc-link` | `PSP-LLSC-001` | `MEASURED` (2026-10-15, `e45e667b`)",
                      oracle_doc)
        readme = (self.fixture / "README.md").read_text(encoding="utf-8")
        self.assertIn("| `llsc-link` | `PSP-LLSC-001` |", readme)


class Record20261015Tests(unittest.TestCase):
    """The 2026-10-15 PSP-3000 runs recorded in the public tree.

    Covers the three #1037 probes (the VFPU corpus and timer-order case from 22b027f6, the
    audio SRC case from 7fc1641e), the 32-case requalification of the campaign queue on
    f2d6c91a, and the words of the 2026-10-10 ge-nan run that the index cited without
    publishing. The raw envelopes and logs are private; these tests pin what the public
    rows, index entries and notes say, and that each says no more than the cells show.
    """

    ANCHOR = "docs/HARDWARE_ORACLE.md#measured-to-date-index-exact-cells-only-do-not-generalize"
    # Campaign case -> the oracle row its campaign case measured.
    REQUALIFIED = {
        "audio-query": "PSP-AUDIO-001", "cache-alias": "PSP-CACHE-001",
        "display-mask-duty": "PSP-DISPLAY-001", "display-vblank-window": "PSP-DISPLAY-004",
        "display-wait-late": "PSP-DISPLAY-002", "display-wait-priority": "PSP-DISPLAY-003",
        "dma-cells": "PSP-DMAC-001", "fpu-cc": "PSP-FPU-CC-001", "fpu-vector": "PSP-FPU-001",
        "ge-break-continue": "PSP-GE-CONTROL-001", "ge-nan": "PSP-GE-001",
        "io-matrix": "PSP-IO-001", "kernel-alarm": "PSP-ALARM-001",
        "kernel-misc": "PSP-KERNEL-MISC-001", "llsc-link": "PSP-LLSC-001",
        "mutex-interrupt-context": "PSP-MUTEX-001", "mutex-priority-inheritance": "PSP-MUTEX-001",
        "mutex-refer-unlocked": "PSP-MUTEX-001", "mutex-timeout-quanta": "PSP-MUTEX-001",
        "refer-status-size": "PSP-KERNEL-STATUS-001", "registry-readonly": "PSP-REGISTRY-001",
        "smoke": "PSP-SMOKE-001", "thread-exit-delete": "PSP-THREAD-EXIT-001",
        "thread-scheduler": "PSP-THREAD-003", "vfpu-compare": "PSP-VFPU-CMP-001",
        "wait-outcomes": "PSP-WAIT-001",
    }
    NOT_ELIGIBLE = (
        "teardown-test", "dma-invalid-tail-s0", "dma-invalid-tail-memcpy-src",
        "dma-invalid-tail-memcpy-dst", "dma-invalid-tail-try-src", "dma-invalid-tail-try-dst",
    )

    def setUp(self) -> None:
        self.root = Path(__file__).resolve().parents[1]
        self.manifest = json.loads(
            (self.root / "tools" / "psp_oracle" / "manifest.json").read_text(encoding="utf-8"))
        self.rows = {test["id"]: test for test in self.manifest["tests"]}
        self.doc = (self.root / "docs" / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        self.readme = (self.root / "fixtures" / "psp_oracle" / "README.md").read_text(
            encoding="utf-8")

    def _entry(self, heading: str) -> str:
        """The index entry that starts with `heading`, up to the next top-level entry."""
        start = self.doc.index(heading)
        end = self.doc.index("\n- **", start + 1)
        return self.doc[start:end]

    @staticmethod
    def _paragraph_blocks(text: str) -> list[str]:
        return [
            " ".join(line.strip() for line in block.splitlines() if line.strip())
            for block in re.split(r"\n\s*\n", text)
            if block.strip()
        ]

    @staticmethod
    def _table_rows(text: str) -> list[tuple[str, ...]]:
        rows = []
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped.startswith("|") or not stripped.endswith("|"):
                continue
            cells = tuple(cell.strip().replace("`", "") for cell in stripped.strip("|").split("|"))
            if all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells):
                continue
            rows.append(cells)
        return rows

    def assertRuleStated(self, text: str, terms: tuple[str, ...], rule: str) -> None:
        matches = [
            block for block in self._paragraph_blocks(text)
            if all(term.lower() in block.lower() for term in terms)
        ]
        self.assertTrue(matches, f"the oracle docs no longer state {rule}: no block has all of {terms}")

    def test_the_three_new_rows_cite_a_section_that_names_every_cell(self) -> None:
        for test_id in ("PSP-VFPU-TRANSCENDENTAL-001", "PSP-HLE-TIMER-ORDER-001",
                        "PSP-HLE-AUDIO-SRC-001"):
            with self.subTest(test_id=test_id):
                row = self.rows[test_id]
                self.assertEqual(row["hardware_evidence"], "MEASURED")
                self.assertEqual(row["evidence_ref"], self.ANCHOR)
                self.assertEqual(row["evidence_cases"][:len(row["case_ids"])], row["case_ids"])
                self.assertNotIn("evidence_note", row)
                hle_manifest.measured_evidence_section(row)

    def test_the_audio_src_row_cites_the_re_run_and_not_the_captured_first_run(self) -> None:
        row = self.rows["PSP-HLE-AUDIO-SRC-001"]
        note = row["measurement_note"]
        self.assertIn("7fc1641e", note)
        self.assertRuleStated(
            note,
            ("acceptance-eligible", "0 blockers"),
            "the rerun's acceptance eligibility and blocker count",
        )
        self.assertRuleStated(
            note,
            ("not acceptance-eligible", "not cited"),
            "the first run's non-eligible and uncited status",
        )
        for word in ("0x80260008", "0x80268002", "0x400", "0x80000104", "0x80000003"):
            self.assertIn(word, note)
            self.assertIn(word, self.doc)
        self.assertRuleStated(
            self.doc,
            ("16 records", "PASS", "2 SKIP"),
            "the captured-only run's record and skip counts",
        )
        for case_id in ("src-final-cleanup", "out2-final-cleanup"):
            with self.subTest(case_id=case_id):
                self.assertIn(f"`{case_id}`", self.doc)

    def test_the_timer_order_row_claims_no_tie_break_and_no_absolute_delay(self) -> None:
        row = self.rows["PSP-HLE-TIMER-ORDER-001"]
        note = row["measurement_note"]
        for word in ("17,125", "53,561", "13,579", "0x8002019F"):
            self.assertIn(word, note)
        self.assertRuleStated(
            note,
            ("tie-break", "equal due time", "not"),
            "the absence of an equal-due-time tie-break measurement",
        )
        self.assertRuleStated(
            note,
            ("no absolute delay", "cited"),
            "the absence of an absolute-delay claim",
        )
        self.assertRuleStated(
            self.doc,
            ("two alarms", "same due time", "not", "ordered"),
            "the equal-due-time limitation in the published interpretation",
        )
        self.assertFalse(
            any(
                "equal due times" in block.lower() and "run in set order" in block.lower()
                for block in self._paragraph_blocks(self.doc)
            ),
            "the documentation must not turn the measured sequence into a tie-break rule",
        )

    def test_the_ge_nan_words_are_published_without_a_raster_rule(self) -> None:
        row = self.rows["PSP-GE-001"]
        self.assertEqual(row["hardware_evidence"], "MEASURED")
        self.assertIn("f2d6c91a", row["measurement_note"])
        self.assertIn("do not establish a rasterization rule", row["measurement_note"])
        entry = self._entry("- **PSP-3000 requalification of the 32-case campaign queue")
        expected_rows = {
            ("0x7FC00000 (qNaN)", "0x7F800001", "0x7F800001"),
            ("0x7F800000 (+Inf)", "0x7F800000", "0x7F800000"),
            ("0xFF800000 (-Inf)", "0xFF800000", "0xFF800000"),
            ("0x80000000 (-0)", "0x00000000", "0x80000000"),
            ("0x00000001 (minimum positive denormal)", "0x00000000", "0x00000000"),
            ("0x7FC00000 (qNaN)", "0x2B6647A7", "0x9DBEB5C5", "0x9DBEB5C5"),
            ("0xFF800000 (-Inf)", "0xA1F002BD", "0x9DBEB5C5", "0x9DBEB5C5"),
            ("0x80000000 (-0)", "0xA929B835", "0x9DBEB5C5", "0x9DBEB5C5"),
        }
        self.assertTrue(expected_rows.issubset(set(self._table_rows(entry))))
        self.assertRuleStated(
            entry,
            ("hash", "whole scanned frame", "nothing about which pixels differ"),
            "the limitation on what the framebuffer hash establishes",
        )
        self.assertRuleStated(
            entry,
            ("no rasterization rule", "NOT_MEASURED"),
            "the unmeasured rasterization verdict",
        )

    def test_the_requalification_run_is_a_newer_citation_for_every_eligible_case(self) -> None:
        entry = self._entry("- **PSP-3000 requalification of the 32-case campaign queue")
        self.assertRuleStated(
            entry,
            ("26 case envelopes", "acceptance-eligible"),
            "the number of acceptance-eligible campaign envelopes",
        )
        table_rows = self._table_rows(entry)
        eligible_rows = {
            (row[0], row[1]) for row in table_rows if len(row) >= 3 and row[0] != "Campaign case"
        }
        for case, test_id in self.REQUALIFIED.items():
            with self.subTest(case=case):
                self.assertIn((case, test_id), eligible_rows)
                row = self.rows[test_id]
                self.assertEqual(row["hardware_evidence"], "MEASURED")
                self.assertIn("f2d6c91a", row["measurement_note"])
        for case in self.NOT_ELIGIBLE:
            with self.subTest(case=case):
                self.assertIn(case, entry)
                self.assertNotIn(case, {row[0] for row in table_rows if row})
        # Captured-only rows stay captured: the requalification run adds no measured cell.
        self.assertEqual(self.rows["PSP-TEARDOWN-001"]["hardware_evidence"], "CAPTURED")
        self.assertIn("f2d6c91a", self.rows["PSP-TEARDOWN-001"]["evidence_note"])
        self.assertIn("f2d6c91a", self.rows["PSP-DMAC-001"]["measurement_note"])
        self.assertEqual(len(self.REQUALIFIED) + len(self.NOT_ELIGIBLE), 32)

    def test_readme_and_index_agree_on_the_status_of_the_new_probes(self) -> None:
        rows = self._table_rows(self.readme)
        by_case = {row[0]: row for row in rows if len(row) >= 3}
        audio = by_case["hle-audio-src"]
        self.assertEqual(audio[1], "PSP-HLE-AUDIO-SRC-001")
        self.assertTrue(all(term in audio[2] for term in ("MEASURED", "2026-10-15", "7fc1641e")))
        for case, evidence_id, commit in (
            ("hle-timer-order", "PSP-HLE-TIMER-ORDER-001", "22b027f6"),
            ("vfpu-transcendental-corpus", "PSP-VFPU-TRANSCENDENTAL-001", "22b027f6"),
        ):
            with self.subTest(case=case):
                row = by_case[case]
                self.assertEqual(row[1], evidence_id)
                self.assertTrue(all(term in row[2] for term in ("MEASURED", "2026-10-15", commit)))

    def test_the_audio_src_and_timer_comparisons_are_recorded_and_pinned(self) -> None:
        for name in ("test_audio_src_matches_the_measured_contract",
                     "test_alarm_delivery_order_matches_the_timer_order_probe"):
            self.assertIn(f"`{name}`", self.doc)
        self.assertRuleStated(
            self.doc,
            ("runtime", "no SRC handlers", "before this entry"),
            "the SRC handler introduction point",
        )
        self.assertRuleStated(
            self.doc,
            ("VTimer handler cells", "do not match"),
            "the VTimer comparison's current mismatch status",
        )
        selftest = (self.root / "src" / "rt" / "hle_thread_selftest.c").read_text(
            encoding="utf-8")
        for name in ("test_audio_src_matches_the_measured_contract",
                     "test_alarm_delivery_order_matches_the_timer_order_probe"):
            self.assertEqual(selftest.count(f"static void {name}(void)"), 1)
            self.assertEqual(selftest.count(f"    {name}();"), 1)
        hle = (self.root / "src" / "rt" / "hle.c").read_text(encoding="utf-8")
        for nid, name in ((0x38553111, "sceAudioSRCChReserve"), (0x5c37c0ae, "sceAudioSRCChRelease"),
                          (0xe0727056, "sceAudioSRCOutputBlocking")):
            self.assertIn(f'sr_hle_register(0x{nid:08x}, "{name}", h_', hle)

    def test_the_transcendental_comparison_is_recorded_and_vrexp2_is_pinned(self) -> None:
        self.assertRuleStated(
            self.doc,
            ("ten operations", "matched", "all 63 inputs"),
            "the number of matching transcendental operations and inputs",
        )
        self.assertRuleStated(
            self.doc,
            ("vrexp2.s", "none"),
            "the measured vrexp2 mismatch",
        )
        self.assertRuleStated(
            self.doc,
            ("both paths", "exp2(-x)"),
            "the runtime correction applied to both execution paths",
        )
        self.assertRuleStated(
            self.doc,
            ("five more negated forms", "unmeasured"),
            "the unmeasured negated forms",
        )
        selftest = (self.root / "src" / "rt" / "vfpu_interp_selftest.c").read_text(
            encoding="utf-8")
        start = selftest.index("static int check_vrexp2_measured_words(void)")
        body = selftest[start:selftest.index("\nint main(void)", start)]
        pairs = re.findall(r"\{0x([0-9a-f]{8})u, 0x([0-9a-f]{8})u\}", body)
        self.assertEqual(len(pairs), 63)
        # exp2(-x): 1.0 for zero (the first special), 2.0 for -1.0 and just under 0.5 for +1.0.
        words = {int(a, 16): int(b, 16) for a, b in pairs}
        self.assertEqual(words[0x00000000], 0x3F800000)
        self.assertEqual(words[0xBF800000], 0x40000000)
        self.assertEqual(words[0x3F800000], 0x3EFFFFFC)
        interp = (self.root / "src" / "rt" / "vfpu_interp.c").read_text(encoding="utf-8")
        self.assertIn("case 28: d[i] = sr_vfpu_exp2(-v[i]); break;", interp)
        self.assertNotIn("-sr_vfpu_exp2(v[i])", interp)
