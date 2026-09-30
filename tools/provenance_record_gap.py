#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Inventory the tracked paths that have no exact trusted provenance record.

Adding or editing an implementation-bearing path needs an exact,
implementation-grade record in the detailed development ledger.  That ledger
lives in a private repository: a contributor cannot create a record, and a
maintainer has to admit paths one batch at a time.  The result is that editing
an ordinary, already-merged file -- a CI workflow, a pinned dependency lock --
fails the merge-path gate with ``TRUSTED_PATH_MISSING``, which reads as "your
change is wrong" when in fact the only remedy is a maintainer admission.

This tool answers the batch question: which tracked paths would fail that way
today?  It classifies with the gate's own code, never a second copy of it:

* ``provenance_ledger.admission_requires_implementation`` decides whether a
  path needs implementation-grade authority at all;
* ``provenance_attest_verify._backing`` reports how strongly the authority
  speaks about a path -- ``exact``, ``deterministic``, ``blanket`` or ``none``;
* the publication policy decides which tracked paths are on the public surface.

The default source of truth is the committed *public* ledger, which discloses a
``record_id`` only for a path an exact trusted record already covers.  That is
the same answer the gate derives, read from a file that is in the repository, so
the inventory can be produced by anyone -- including the maintainer -- without
the private input ever entering a contributor's hands.  Pass
``--trusted-ledger`` to ask the private authority directly instead; the two
must agree, and ``--check`` fails closed when they do not.

This tool never edits a ledger, a policy, or any other control.  It prints an
inventory; a human decides what to admit.  For an upstream-derived production
gap, the inventory also reports whether the maintained independence campaign
names an owner and an explicit disposition.  That roadmap annotation is
planning evidence only; it never authorizes a provenance admission.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from . import provenance_attest_verify as verifier  # noqa: E402
    from . import provenance_ledger  # noqa: E402
    from .publication_policy import load_policy  # noqa: E402
except ImportError:  # pragma: no cover - direct script execution
    import provenance_attest_verify as verifier  # noqa: E402
    import provenance_ledger  # noqa: E402
    from publication_policy import load_policy  # noqa: E402

LEDGER_PATH = "assets/public_provenance_ledger.json"
POLICY_PATH = "assets/public_source_profile.json"
INDEPENDENCE_PLAN_PATH = "docs/INDEPENDENCE_CAMPAIGN.md"

_CAMPAIGN_INVENTORY_ROW = re.compile(r"^\|\s*(G\d+)\s*\|\s*(.*?)\s*\|")
_CAMPAIGN_TARGET = re.compile(r"^\s*-\s+\*\*(G\d+)\b")
_CODE_SPAN = re.compile(r"`([^`]+)`")
_PRODUCTION_PREFIXES = ("src/", "tools/")


def _campaign_groups(plan_text: str) -> dict[str, tuple[str, ...]]:
    """Return the source paths named by each independence campaign group.

    The campaign is deliberately prose, but its inventory table already uses
    stable group IDs and code spans for the paths it owns.  Keeping this small
    parser here makes a newly reported path fail closed when it is absent from
    that maintained inventory instead of silently treating the record gap as
    an owned independence item.
    """
    groups: dict[str, tuple[str, ...]] = {}
    for line in plan_text.splitlines():
        match = _CAMPAIGN_INVENTORY_ROW.match(line)
        if match is None:
            continue
        paths = tuple(
            span for span in _CODE_SPAN.findall(match.group(2))
            if span.startswith(_PRODUCTION_PREFIXES)
        )
        if paths:
            groups[match.group(1)] = paths
    return groups


def _campaign_targets(plan_text: str) -> dict[str, str]:
    """Return the explicit disposition text for each campaign group."""
    lines = plan_text.splitlines()
    targets: dict[str, str] = {}
    starts = [
        (index, match.group(1))
        for index, line in enumerate(lines)
        if (match := _CAMPAIGN_TARGET.match(line)) is not None
    ]
    for position, (start, group) in enumerate(starts):
        stop = starts[position + 1][0] if position + 1 < len(starts) else len(lines)
        block = " ".join(lines[start:stop])
        if "bounded-(c)" in block or re.search(r"\(c\)", block):
            targets[group] = "bounded-exclusion"
        elif re.search(r"\(a\)\s*\+\s*\(a\)", block):
            targets[group] = "clean-room-rewrite"
        elif re.search(r"\(a\)", block):
            targets[group] = "clean-room-rewrite"
        elif re.search(r"\(b\)", block):
            targets[group] = "LLE-replacement"
        elif "evidence, not rewrite" in block:
            targets[group] = "evidence-hardening"
    return targets


