#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Public CI gate for import coverage and fake-success regressions (issue #71).

Runs entirely on public inputs (src/rt/hle.c plus synthetic in-memory ELF
fixtures); no game file is read and the gate must never require one. It fails
when:

  1. the HLE registration manifest cannot be extracted fail-closed
     (unaccounted registration forms, duplicate NIDs, unknown handlers, or
     stale curated metadata);
  2. an alias-consistency or NID-mislabel finding is not covered by an
     acknowledged waiver in tools/hle_registry_meta.py, or a waiver has gone
     stale (its finding no longer reproduces);
  3. the manifest classification drifts from the committed baseline
     (tools/import_audit_baseline.json) in any way -- a downgrade from
     dedicated/controlled handling to fake_success/missing is called out
     explicitly; any other drift requires a reviewed baseline refresh via
     `python tools/hle_manifest.py --write-baseline`;
  4. a malformed synthetic fixture is not rejected with a clean
     ImportTableError, or the well-formed mixed fixture does not classify
     exactly as expected;
  5. the classifier report is not byte-deterministic.

Deliberately NOT a failure: a private title importing missing/unsupported
functions. Private audits are a separate local run (docs/IMPORT_AUDIT.md)
whose reports stay untracked.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from hle_manifest import (  # noqa: E402
    DEFAULT_BASELINE,
    ManifestError,
    build_manifest,
    manifest_to_baseline,
    unwaived_and_stale,
)
from import_audit import classify_imports, render_text  # noqa: E402
from import_fixtures import (  # noqa: E402
    INTERLEAVED_NIDS,
    INTERLEAVED_SHAPE,
    MIXED_FIXTURE_LIBS,
    build_import_elf,
    build_interleaved_import_elf,
)
from psp_import_table import (  # noqa: E402
    UNATTRIBUTED_LIBRARY,
    ImportTableError,
    parse_import_table,
)

DOWNGRADE_FROM = {"dedicated", "controlled_unsupported"}
DOWNGRADE_TO = {"fake_success", "missing"}

MALFORMED_FIXTURES = (
    "truncated_file",
    "zero_entry_size",
    "entry_overrun",
    "entry_header_truncated",
    "wrapped_nid_table",
    "wrapped_stub_area",
    "bad_name_ptr",
    "unterminated_name",
    "null_nid_table",
    "nid_table_partially_backed",
    "stub_table_partially_backed",
    "stub_area_unmapped",
    "stub_area_misaligned",
    "stub_range_reversed",
    "stubend_past_segment",
    "sectionless_bad_paddr",
)

# What the mixed public fixture must classify to, per (lib, nid).
MIXED_EXPECTED = {
    ("ThreadManForUser", 0x446D8DE6): "dedicated",           # sceKernelCreateThread
    ("ThreadManForUser", 0x349D6D6C): "dedicated",           # sceKernelCheckCallback
    ("scePsmfPlayer", 0x46F61F8B): "controlled_unsupported",  # scePsmfPlayerGetVideoData
    ("SynthLibA", 0x00C0FFEE): "missing",
    ("SynthLibA", 0x0BADF00D): "missing",
    ("SynthLibB", 0x0BADF00D): "missing",
}


def fail(msg: str) -> None:
    print(f"import_audit_gate: FAIL: {msg}", file=sys.stderr)
    raise SystemExit(1)


def check_findings_vs_waivers(findings: list[dict]) -> None:
    unwaived, stale = unwaived_and_stale(findings)
    if unwaived:
        lines = [
            f"  0x{f['nid']:08x} {f['name']} [{f['finding']}] handler {f['handler']}: {f['why']}"
            for f in unwaived
        ]
        fail(
            "unwaived registration findings (fix the registration, or add a waiver "
            "with an issue link in tools/hle_registry_meta.py):\n" + "\n".join(lines)
        )
    if stale:
        lines = [f"  0x{w['nid']:08x} [{w['finding']}] handler {w['handler']}" for w in stale]
        fail(
            "stale waivers -- the defect no longer reproduces, so retire the waiver "
            "in tools/hle_registry_meta.py:\n" + "\n".join(lines)
        )


def check_baseline(manifest: dict) -> None:
    if not DEFAULT_BASELINE.exists():
        fail(f"missing committed baseline {DEFAULT_BASELINE}; run tools/hle_manifest.py --write-baseline")
    baseline = json.loads(DEFAULT_BASELINE.read_text(encoding="ascii"))
    current = manifest_to_baseline(manifest)

    downgrades = []
    for nid, old in baseline.items():
        new = current.get(nid)
        new_class = new["classification"] if new else "missing"
        if old["classification"] in DOWNGRADE_FROM and new_class in DOWNGRADE_TO:
            downgrades.append(f"  {nid} {old['name']}: {old['classification']} -> {new_class}")
    if downgrades:
        fail(
            "import-coverage regression: previously handled NIDs degraded to "
            "fake-success/missing:\n" + "\n".join(downgrades)
        )
    if current != baseline:
        changed = sorted(
            set(k for k in baseline if baseline.get(k) != current.get(k))
            | set(k for k in current if baseline.get(k) != current.get(k))
        )
        fail(
            f"manifest drifted from committed baseline ({len(changed)} NIDs, e.g. {changed[:5]}); "
            "review the change and refresh with `python tools/hle_manifest.py --write-baseline`"
        )


