#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate the deterministic Nakagawa VFPU compatibility census."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections import Counter, defaultdict
from functools import lru_cache
from pathlib import Path
from typing import Iterable

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import codegen
from vfpu_synth_gen import generate_memory_cop2_corpus, generate_synthetic_corpus

CENSUS_SCHEMA = 1
TRACKING_ISSUE = 326
ACCELERATED_FORMS: tuple[str, ...] = ()
INTERPRETER_KINDS = {0: "unsupported", 1: "compute", 2: "state"}
MEMORY_OPS = {0x32, 0x35, 0x36, 0x3A, 0x3D, 0x3E}
VECTOR_SIZE_OPS = {0x18, 0x19, 0x1B, 0x34, 0x3C}
RETURN_PATTERN = re.compile(r"return\s+(SR_VFPU_(?:OTHER|COMPUTE|STATE));")
PAYLOAD_VALUES = tuple(
    sorted(
        {
            (high << 8) | low
            for high in (0x0000, 0x0101, 0x3F7F, 0x8000, 0xFFFF)
            for low in (0x00, 0x01, 0x0F, 0x7F, 0x8F, 0x90, 0xFF)
        }
    )
)


class CensusError(RuntimeError):
    pass


class CensusAgreementError(CensusError):
    pass


def _canonical_json(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


@lru_cache(maxsize=1)
def production_vfpu_opcodes() -> tuple[tuple[int, ...], tuple[int, ...]]:
    routed: set[int] = set()
    direct_controls: set[int] = set()
    original = codegen.vfpu_effect
    for op in range(64):
        called = False

        def traced(addr, word, lle_cpu=False, delay_branch_pc=None):
            nonlocal called
            called = True
            return original(addr, word, lle_cpu=lle_cpu, delay_branch_pc=delay_branch_pc)

        codegen.vfpu_effect = traced
        try:
            try:
                body, _, _ = codegen.effect(0x1000, op << 26)
            except codegen.Unsupported:
                body = ""
        finally:
            codegen.vfpu_effect = original
        if called:
            routed.add(op)
        elif "s->vfpuCtrl[0]=0xe4u" in body and "s->vfpuCtrl[1]=0xe4u" in body:
            direct_controls.add(op)
    if not routed:
        raise CensusError("production effect() routed no VFPU opcodes to vfpu_effect()")
    return tuple(sorted(routed)), tuple(sorted(direct_controls))


def systematic_words() -> tuple[int, ...]:
    routed, controls = production_vfpu_opcodes()
    words: set[int] = set()
    for op in routed:
        for selector21 in range(32):
            for selector16 in range(32):
                words.add((op << 26) | (selector21 << 21) | (selector16 << 16))
                for sample in range(4):
                    size_code = (selector21 + selector16 * 3 + sample) & 3
                    payload = PAYLOAD_VALUES[
                        (selector21 * 17 + selector16 * 11 + sample * 23) % len(PAYLOAD_VALUES)
                    ]
                    word = (op << 26) | (selector21 << 21) | (selector16 << 16) | payload
                    word &= ~((1 << 7) | (1 << 15))
                    word |= ((size_code >> 1) & 1) << 15
                    word |= (size_code & 1) << 7
                    words.add(word)
    for op in controls:
        words.update({op << 26, 0xFFFFFFFF})
    words.update(generate_synthetic_corpus())
    words.update(generate_memory_cop2_corpus())
    return tuple(sorted(words))


def _transform_interpreter_source() -> tuple[str, int]:
    source_path = ROOT / "src" / "rt" / "vfpu_interp.c"
    source = source_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    transformed, count = RETURN_PATTERN.subn(r"return census_record_return(\1, __LINE__);", source)
    if count == 0:
        raise CensusError("production VFPU interpreter has no instrumentable return sites")
    return transformed, count


def _probe_support_source() -> str:
    return r'''#include "recomp.h"
#include <stdlib.h>

uint8_t *g_mem;
uint32_t g_sr_debug = 0;
int g_sr_metadata_watch = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_heap_watch = 0;
int g_hle_depth = 0;
CpuState *s_cpu = NULL;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_count = 0;
unsigned g_sr_store_context_limit = 0;
int g_sr_store_context_mem_gpr = 0;
unsigned g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
unsigned g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
int g_sr_last_writer_enabled = 0;

void sr_note_mem_write(uint32_t address, uint32_t width, uint32_t value, uint32_t pc) {
    (void)address;
    (void)width;
    (void)value;
    (void)pc;
}

void sr_heap_note_write(uint32_t address, uint32_t width, uint32_t value, uint32_t pc) {
    (void)address;
    (void)width;
    (void)value;
    (void)pc;
}

uint32_t sched_current_uid(void) { return 0u; }
uint32_t sr_get_ge_status(void) { return 0u; }

int census_probe_memory_init(void) {
    /* Whole 32 MiB user-RAM window: SR_HOST() maps guest addresses without bounds checks, so
     * signed +/-32 KiB VFPU load/store offsets must never leave the allocation. */
    g_mem = (uint8_t *)calloc(1, 0x02000000u);
    return g_mem != NULL;
}

void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) {
    (void)s;
    (void)idx;
    (void)prefix;
    for (int i = 0; i < n; i++) r[i] = (float)(i + 1);
}

void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t prefix) {
    (void)s;
    (void)idx;
    (void)d;
    (void)n;
    (void)prefix;
}

float sr_vfpu_rcp(float x) { return x; }
float sr_vfpu_rsqrt(float x) { return x; }
float sr_vfpu_sqrt(float x) { return x; }
float sr_vfpu_asin(float x) { return x; }
float sr_vfpu_log2(float x) { return x; }
float sr_vfpu_sin(float x) { return x; }
float sr_vfpu_cos(float x) { return x; }
float sr_vfpu_exp2(float x) { return x; }

int sr_cpu_lle_enabled(void) { return 0; }
unsigned sr_cpu_data_access_fault(const CpuState *s, uint32_t address, unsigned width, int store) {
    (void)s;
    (void)address;
    (void)width;
    (void)store;
    return 0u;
}
int sr_cpu_raise_data_fault(CpuState *s, unsigned code, uint32_t address, uint32_t pc) {
    (void)s;
    (void)code;
    (void)address;
    (void)pc;
    return -1;
}
void sr_oor(uint32_t address, uint32_t value, int store) {
    (void)address;
    (void)value;
    (void)store;
}
'''


def _probe_main_source() -> str:
    return r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"

static int census_return_kind;
static int census_return_line;

static int census_record_return(int kind, int line) {
    census_return_kind = kind;
    census_return_line = line;
    return kind;
}

int census_probe_memory_init(void);
#line 1 "src/rt/vfpu_interp.c"
#include "vfpu_interp_census.c"
#undef vfpu_interp_census_line

int main(void) {
    CpuState s;
    char text[32];
    if (!census_probe_memory_init()) return 3;
    while (fgets(text, sizeof(text), stdin) != NULL) {
        unsigned long parsed = 0;
        if (sscanf(text, "%lx", &parsed) != 1 || parsed > 0xffffffffUL) return 4;
        memset(&s, 0, sizeof(s));
        for (int i = 0; i < 32; i++) s.r[i] = 0x08100000u;  /* +/-32 KiB offsets stay in RAM */
        s.vfpuCtrl[0] = 0xe4u;
        s.vfpuCtrl[1] = 0xe4u;
        s.vfpuCtrl[2] = 0u;
        census_return_kind = -1;
        census_return_line = -1;
        int kind = sr_vfpu_interp(&s, (uint32_t)parsed);
        if (kind != census_return_kind) return 5;
        printf("%08lx,%d,%d\n", parsed, kind, census_return_line);
    }
    return 0;
}
'''


def _compiler_command() -> list[str]:
    configured = os.environ.get("CC", "").strip()
    candidates = []
    if configured:
        candidates.append(shlex.split(configured, posix=os.name != "nt"))
    candidates.extend([[name] for name in ("gcc", "cc", "clang")])
    for candidate in candidates:
        if not candidate:
            continue
        executable = shutil.which(candidate[0])
        if executable:
            return [executable, *candidate[1:]]
    raise CensusError("a host C compiler is required to drive the production VFPU interpreter")


_PROBE_CACHE: dict[tuple[tuple[int, ...], str, tuple[str, ...]], dict[int, tuple[int, int]]] = {}


def _run_interpreter_probe(words: tuple[int, ...]) -> dict[int, tuple[int, int]]:
    transformed, return_sites = _transform_interpreter_source()
    source_hash = hashlib.sha256(transformed.encode("utf-8")).hexdigest()
    compiler = _compiler_command()
    key = (words, source_hash, tuple(compiler))
    if key in _PROBE_CACHE:
        return _PROBE_CACHE[key]
    with tempfile.TemporaryDirectory(prefix="nakagawa_vfpu_census_") as tmp:
        root = Path(tmp)
        interpreter_path = root / "vfpu_interp_census.c"
        main_path = root / "probe.c"
        support_path = root / "support.c"
        executable = root / ("probe.exe" if os.name == "nt" else "probe")
        interpreter_path.write_text(transformed, encoding="utf-8", newline="\n")
        main_path.write_text(_probe_main_source(), encoding="ascii", newline="\n")
        support_path.write_text(_probe_support_source(), encoding="ascii", newline="\n")
        command = [
            *compiler,
            "-std=c11",
            "-O0",
            f"-I{ROOT / 'src' / 'rt'}",
            str(main_path),
            str(support_path),
            "-lm",
            "-o",
            str(executable),
        ]
        compiled = subprocess.run(
            command,
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if compiled.returncode != 0:
            detail = (compiled.stderr or compiled.stdout).strip()
            raise CensusError(f"production VFPU decoder probe failed to compile: {detail}")
        payload = "\n".join(f"{word:08x}" for word in words) + "\n"
        ran = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            input=payload,
            capture_output=True,
            text=True,
            timeout=120,
        )
        if ran.returncode != 0:
            completed = ran.stdout.splitlines()
            last_word = completed[-1].split(",", 1)[0] if completed else "none"
            raise CensusError(
                f"production VFPU decoder probe failed with status {ran.returncode} after {last_word}"
            )
    results: dict[int, tuple[int, int]] = {}
    for line in ran.stdout.splitlines():
        fields = line.split(",")
        if len(fields) != 3:
            raise CensusError("production VFPU decoder probe emitted malformed output")
        word = int(fields[0], 16)
        kind = int(fields[1], 10)
        return_line = int(fields[2], 10)
        if word not in words or kind not in INTERPRETER_KINDS or return_line <= 0:
            raise CensusError(
                f"production VFPU decoder probe emitted invalid word={word:08x} kind={kind} line={return_line}"
            )
        results[word] = (kind, return_line)
    if len(results) != len(words) or return_sites <= 0:
        raise CensusError("production VFPU decoder probe did not classify every visited encoding")
    _PROBE_CACHE[key] = results
    return results


def _reason_template(exc: Exception) -> str:
    reason = str(exc).split(" at 0x", 1)[0]
    return re.sub(r"\b(size|register) \d+\b", r"\1 {n}", reason)


def _reason_kind(reason: str) -> str:
    if reason.startswith("vcrs size") or reason.startswith("vqmul size") or "vcmov imm3" in reason:
        return "operand-size-selector"
    if reason.startswith("cop2 sub"):
        return "operand-register" if re.search(r"sub (?:3|7) or register", reason) else "encoding"
    if "register" in reason:
        return "operand-register"
    if "prefix" in reason:
        return "operand-prefix"
    return "encoding"


def _aot_structure(body: str) -> str:
    normalized = body
    prefix_names = (
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
    )
    for index, name in enumerate(prefix_names):
        normalized = normalized.replace(f"vfpuCtrl[{index}]", f"vfpuCtrl[prefix{name}]")
    number = r"(?:0[xX][0-9a-fA-F]+|(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?)[fFlLuU]*"
    normalized = re.sub(number, "#", normalized)
    return " ".join(normalized.split())


def _classify_aot(word: int) -> dict[str, object]:
    routed, controls = production_vfpu_opcodes()
    op = word >> 26
    if op in controls:
        body, _, _ = codegen.effect(0x1000, word)
        return {
            "aot_disposition": "aot-direct",
            "aot_source": "effect-direct-control",
            "aot_reason": "",
            "aot_emitted": body,
        }
    if op not in routed:
        return {
            "aot_disposition": "not-vfpu-decoder-opcode",
            "aot_source": None,
            "aot_reason": "opcode is not routed to the production VFPU decoder",
            "aot_emitted": "",
        }
    try:
        body, _, _ = codegen.vfpu_effect(0x1000, word)
    except codegen.Unsupported as exc:
        reason = _reason_template(exc)
        return {
            "aot_disposition": "aot-unsupported",
            "aot_source": "vfpu_effect",
            "aot_reason": reason,
            "aot_reason_kind": _reason_kind(reason),
            "aot_emitted": codegen.normal_line(0x1000, word),
        }
    return {
        "aot_disposition": "aot-routes-to-interpreter" if "sr_vfpu_interp" in body else "aot-direct",
        "aot_source": "vfpu_effect",
        "aot_reason": "",
        "aot_emitted": body,
    }


def classify_words(words: Iterable[int]) -> list[dict[str, object]]:
    ordered = tuple(sorted(set(words)))
    probe = _run_interpreter_probe(ordered)
    records: list[dict[str, object]] = []
    for word in ordered:
        record = _classify_aot(word)
        record["aot_structure"] = _aot_structure(str(record["aot_emitted"]))
        kind, return_line = probe[word]
        record.update(
            {
                "word": word,
                "opcode": word >> 26,
                "vector_size": (
                    codegen.vec_size(word) if (word >> 26) in VECTOR_SIZE_OPS else None
                ),
                "interpreter_supported": kind != 0,
                "interpreter_kind": INTERPRETER_KINDS[kind],
                "interpreter_return_line": return_line,
            }
        )
        records.append(record)
    return records


def agreement_status(record: dict[str, object]) -> str:
    aot = record["aot_disposition"]
    supported = bool(record["interpreter_supported"])
    if record["aot_source"] == "effect-direct-control":
        return "explicit-aot-direct-control"
    if aot == "aot-direct" and supported:
        return "agree-supported-direct"
    if aot == "aot-routes-to-interpreter" and supported:
        return "agree-supported-routed"
    if aot == "aot-unsupported" and not supported:
        return "agree-unsupported"
    return "disagreement"


def compatibility_disposition(record: dict[str, object]) -> str:
    aot = record["aot_disposition"]
    supported = bool(record["interpreter_supported"])
    if record["aot_source"] == "effect-direct-control":
        return "aot-direct"
    if aot == "aot-direct" and supported:
        return "aot-direct"
    if aot == "aot-routes-to-interpreter" and supported:
        return "aot-routes-to-sr-vfpu-interp"
    if aot == "aot-unsupported" and supported:
        return "interpreter-only"
    if aot == "aot-unsupported" and record.get("aot_reason_kind") != "encoding":
        return "supported-opcode-form-unmodeled"
    return "unsupported-encoding"


def validate_agreement(records: Iterable[dict[str, object]]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    violations: list[str] = []
    for record in records:
        status = agreement_status(record)
        counts[status] += 1
        if status == "disagreement":
            violations.append(
                f"0x{record['word']:08x}: aot={record['aot_disposition']} "
                f"interpreter={record['interpreter_kind']}"
            )
        emitted = str(record.get("aot_emitted", ""))
        needs_fallback = record["aot_disposition"] in {
            "aot-routes-to-interpreter",
            "aot-unsupported",
        }
        if record["aot_source"] == "vfpu_effect" and needs_fallback:
            required = (
                "sr_vfpu_interp",
                "SR_VFPU_OTHER",
                "VFPU_UNSUPPORTED",
                "sr_unimplemented",
                "issue=326",
                "return",
            )
            if any(token not in emitted for token in required):
                violations.append(f"0x{record['word']:08x}: generated fallback is not fail-closed")
    if violations:
        raise CensusAgreementError("; ".join(violations[:10]))
    return dict(sorted(counts.items()))


def _differential_tests(record: dict[str, object], synthetic_words: set[int]) -> list[str]:
    word = int(record["word"])
    tests: list[str] = []
    if word in synthetic_words:
        tests.extend(
            [
                "tools.test_vfpu_synth_corpus.CategoryDistinguishingTests.test_positive_corpus_has_no_fallback",
                "make vfpu_fuzz",
            ]
        )
    if record["opcode"] in {0x35, 0x3D} and record["interpreter_supported"]:
        tests.append("vfpu-interp-selftest:check_quad_memops")
    return sorted(set(tests))


def _hardware_evidence(record: dict[str, object], vector_size: int | None) -> list[dict[str, str]]:
    word = int(record["word"])
    op = int(record["opcode"])
    evidence: list[dict[str, str]] = []
    measured_triple = vector_size == 3
    memory_ids = {
        0x32: ("PSP-A3-08",),
        0x36: ("PSP-A3-09",),
        0x3A: ("PSP-A3-12",),
        0x3E: ("PSP-A3-10", "PSP-A3-13"),
    }
    if op in memory_ids:
        evidence.extend(
            {
                "oracle_id": oracle_id,
                "kind": "alignment",
                "coverage": "measured load/store alignment and exception behavior",
            }
            for oracle_id in memory_ids[op]
        )
    if op == 0x35 and not (word & 2):
        evidence.append(
            {
                "oracle_id": "PSP-A3-15",
                "kind": "alignment",
                "coverage": "measured left-quad odd-address completion",
            }
        )
    sub3 = (word >> 23) & 7
    sub5 = (word >> 21) & 0x1F
    which = (word >> 16) & 0xF
    if measured_triple and op == 0x19 and sub3 in {1, 4}:
        evidence.append(
            {
                "oracle_id": "issue-40-group-a",
                "kind": "semantic",
                "coverage": "measured vdot/vhdp NaN and infinity cells",
            }
        )
    group_b_form = (
        op == 0x19 and sub3 in {1, 2, 4, 5}
    ) or (
        op == 0x3C and (
            sub3 == 4
            or (sub3 == 7 and sub5 == 28 and which in {1, 2, 4, 5})
        )
    )
    if measured_triple and group_b_form:
        evidence.append(
            {
                "oracle_id": "issue-40-group-b",
                "kind": "semantic",
                "coverage": "measured vmscl and named source/destination overlap cells only",
            }
        )
    if measured_triple and op == 0x3C and sub3 in {0, 3}:
        evidence.append(
            {
                "oracle_id": "issue-40-vmmul-vtfm3-nan",
                "kind": "semantic",
                "coverage": "measured vmmul.t and vtfm3.t NaN cells only",
            }
        )
    immediate_vfpu = op == 0x37 and ((word >> 24) & 3) == 3
    uses_vector_addressing = op in VECTOR_SIZE_OPS or op == 0x12 or immediate_vfpu
    if uses_vector_addressing:
        scalar_addressing = op == 0x12 or immediate_vfpu or vector_size == 1
        coverage = (
            "all 128 scalar register encodings measured"
            if scalar_addressing
            else "14 of 512 wide register encodings measured; remainder is derived consistency only"
        )
        evidence.append({"oracle_id": "hardware_vfpu_addr_001", "kind": "addressing", "coverage": coverage})
    return evidence


def _evidence_tier(disposition: str, differential: list[str], hardware: list[dict[str, str]]) -> str:
    if disposition in {"unsupported-encoding", "supported-opcode-form-unmodeled", "interpreter-only"}:
        return "U"
    if any(item["kind"] == "semantic" for item in hardware):
        return "H"
    if hardware:
        return "HP"
    if differential:
        return "D"
    return "S"


def acceleration_verified(row: dict[str, object]) -> bool:
    supported = row["compatibility_disposition"] in {
        "aot-direct",
        "aot-routes-to-sr-vfpu-interp",
    }
    hardware_sensitive = bool(row["hardware_sensitive"])
    has_differential = bool(row["differential_tests"])
    has_required_hardware = not hardware_sensitive or row["evidence_tier"] == "H"
    return supported and has_differential and has_required_hardware


def _make_rows(records: list[dict[str, object]]) -> list[dict[str, object]]:
    synthetic_words = set(generate_synthetic_corpus()) | set(generate_memory_cop2_corpus())
    grouped: dict[tuple[object, ...], list[dict[str, object]]] = defaultdict(list)
    for record in records:
        memory_variant = record["word"] & 3 if record["opcode"] in MEMORY_OPS else 0
        key = (
            record["opcode"],
            record["aot_disposition"],
            record["aot_structure"],
            record["aot_reason"],
            record["interpreter_kind"],
            record["vector_size"],
            record["interpreter_return_line"],
            memory_variant,
        )
        grouped[key].append(record)
    rows: list[dict[str, object]] = []
    for key, group in grouped.items():
        op, _, _, reason, _, vector_size, return_line, _ = key
        dispositions = {compatibility_disposition(record) for record in group}
        agreements = {agreement_status(record) for record in group}
        if len(dispositions) != 1 or len(agreements) != 1:
            raise CensusError(f"inconsistent census group for opcode 0x{op:02x}")
        disposition = next(iter(dispositions))
        representative = min(int(record["word"]) for record in group)
        identity = (
            op,
            key[1],
            key[2],
            reason,
            key[4],
            vector_size,
            key[7],
        )
        form_hash = hashlib.sha256(
            json.dumps(identity, sort_keys=True, ensure_ascii=True).encode("ascii")
        ).hexdigest()
        selector_word = representative & 0xFFFFFF00
        form_id = f"vfpu-{op:02x}-{selector_word:08x}-{form_hash[:12]}"
        differential = sorted(
            {
                test
                for record in group
                for test in _differential_tests(record, synthetic_words)
            }
        )
        size_classes = (
            [("S", "P", "T", "Q")[int(vector_size) - 1]]
            if vector_size is not None
            else []
        )
        hardware = _hardware_evidence(group[0], vector_size)
        agreement = next(iter(agreements))
        hardware_sensitive = (
            agreement != "explicit-aot-direct-control"
            and disposition not in {"unsupported-encoding", "supported-opcode-form-unmodeled"}
        )
        evidence_tier = _evidence_tier(disposition, differential, hardware)
        row: dict[str, object] = {
            "form_id": form_id,
            "opcode": f"0x{op:02x}",
            "form_sha256": form_hash,
            "canonical_word": f"0x{representative:08x}",
            "selector_bits_25_8": f"0x{selector_word:08x}",
            "vector_size": vector_size,
            "representative_words": [
                f"0x{word:08x}" for word in sorted({int(item['word']) for item in group})[:4]
            ],
            "visited_encodings": len(group),
            "size_classes": sorted(size_classes),
            "aot_disposition": sorted({str(record["aot_disposition"]) for record in group}),
            "interpreter_disposition": sorted({str(record["interpreter_kind"]) for record in group}),
            "compatibility_disposition": disposition,
            "decoder_branch": f"src/rt/vfpu_interp.c:{return_line}",
            "aot_reason": sorted({str(record["aot_reason"]) for record in group if record["aot_reason"]}),
            "agreement": agreement,
            "boundary_issue": TRACKING_ISSUE if disposition in {
                "unsupported-encoding",
                "supported-opcode-form-unmodeled",
                "interpreter-only",
            } else None,
            "differential_tests": differential,
            "hardware_measured": hardware,
            "hardware_sensitive": hardware_sensitive,
            "evidence_tier": evidence_tier,
            "sufficiently_verified_for_acceleration": False,
        }
        row["sufficiently_verified_for_acceleration"] = acceleration_verified(row)
        if reason and row["boundary_issue"] is None:
            raise CensusError(f"supported census row retained unsupported reason {reason}")
        rows.append(row)
    rows.sort(key=lambda row: str(row["form_id"]))
    if len({row["form_id"] for row in rows}) != len(rows):
        raise CensusError("duplicate VFPU census form id")
    return rows


def build_census() -> dict[str, object]:
    words = systematic_words()
    records = classify_words(words)
    agreement = validate_agreement(records)
    rows = _make_rows(records)
    known_form_ids = {str(row["form_id"]) for row in rows}
    unknown_accelerated = sorted(set(ACCELERATED_FORMS) - known_form_ids)
    if unknown_accelerated:
        raise CensusAgreementError(
            "SIMD registry names unknown forms: " + ", ".join(unknown_accelerated)
        )
    accelerated = [row["form_id"] for row in rows if row["form_id"] in ACCELERATED_FORMS]
    acceleration_violations = [
        form_id for form_id in accelerated if not next(row for row in rows if row["form_id"] == form_id)["sufficiently_verified_for_acceleration"]
    ]
    if acceleration_violations:
        raise CensusAgreementError(
            "SIMD forms lack required evidence: " + ", ".join(acceleration_violations)
        )
    disposition_counts = Counter(str(row["compatibility_disposition"]) for row in rows)
    tier_counts = Counter(str(row["evidence_tier"]) for row in rows)
    by_opcode: dict[str, Counter[str]] = defaultdict(Counter)
    for row in rows:
        by_opcode[str(row["opcode"])][str(row["compatibility_disposition"])] += 1
    source_paths = (
        "tools/codegen.py",
        "src/rt/vfpu_interp.c",
        "src/rt/vfpu_interp_selftest.c",
        "src/rt/vfpu_fuzz.c",
        "tools/test_vfpu_synth_corpus.py",
        "tools/vfpu_fuzz_gen.py",
        "tools/vfpu_synth_gen.py",
        "tools/psp_oracle/manifest.json",
        "docs/HARDWARE_ORACLE.md",
    )
    return {
        "schema": CENSUS_SCHEMA,
        "tracking_issue": TRACKING_ISSUE,
        "sources": [
            {"path": path, "sha256": _sha256(ROOT / path)}
            for path in source_paths
        ],
        "systematic_space": {
            "method": "orthogonal selector21 x selector16 covering samples plus the public differential corpora",
            "opcode_families": [f"0x{op:02x}" for op in production_vfpu_opcodes()[0]],
            "explicit_aot_control_opcodes": [f"0x{op:02x}" for op in production_vfpu_opcodes()[1]],
            "selector21_values": 32,
            "selector16_values": 32,
            "size_codes": 4,
            "payload_patterns": len(PAYLOAD_VALUES),
            "words_visited": len(words),
            "production_decoder_return_sites": _transform_interpreter_source()[1],
        },
        "summary": {
            "rows": len(rows),
            "by_disposition": dict(sorted(disposition_counts.items())),
            "by_evidence_tier": dict(sorted(tier_counts.items())),
            "by_opcode": {
                op: dict(sorted(counts.items())) for op, counts in sorted(by_opcode.items())
            },
        },
        "hardware_oracle_boundary": {
            "formal_manifest_vfpu_ids": [],
            "documented_evidence_ids": [
                "hardware_vfpu_addr_001",
                "issue-40-group-a",
                "issue-40-group-b",
                "issue-40-vmmul-vtfm3-nan",
                "PSP-A3-08",
                "PSP-A3-09",
                "PSP-A3-10",
                "PSP-A3-11",
                "PSP-A3-12",
                "PSP-A3-13",
                "PSP-A3-14",
                "PSP-A3-15",
            ],
            "context_only_not_inherited": ["PSP-A3-11", "PSP-A3-14"],
            "rule": "documented cells are row-level evidence; context-only cells are not inherited; the formal PSP oracle manifest currently has no VFPU id",
        },
        "agreement_gate": {
            "status": "pass",
            "rule": "production vfpu_effect and sr_vfpu_interp support dispositions must agree; semantic equivalence remains differential-test evidence",
            "counts": agreement,
        },
        "acceleration_gate": {
            "rule": "differential coverage is mandatory; hardware-sensitive forms also require evidence tier H",
            "all_supported_forms_are_hardware_sensitive": True,
            "accelerated_forms": list(ACCELERATED_FORMS),
            "violations": acceleration_violations,
            "sufficiently_verified_forms": [
                str(row["form_id"])
                for row in rows
                if row["sufficiently_verified_for_acceleration"]
            ],
        },
        "encounter_input_schema": {
            "media_type": "application/json",
            "shape": [{"word": "0xVVVVVVVV", "count": 1}],
            "allowed_keys": ["word", "count"],
            "rejected_context": ["address", "pc", "mnemonic", "title", "retail metadata"],
        },
        "rows": rows,
    }


def render_markdown(census: dict[str, object]) -> str:
    summary = census["summary"]
    dispositions = summary["by_disposition"]
    total_rows = summary["rows"]
    lines = [
        "# VFPU Compatibility Census",
        "",
        "Generated from production `codegen.vfpu_effect()` and `sr_vfpu_interp()` behavior.",
        f"The systematic gate visited {census['systematic_space']['words_visited']} encodings and produced {total_rows} rows.",
        "",
        "| Disposition | Rows | Product boundary |",
        "| --- | ---: | --- |",
    ]
    boundary = {
        "aot-direct": "translated directly by AOT",
        "aot-routes-to-sr-vfpu-interp": "explicit interpreter route",
        "interpreter-only": "in the works — tracking issue #326",
        "supported-opcode-form-unmodeled": "in the works — tracking issue #326",
        "unsupported-encoding": "in the works — tracking issue #326",
    }
    for name in (
        "aot-direct",
        "aot-routes-to-sr-vfpu-interp",
        "interpreter-only",
        "supported-opcode-form-unmodeled",
        "unsupported-encoding",
    ):
        lines.append(f"| `{name}` | {dispositions.get(name, 0)} | {boundary[name]} |")
    lines.extend(
        [
            "",
            "AOT/interpreter support-disposition agreement is a fail-closed gate. Unknown or unmodeled forms stop through the named issue #326 unimplemented boundary before the next guest instruction; semantic equivalence remains separate differential evidence.",
            "The AOT-owned `vflush` control is listed explicitly and is not a fallback eligibility claim.",
            "",
            "## Evidence",
            "",
            "Differential coverage proves AOT/interpreter agreement, not PSP hardware equivalence. Hardware IDs and partial coverage are recorded per JSON row.",
            f"Evidence tiers: `{json.dumps(summary['by_evidence_tier'], sort_keys=True)}`.",
            "",
            "## #292 acceleration gate",
            "",
        ]
    )
    if census["acceleration_gate"]["accelerated_forms"]:
        lines.append("Accelerated forms: " + ", ".join(census["acceleration_gate"]["accelerated_forms"]))
    else:
        lines.append("No SIMD path exists; the accelerated-form set is empty. New acceleration remains in the works under #292 and must pass this census gate.")
    lines.extend(
        [
            "",
            "## Second-title hook",
            "",
            "`--encodings FILE` accepts only a JSON list of `{word, count}` objects. Addresses, PCs, mnemonics, titles, and retail metadata are rejected.",
            "",
        ]
    )
    return "\n".join(lines)


def _load_encounter_words(path: Path) -> list[tuple[int, int]]:
    try:
        data = json.loads(path.read_text(encoding="ascii"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise CensusError("encounter input must be an ASCII JSON list of word/count objects") from exc
    if not isinstance(data, list):
        raise CensusError("encounter input must be a JSON list")
    counts: Counter[int] = Counter()
    for item in data:
        if not isinstance(item, dict) or set(item) != {"word", "count"}:
            raise CensusError("each encounter entry must contain exactly word and count")
        word = item["word"]
        count = item["count"]
        if not isinstance(word, str) or re.fullmatch(r"0x[0-9A-Fa-f]{8}", word) is None:
            raise CensusError("encounter word must be an eight-digit 0x-prefixed string")
        if isinstance(count, bool) or not isinstance(count, int) or count <= 0:
            raise CensusError("encounter count must be a positive integer")
        counts[int(word, 16)] += count
    if not counts:
        raise CensusError("encounter input must contain at least one word")
    return sorted(counts.items())


def build_encounter_report(path: Path) -> dict[str, object]:
    counted = _load_encounter_words(path)
    records = classify_words(word for word, _ in counted)
    if any(record["aot_source"] is None for record in records):
        raise CensusError("encounter input contains a word outside the production VFPU decoders")
    validate_agreement(records)
    synthetic_words = set(generate_synthetic_corpus()) | set(generate_memory_cop2_corpus())
    count_by_word = dict(counted)
    entries = []
    for record in records:
        disposition = compatibility_disposition(record)
        entries.append(
            {
                "word": f"0x{record['word']:08x}",
                "count": count_by_word[int(record["word"])],
                "disposition": disposition,
                "aot": record["aot_disposition"],
                "interpreter": record["interpreter_kind"],
                "reason": record["aot_reason"],
                "tracking_issue": TRACKING_ISSUE if disposition in {
                    "interpreter-only",
                    "supported-opcode-form-unmodeled",
                    "unsupported-encoding",
                } else None,
                "differential_tests": _differential_tests(record, synthetic_words),
            }
        )
    return {
        "schema": CENSUS_SCHEMA,
        "total_words": len(entries),
        "total_count": sum(count_by_word.values()),
        "entries": entries,
    }


def text_report() -> str:
    return render_markdown(build_census())


def json_report() -> str:
    return _canonical_json(build_census())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", nargs="?", const=ROOT / "build" / "vfpu_census.json", type=Path)
    parser.add_argument("--markdown", nargs="?", const=ROOT / "build" / "vfpu_census.md", type=Path)
    parser.add_argument("--format", choices=("text", "json"))
    parser.add_argument("--encodings", type=Path)
    args = parser.parse_args(argv)
    if args.encodings is not None and (args.json is not None or args.markdown is not None):
        parser.error("--encodings cannot be combined with census output paths")
    try:
        if args.encodings is not None:
            sys.stdout.write(_canonical_json(build_encounter_report(args.encodings)))
            return 0
        census = None
        if args.json is not None or args.markdown is not None:
            census = build_census()
        if args.json is not None:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(_canonical_json(census), encoding="ascii", newline="\n")
        if args.markdown is not None:
            args.markdown.parent.mkdir(parents=True, exist_ok=True)
            args.markdown.write_text(render_markdown(census), encoding="utf-8", newline="\n")
        if args.format == "json":
            sys.stdout.write(_canonical_json(census if census is not None else build_census()))
        elif args.format == "text" or (args.json is None and args.markdown is None):
            sys.stdout.write(render_markdown(census if census is not None else build_census()) + "\n")
    except CensusError as exc:
        sys.stderr.write(f"vfpu_coverage_report: {exc}\n")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