def independence_metadata(
    path: str,
    *,
    campaign_path: Path = ROOT / INDEPENDENCE_PLAN_PATH,
) -> dict[str, object] | None:
    """Find the maintained owner/disposition for a production path.

    A group ID is the campaign owner for this inventory slice.  ``None`` is a
    deliberate fail-closed result: a new upstream-derived production path must
    first be named in the campaign inventory and given an explicit disposition
    (including a bounded exclusion) before it can be treated as planned work.
    """
    try:
        plan_text = campaign_path.read_text(encoding="utf-8")
    except OSError:
        return None
    groups = _campaign_groups(plan_text)
    targets = _campaign_targets(plan_text)
    for group, patterns in groups.items():
        if not any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns):
            continue
        disposition = targets.get(group)
        if disposition is None:
            return None
        return {
            "status": "mapped",
            "owner": group,
            "disposition": disposition,
            "source": INDEPENDENCE_PLAN_PATH,
        }
    return None


def tracked_paths(repo: Path = ROOT) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=repo, capture_output=True, check=True,
    )
    return sorted(
        item.decode("utf-8", errors="surrogateescape")
        for item in result.stdout.split(b"\0")
        if item
    )


def exact_paths_from_public_ledger(ledger_path: Path) -> set[str]:
    """Paths the committed public ledger already attributes to a record.

    The public ledger names a ``record_id`` only where the trusted authority
    supplies an exact, implementation-grade record for that path; every other
    entry carries a deterministic statement about what the file *is*.  A path
    is therefore "covered" exactly when its public entry is content-gated.
    """
    document = json.loads(ledger_path.read_text(encoding="utf-8"))
    covered: set[str] = set()
    for entry in document.get("entries", []):
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            continue
        classification, record_id = verifier._claim(entry)
        if classification in verifier.IMPLEMENTATION_CLASSES and record_id:
            covered.add(entry["path"])
    return covered


def public_classifications(ledger_path: Path) -> dict[str, str]:
    """Return the public ledger's path classifications for gap diagnostics."""
    document = json.loads(ledger_path.read_text(encoding="utf-8"))
    return {
        entry["path"]: entry["classification"]
        for entry in document.get("entries", [])
        if isinstance(entry, dict)
        and isinstance(entry.get("path"), str)
        and isinstance(entry.get("classification"), str)
    }


def exact_paths_from_trusted_ledger(trusted_ledger: Path) -> set[str]:
    """Ask the private authority directly (maintainer-side, optional)."""
    exact, _patterns, _ids = verifier.load_trusted_records(trusted_ledger.read_bytes())
    return {
        path for path, record in exact.items()
        if provenance_ledger.class_for(path, record)[0] in verifier.IMPLEMENTATION_CLASSES
    }


def record_gaps(
    *,
    repo: Path = ROOT,
    covered: set[str] | None = None,
) -> list[dict[str, str]]:
    """Return the tracked public paths that no exact trusted record covers.

    ``covered`` is the set of paths the authority speaks about exactly.  When it
    is ``None`` the committed public ledger is used.
    """
    if covered is None:
        covered = exact_paths_from_public_ledger(repo / LEDGER_PATH)
    classifications = public_classifications(repo / LEDGER_PATH)
    campaign_path = repo / INDEPENDENCE_PLAN_PATH
    policy = load_policy(repo / POLICY_PATH)
    gaps: list[dict[str, object]] = []
    for path in tracked_paths(repo):
        if policy.resolve(path).disposition != "included":
            continue
        if not provenance_ledger.admission_requires_implementation(path):
            continue
        if path in covered:
            continue
        classification = classifications.get(path)
        if classification is None:
            classification, _evidence = provenance_ledger.class_for(path, None)
        gap: dict[str, object] = {
            "path": path,
            "reason": "no exact trusted record",
            "deterministic_class": classification,
        }
        if path.startswith(_PRODUCTION_PREFIXES) and classification in {
            "upstream_derived",
            "unresolved",
        }:
            metadata = independence_metadata(path, campaign_path=campaign_path)
            gap["independence"] = metadata or {
                "status": "missing",
                "reason": "no explicit campaign owner or disposition",
                "source": INDEPENDENCE_PLAN_PATH,
            }
        gaps.append(gap)
    return gaps