def check_fixtures(manifest: dict) -> None:
    for corrupt in MALFORMED_FIXTURES:
        blob = build_import_elf(MIXED_FIXTURE_LIBS, corrupt=corrupt)
        try:
            parse_import_table(blob)
        except ImportTableError:
            continue
        except Exception as e:  # noqa: BLE001 -- any other escape is exactly the failure mode under test
            fail(f"malformed fixture {corrupt!r} escaped with {type(e).__name__}: {e}")
        fail(f"malformed fixture {corrupt!r} parsed without an error")

    # The stripped-PRX location path must yield the identical import set.
    sectioned = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS))
    sectionless = parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS, sectionless=True))
    if sectioned.funcs != sectionless.funcs:
        fail("sectionless (phdr[0].p_paddr) parse disagrees with the sectioned parse")

    table = sectioned
    got = {}
    report = classify_imports(table.funcs, manifest)
    for row in report["imports"]:
        got[(row["lib"], int(row["nid"], 16))] = row["classification"]
    if got != MIXED_EXPECTED:
        fail(f"mixed fixture classification mismatch:\n  got      {sorted(got.items())}\n  expected {sorted(MIXED_EXPECTED.items())}")
    dupes = report["cross_library_duplicate_nids"]
    if dupes != [{"nid": "0x0badf00d", "libs": ["SynthLibA", "SynthLibB"]}]:
        fail(f"cross-library duplicate detection mismatch: {dupes}")

    # The interleaved stub-table shape (overlapping window runs plus trailing
    # slots no window claims) must pair stub slots with NIDs globally: every
    # sectioned position is emitted, the uncovered ones stay visible as
    # UNATTRIBUTED_LIBRARY, and the parse must be deterministic. This is the
    # 35-of-51 regression: the old per-window span walk dropped the 16 slots
    # outside every window run.
    interleaved = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
    if len(interleaved.funcs) != len(INTERLEAVED_NIDS):
        fail(
            f"interleaved fixture yielded {len(interleaved.funcs)} positions; "
            f"expected {len(INTERLEAVED_NIDS)} from the global pairing"
        )
    if sum(1 for f in interleaved.funcs if f.library == UNATTRIBUTED_LIBRARY) != 16:
        fail("interleaved fixture must surface 16 unattributed slots, not drop them")
    interleaved_again = parse_import_table(build_interleaved_import_elf(INTERLEAVED_SHAPE, INTERLEAVED_NIDS))
    if (
        interleaved.funcs != interleaved_again.funcs
        or interleaved.findings != interleaved_again.findings
    ):
        fail("interleaved fixture parse is not deterministic")
    for sectionless in (False, True):
        blob = build_interleaved_import_elf(
            INTERLEAVED_SHAPE, INTERLEAVED_NIDS,
            sectionless=sectionless, corrupt="nid_region_mismatch",
        )
        try:
            parse_import_table(blob)
        except ImportTableError:
            continue
        except Exception as e:  # noqa: BLE001
            fail(f"interleaved nid_region_mismatch (sectionless={sectionless}) escaped with {type(e).__name__}: {e}")
        fail(f"interleaved nid_region_mismatch (sectionless={sectionless}) parsed without an error")

    again = classify_imports(parse_import_table(build_import_elf(MIXED_FIXTURE_LIBS)).funcs, manifest)
    blob_a = json.dumps(report, indent=2, sort_keys=True)
    blob_b = json.dumps(again, indent=2, sort_keys=True)
    if blob_a != blob_b or render_text(report) != render_text(again):
        fail("classifier report is not deterministic across runs")


def main() -> int:
    try:
        manifest = build_manifest()
    except ManifestError as e:
        fail(str(e))
    findings = [
        {**f, "nid": int(f["nid"], 16)} for f in manifest["findings"]
    ]
    check_findings_vs_waivers(findings)
    check_baseline(manifest)
    check_fixtures(manifest)
    regs = manifest["registrations"]
    counts = {}
    for r in regs:
        counts[r["classification"]] = counts.get(r["classification"], 0) + 1
    print(
        "import_audit_gate: PASS -- "
        f"{len(regs)} registrations ({', '.join(f'{k} {v}' for k, v in sorted(counts.items()))}), "
        f"{len(findings)} waived findings, {len(MALFORMED_FIXTURES)} malformed fixtures rejected cleanly"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
