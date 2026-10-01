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
    return 1 if (args.check and gaps) else 0


if __name__ == "__main__":
    raise SystemExit(main())