#: Known upstream-derived production paths with no explicit roadmap disposition
#: in docs/INDEPENDENCE_CAMPAIGN.md, grouped by owner issue.  This baseline is a
#: strict ratchet: a listed gap is permitted only while it remains unresolved and
#: must be removed once the campaign gives it a disposition.  Any unlisted gap
#: fails closed, and so does any stale entry.
_KNOWN_DISPOSITION_GAP_GROUPS: tuple[tuple[str, str, tuple[str, ...]], ...] = (
    (
        "#355",
        "decryption primitives (AES, Kirk, KLE, PRX, SHA-1) have no independence campaign group yet",
        (
            "src/core/nk_psp_aes.c",
            "src/core/nk_psp_aes.h",
            "src/core/nk_psp_kirk.c",
            "src/core/nk_psp_kirk.h",
            "src/core/nk_psp_kle.c",
            "src/core/nk_psp_kle.h",
            "src/core/nk_psp_prx.c",
            "src/core/nk_psp_prx.h",
            "src/core/nk_psp_sha1.c",
            "src/core/nk_psp_sha1.h",
        ),
    ),
    (
        "#355",
        "vendored LGPL ATRAC3+ decoder and its selftest have no independence campaign group yet",
        (
            "src/rt/atrac3p/LICENSE.LGPLv2.1.txt",
            "src/rt/atrac3p/PROVENANCE.md",
            "src/rt/atrac3p/atrac3p_api.c",
            "src/rt/atrac3p/atrac3p_api.h",
            "src/rt/atrac3p/libavcodec/atrac.c",
            "src/rt/atrac3p/libavcodec/atrac.h",
            "src/rt/atrac3p/libavcodec/atrac3plus.c",
            "src/rt/atrac3p/libavcodec/atrac3plus.h",
            "src/rt/atrac3p/libavcodec/atrac3plus_data.h",
            "src/rt/atrac3p/libavcodec/atrac3plusdec.c",
            "src/rt/atrac3p/libavcodec/atrac3plusdsp.c",
            "src/rt/atrac3p/libavcodec/avcodec.h",
            "src/rt/atrac3p/libavcodec/avfft.h",
            "src/rt/atrac3p/libavcodec/bitstream.c",
            "src/rt/atrac3p/libavcodec/config.h",
            "src/rt/atrac3p/libavcodec/fft-internal.h",
            "src/rt/atrac3p/libavcodec/fft.h",
            "src/rt/atrac3p/libavcodec/fft_float.c",
            "src/rt/atrac3p/libavcodec/fft_init_table.c",
            "src/rt/atrac3p/libavcodec/fft_table.h",
            "src/rt/atrac3p/libavcodec/fft_template.c",
            "src/rt/atrac3p/libavcodec/get_bits.h",
            "src/rt/atrac3p/libavcodec/internal.h",
            "src/rt/atrac3p/libavcodec/mathops.h",
            "src/rt/atrac3p/libavcodec/mdct_float.c",
            "src/rt/atrac3p/libavcodec/mdct_template.c",
            "src/rt/atrac3p/libavcodec/put_bits.h",
            "src/rt/atrac3p/libavcodec/sinewin.c",
            "src/rt/atrac3p/libavcodec/sinewin.h",
            "src/rt/atrac3p/libavcodec/sinewin_tablegen.h",
            "src/rt/atrac3p/libavcodec/version.h",
            "src/rt/atrac3p/libavcodec/vlc.h",
            "src/rt/atrac3p/libavutil/attributes.h",
            "src/rt/atrac3p/libavutil/avassert.h",
            "src/rt/atrac3p/libavutil/avconfig.h",
            "src/rt/atrac3p/libavutil/avutil.h",
            "src/rt/atrac3p/libavutil/bswap.h",
            "src/rt/atrac3p/libavutil/channel_layout.h",
            "src/rt/atrac3p/libavutil/common.h",
            "src/rt/atrac3p/libavutil/config.h",
            "src/rt/atrac3p/libavutil/dynarray.h",
            "src/rt/atrac3p/libavutil/error.h",
            "src/rt/atrac3p/libavutil/float_dsp.c",
            "src/rt/atrac3p/libavutil/float_dsp.h",
            "src/rt/atrac3p/libavutil/intmath.c",
            "src/rt/atrac3p/libavutil/intmath.h",
            "src/rt/atrac3p/libavutil/intreadwrite.h",
            "src/rt/atrac3p/libavutil/libm.h",
            "src/rt/atrac3p/libavutil/log.h",
            "src/rt/atrac3p/libavutil/log2_tab.c",
            "src/rt/atrac3p/libavutil/macros.h",
            "src/rt/atrac3p/libavutil/mathematics.h",
            "src/rt/atrac3p/libavutil/mem.c",
            "src/rt/atrac3p/libavutil/mem.h",
            "src/rt/atrac3p/libavutil/mem_internal.h",
            "src/rt/atrac3p/libavutil/qsort.h",
            "src/rt/atrac3p/libavutil/reverse.c",
            "src/rt/atrac3p/libavutil/reverse.h",
            "src/rt/atrac3p/libavutil/thread.h",
            "src/rt/atrac3p/libavutil/version.h",
            "src/rt/atrac3p_selftest.c",
        ),
    ),
    (
        "#354",
        "host shell glue awaiting the host-shell independence lane",
        (
            "src/rt/funcdiff.c",
            "src/rt/gui.c",
            "src/rt/osk_win.c",
        ),
    ),
    (
        "#359",
        "extracted HLE power handlers awaiting the HLE independence lane",
        (
            "src/rt/hle_power.c",
            "src/rt/hle_power.h",
        ),
    ),
    (
        "#355",
        "trace, microtest and codegen-gate tooling has no independence campaign group yet",
        (
            "tools/TRACE_FORMAT.md",
            "tools/codegen_gate.py",
            "tools/funcdiff_cmp.py",
            "tools/gen_microtest.py",
            "tools/microtest_gate.py",
            "tools/nidseq.py",
            "tools/ppm2png.py",
            "tools/ppmdiff.py",
            "tools/test_codegen_gate_b_encoding.py",
            "tools/test_hle_manifest.py",
            "tools/test_savedata_spans.py",
            "tools/tracediff.py",
            "tools/vfpu_fuzz_gen.py",
        ),
    ),
)

