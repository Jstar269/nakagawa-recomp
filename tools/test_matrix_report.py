# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the psp-recomp authors

"""Generate a conservative, machine-readable unittest evidence matrix.

The report deliberately labels semantic fields as heuristic unless a test's
source makes the evidence layer obvious.  It is an inventory aid, not a claim
that AST/source inspection proves the public behavior named by a test.
"""

from __future__ import annotations

import argparse
import ast
from collections import Counter
import importlib
import json
from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
_ISSUE_RE = re.compile(r"(?<![A-Za-z])#(\d{1,4})\b")
_PRIVATE_RE = re.compile(r"place_game_here|EBOOT\.elf|\.iso\b|memstick|PSPDEV|PSPLINK|VULKAN_SDK", re.I)
_SOURCE_RE = re.compile(
    r"read_text\(|read_bytes\(|assert(?:Not)?(?:In|Regex)\(|source[-_ ]shape|source\s*=|read_file",
    re.I,
)
_STRUCTURAL_MODULES = {
    "test_ci_paths",
    "test_ci_required",
    "test_compat_manifest",
    "test_hst_manager_manifest",
    "test_hst_title_manifest",
    "test_pspdev_lock",
    "test_pspdev_probe",
    "test_public_ci_wiring",
    "test_publish_audit",
    "test_release_manifest",
    "test_shader_embed",
    "test_title_codegen_plan",
    "test_title_codegen_synthetic",
    "test_title_manager_plan",
    "test_title_manifest",
    "test_vfpu_provenance",
    "test_vfpu_synth_corpus",
    "test_vfpu_trace_bits",
}
# `disposition` previously restated `source_shape_classification`: every
# classified case became "UNKNOWN", which reads as an unreviewed backlog even
# though those cases are precisely the ones that HAVE been categorised. The
# mapping below carries the review outcome instead. It never emits a delete
# verdict on its own: categories C and E are the deletion boundary and remain
# empty, and each label still requires source inspection per
# docs/TEST_SHAPE_CLASSIFICATION.md.
_DISPOSITION_BY_SHAPE = {
    "NOT_APPLICABLE": "KEEP",
    "A_LEGITIMATE_STRUCTURAL_INVARIANT": "KEEP",
    "B_HISTORICAL_BEHAVIORAL_PROXY": "RETIRE_WITH_BEHAVIORAL_SEAM",
    "C_REDUNDANT_BEHAVIORAL_PROXY": "DELETE_CANDIDATE",
    "D_ONLY_AVAILABLE_EVIDENCE": "KEEP_UNTIL_EXECUTABLE_SEAM",
    "E_OBSOLETE_INVARIANT": "DELETE_CANDIDATE",
}

_PROXY_MODULES = {
    "test_callback_correctness",
    "test_hle_umd_wakeup",
    "test_manager_symbol_docs",
    "test_progress_tracker",
}
_ONLY_AVAILABLE_MODULES = {
    "test_codegen_continuations",
    "test_codegen_no_shadow_stubs",
    "test_codegen_retail_allocator",
    "test_sdkver_c",
}
# These modules mix executable/compile checks with source-shape assertions.
# Their case-level evidence stays conservative below, but the file-level
# review inventory must not omit them merely because one subprocess call makes
# the module integration-heavy.  This explicit set accounts for the ten mixed
# files in the original 51-file source-shape inventory.
_MIXED_SOURCE_SHAPE_MODULE_CATEGORIES = {
    "test_asset_index_c": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_build_truth": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_codegen_entry_semantics": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_codegen_gate_b_encoding": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_codegen_madd_msub": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_dispatch_c": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_evf_c": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_guest_printf": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_hst_doctor": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
    "test_hst_doctor_hardening": "A_LEGITIMATE_STRUCTURAL_INVARIANT",
}


def _flatten(suite: unittest.TestSuite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from _flatten(item)
        else:
            yield item


def _subsystem(module: str) -> str:
    if any(token in module for token in ("import", "prx", "pspsdk", "manifest")):
        return "imports-prx-manifests"
    if any(token in module for token in ("codegen", "analyze", "elf", "title_codegen")):
        return "codegen-analyzer"
    if "pgd" in module:
        return "pgd"
    if any(token in module for token in ("hle", "callback", "sched", "evf", "guestmem", "dispatch")):
        return "hle-evidence-scheduler"
    if "vfpu" in module:
        return "vfpu"
    if any(token in module for token in ("hst", "manager", "build", "doctor", "vulkan")):
        return "manager-build-security"
    return "other"


def _phrase(name: str) -> str:
    return name.removeprefix("test_").replace("_", " ")


def _source_file(module_name: str, module) -> str:
    source = Path(getattr(module, "__file__", "")).resolve()
    try:
        return source.relative_to(ROOT).as_posix()
    except ValueError:
        return f"tools/{module_name}.py"


def _class_source(module, class_name: str) -> str:
    path = Path(getattr(module, "__file__", ""))
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, SyntaxError):
        return ""
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            return ast.get_source_segment(path.read_text(encoding="utf-8"), node) or ""
    return ""


def _source_shape_classification(module_name: str, text: str, evidence: str) -> str:
    if evidence not in {"source-shape", "manifest/structural", "security tripwire"}:
        return "NOT_APPLICABLE"
    if module_name in _PROXY_MODULES:
        return "B_HISTORICAL_BEHAVIORAL_PROXY"
    if module_name in _ONLY_AVAILABLE_MODULES:
        return "D_ONLY_AVAILABLE_EVIDENCE"
    if module_name in _STRUCTURAL_MODULES or any(
        token in text.lower() for token in ("spdx", "provenance", "forbidden", "manifest contract")
    ):
        return "A_LEGITIMATE_STRUCTURAL_INVARIANT"
    return "D_ONLY_AVAILABLE_EVIDENCE"