KNOWN_DISPOSITION_GAPS: dict[str, dict[str, str]] = {
    path: {"issue": issue, "reason": reason}
    for issue, reason, paths in _KNOWN_DISPOSITION_GAP_GROUPS
    for path in paths
}


def disposition_gaps(*, repo: Path = ROOT) -> list[dict[str, object]]:
    """Return every upstream-derived production path with no campaign disposition.

    Unlike :func:`record_gaps` this does not depend on record coverage: an
    upstream-derived path needs a roadmap disposition whether or not a trusted
    record already covers it, so admitting a record never hides the gap.
    """
    classifications = public_classifications(repo / LEDGER_PATH)
    campaign_path = repo / INDEPENDENCE_PLAN_PATH
    policy = load_policy(repo / POLICY_PATH)
    gaps: list[dict[str, object]] = []
    for path in tracked_paths(repo):
        if not path.startswith(_PRODUCTION_PREFIXES):
            continue
        if policy.resolve(path).disposition != "included":
            continue
        classification = classifications.get(path)
        if classification is None:
            classification, _evidence = provenance_ledger.class_for(path, None)
        if classification not in {"upstream_derived", "unresolved"}:
            continue
        if independence_metadata(path, campaign_path=campaign_path) is not None:
            continue
        gaps.append({
            "path": path,
            "deterministic_class": classification,
            "independence": {
                "status": "missing",
                "reason": "no explicit campaign owner or disposition",
                "source": INDEPENDENCE_PLAN_PATH,
            },
        })
    return gaps