def _evidence(module_name: str, text: str) -> str:
    lower = text.lower()
    if module_name in {"test_publish_audit", "test_key_scrub_tools"} or "forbidden publication" in lower:
        return "security tripwire"
    if module_name in _STRUCTURAL_MODULES or "manifest" in module_name or "provenance" in module_name:
        return "manifest/structural"
    if _SOURCE_RE.search(text) and not any(token in lower for token in ("subprocess.run", "check_output", "compile")):
        return "source-shape"
    if "subprocess.run" in lower or "check_output" in lower or "ctypes" in lower:
        return "integration"
    return "pure functional/unit"


def _fixture(text: str, skip_reason: str | None) -> str:
    if _PRIVATE_RE.search(text):
        return "private local/environment-dependent"
    if skip_reason or any(token in text.lower() for token in ("sanitizer", "addresssanitizer", "mipsel-linux")):
        return "environment-dependent"
    return "source-owned synthetic"


def build_report() -> dict[str, object]:
    loader = unittest.TestLoader()
    suite = loader.discover("tools", pattern="test_*.py")
    cases = []
    modules: dict[str, object] = {}
    for case in _flatten(suite):
        module_name = case.__class__.__module__
        module = modules.setdefault(module_name, importlib.import_module(module_name))
        source_file = _source_file(module_name, module)
        text = Path(module.__file__).read_text(encoding="utf-8")
        method_name = getattr(case, "_testMethodName", "")
        class_name = case.__class__.__name__
        method = getattr(case, method_name, None)
        skip_reason = getattr(method, "__unittest_skip_why__", None) or getattr(
            case.__class__, "__unittest_skip_why__", None
        )
        evidence = _evidence(module_name, text)
        source_shape = _source_shape_classification(module_name, text, evidence)
        issues = sorted({int(value) for value in _ISSUE_RE.findall(text)})
        cases.append(
            {
                "id": case.id(),
                "source_file": source_file,
                "module": module_name,
                "class": class_name,
                "method": method_name,
                "subsystem": _subsystem(module_name),
                "primary_invariant": _phrase(method_name),
                "secondary_invariants": [],
                "related_issues": [f"#{issue}" for issue in issues],
                "evidence_type": evidence,
                "fixture_type": _fixture(text, skip_reason),
                "skip_condition": skip_reason,
                "platform_condition": skip_reason or None,
                "sanitizer_condition": "sanitizer" if "sanitizer" in text.lower() else None,
                "expected_failure_mode": "reject/raise" if "assertRaises" in text else "success/observable result",
                "canonical_subsystem_owner": source_file,
                "known_historical_bug_or_regression": None,
                "stronger_coverage_exists": None,
                "duplicate_similarity_candidates": [],
                "source_shape_classification": source_shape,
                "disposition": _DISPOSITION_BY_SHAPE[source_shape],
                "metadata_quality": "heuristic; inspect source before semantic decisions",
            }
        )
    cases.sort(key=lambda item: item["id"])
    source_shape_counts = Counter(
        item["source_shape_classification"]
        for item in cases
        if item["source_shape_classification"] != "NOT_APPLICABLE"
    )
    source_shape_files: dict[str, set[str]] = {}
    for item in cases:
        category = item["source_shape_classification"]
        if category != "NOT_APPLICABLE":
            source_shape_files.setdefault(item["source_file"], set()).add(category)
    # File-level review is intentionally broader than case-level evidence:
    # mixed modules can contain real executable checks alongside structural
    # assertions.  Keep those cases' evidence labels unchanged while retaining
    # every reviewed file in the inventory.
    for module_name, category in _MIXED_SOURCE_SHAPE_MODULE_CATEGORIES.items():
        source_shape_files.setdefault(f"tools/{module_name}.py", set()).add(category)
    source_shape_file_counts = Counter(
        category
        for categories in source_shape_files.values()
        for category in categories
    )
    return {
        "schema": 1,
        "generated_by": "tools/test_matrix_report.py",
        "discovery_contract": "python -m unittest discover -s tools -p 'test_*.py'",
        "repository_relative": True,
        "case_count": len(cases),
        "class_count": len({(case["module"], case["class"]) for case in cases}),
        "module_count": len({case["module"] for case in cases}),
        "semantic_vectors": {
            "status": "not mechanically inferable",
            "case_id_lower_bound": len(cases),
            "subtest_sites": sum("subTest" in Path(ROOT / case["source_file"]).read_text(encoding="utf-8") for case in cases),
        },
        "source_shape_classification_counts": dict(sorted(source_shape_counts.items())),
        "source_shape_file_count": len(source_shape_files),
        "source_shape_file_classification_counts": dict(sorted(source_shape_file_counts.items())),
        "source_shape_file_review_note": (
            "51-file review set includes ten explicit mixed source/behavior modules; "
            "case-level evidence remains heuristic and is not promoted by this file-level inventory"
        ),
        "source_shape_file_categories": {
            path: sorted(categories)
            for path, categories in sorted(source_shape_files.items())
        },
        "cases": cases,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    report = json.dumps(build_report(), indent=2, sort_keys=True) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(report, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