def validate_baseline(baseline: dict[str, dict[str, str]]) -> None:
    """Validate that every baseline entry carries an owner issue and reason."""
    for path, entry in baseline.items():
        if not isinstance(entry, dict):
            raise ValueError(f"baseline entry for {path!r} must be a dictionary")
        owner = entry.get("issue")
        reason = entry.get("reason")
        if not isinstance(owner, str) or not re.fullmatch(r"#[1-9][0-9]*", owner):
            raise ValueError(f"baseline entry for {path!r} missing owner issue (#N)")
        if not reason or not isinstance(reason, str) or not reason.strip():
            raise ValueError(f"baseline entry for {path!r} missing reason")


def check_disposition_gaps(
    gaps: list[dict[str, object]],
    *,
    baseline: dict[str, dict[str, str]] | None = None,
) -> tuple[list[str], list[str]]:
    """Check gaps against the disposition ratchet baseline.

    Returns (unlisted_gaps, stale_baseline_entries).
    - unlisted_gaps: upstream-derived production paths with status=missing that
      are not recorded in the baseline.
    - stale_baseline_entries: baseline paths that are no longer missing gaps.
    """
    if baseline is None:
        baseline = KNOWN_DISPOSITION_GAPS

    missing_gaps = {
        str(gap["path"])
        for gap in gaps
        if isinstance(gap.get("independence"), dict)
        and gap["independence"].get("status") == "missing"
    }

    baseline_paths = set(baseline.keys())
    unlisted = sorted(missing_gaps - baseline_paths)
    stale = sorted(baseline_paths - missing_gaps)
    return unlisted, stale


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument(
        "--trusted-ledger", type=Path, default=None,
        help="external detailed implementation ledger; when supplied, the exact-record answer "
             "comes from the authority instead of the committed public projection",
    )
    parser.add_argument("--json", action="store_true", help="emit the inventory as JSON")
    parser.add_argument(
        "--check", action="store_true",
        help="fail closed if any upstream-derived production path lacks a roadmap disposition",
    )
    parser.add_argument(
        "--check-records", action="store_true",
        help="exit non-zero when any gap remains (an admission inventory, not a merge gate)",
    )
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    repo = args.repo.resolve()
    source = "committed public ledger (projection of the private authority)"
    covered = None
    if args.trusted_ledger is not None:
        covered = exact_paths_from_trusted_ledger(args.trusted_ledger)
        source = "trusted authority (--trusted-ledger)"

    gaps = record_gaps(repo=repo, covered=covered)

    if args.json:
        print(json.dumps({"source": source, "gap_count": len(gaps), "paths": gaps}, indent=2))
    else:
        print(f"record gap inventory (source: {source})")
        print(f"{len(gaps)} tracked public path(s) have no exact trusted record; editing or "
              f"adding one of them fails with TRUSTED_PATH_MISSING until a maintainer admits it.")
        for gap in gaps:
            print(f"  {gap['path']}  [{gap['deterministic_class']}]")
            independence = gap.get("independence")
            if isinstance(independence, dict) and independence.get("status") == "missing":
                print("    independence: missing campaign owner/disposition")
        if gaps:
            print("\nBatch admission: one trusted record covering all of these paths clears them "
                  "together; per-path records are equally acceptable and are the stronger form.")

    if args.check:
        validate_baseline(KNOWN_DISPOSITION_GAPS)
        unlisted, stale = check_disposition_gaps(disposition_gaps(repo=repo),
                                                 baseline=KNOWN_DISPOSITION_GAPS)
        if unlisted or stale:
            if unlisted:
                print(
                    f"provenance_record_gap: FAIL -- {len(unlisted)} upstream-derived production path(s) "
                    f"have no roadmap disposition:",
                    file=sys.stderr,
                )
                for path in unlisted:
                    print(
                        f"  {path} (status=missing; add owner group and disposition to {INDEPENDENCE_PLAN_PATH})",
                        file=sys.stderr,
                    )
            if stale:
                print(
                    f"provenance_record_gap: FAIL -- {len(stale)} baseline entry/entries are stale "
                    f"(resolved; remove from baseline):",
                    file=sys.stderr,
                )
                for path in stale:
                    print(f"  {path}", file=sys.stderr)
            return 1
        print(f"provenance_record_gap: PASS -- no unlisted upstream-derived production gaps without "
              f"roadmap disposition ({len(KNOWN_DISPOSITION_GAPS)} known gap(s) in the reviewed baseline)")
        return 0

    if args.check_records:
        return 1 if gaps else 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
