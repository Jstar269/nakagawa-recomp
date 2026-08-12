#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Report, and optionally apply, drift between tracked paths and the canonical policy.

``--check`` (the default) reports any tracked path the canonical policy does not
classify and exits non-zero. It changes nothing and is safe for CI and hooks.

``--apply`` adds the drifted paths to ``include_paths``. That is a convenience for
routine, obviously-safe additions **only**; it deliberately refuses to add anything
matching a high-risk pattern (fonts, binary/asset extensions, PGF/PGD/key-adjacent
names). Those must be hand-edited by a human who has made an actual publication
decision, with a rationale, and they show up in the review diff either way.

The friction is the point. Adding a file to the public surface is a publication
decision. It was the absence of any such moment -- new paths under ``src/`` and
``tools/`` being assumed publishable -- that produced the 2026-08-11 breach.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from publication_policy import UNCLASSIFIED, load_policy  # noqa: E402

DEFAULT_POLICY = ROOT / "assets" / "public_source_profile.json"

#: Paths that --apply will never add on its own authority. These are the classes
#: where "is this publishable?" is a real question rather than a formality.
HIGH_RISK = (
    re.compile(r"(?:^|/)font/", re.IGNORECASE),
    re.compile(r"\.(?:pgf|ttf|otf|bin|dat|prx|elf|iso|cso|pbp|psar|sfo|gim|vag|pmf|at3)$", re.IGNORECASE),
    re.compile(r"pg[df]", re.IGNORECASE),
    re.compile(r"(?:key|vkey|kirk|amctrl|secret|token)", re.IGNORECASE),
    re.compile(r"(?:oracle|memstick|original_game|place_game_here)", re.IGNORECASE),
)


def tracked_paths(repo_root: Path) -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=repo_root, capture_output=True, text=True, check=True)
    return out.stdout.split()


def is_high_risk(path: str) -> bool:
    return any(pattern.search(path) for pattern in HIGH_RISK)


def _regen_export(policy) -> None:
    """Rewrite the generated export so it carries the current policy digest.

    ``PUBLIC_EXPORT.json`` is evidence derived from the policy, never a second
    place where publication decisions live. It is regenerated, not edited.
    """
    import public_export

    export_path = ROOT / "PUBLIC_EXPORT.json"
    files = public_export.index_files(ROOT)
    ledger_path = ROOT / "assets" / "public_provenance_ledger.json"
    manifest_path = ROOT / "assets" / "release_manifest.json"
    document = public_export.build_document(
        policy,
        files,
        provenance_ledger=ledger_path.read_bytes() if ledger_path.is_file() else None,
        manifest=manifest_path.read_bytes() if manifest_path.is_file() else None,
    )
    public_export.write_document(export_path, document)
    print(f"regenerated {export_path.name} for policy digest {policy.digest[:16]}...")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--apply", action="store_true",
                        help="add low-risk drifted paths to include_paths (high-risk paths are refused)")
    parser.add_argument("--regen-export", action="store_true",
                        help="rewrite PUBLIC_EXPORT.json so its policy digest matches the current policy")
    args = parser.parse_args(argv)

    policy = load_policy(args.policy)

    if args.regen_export:
        _regen_export(policy)
    drift = [p for p in tracked_paths(ROOT) if policy.resolve(p).disposition == UNCLASSIFIED]

    if not drift:
        print(f"policy sync: OK (no unclassified tracked paths; policy digest {policy.digest[:16]}...)")
        return 0

    risky = [p for p in drift if is_high_risk(p)]
    routine = [p for p in drift if not is_high_risk(p)]

    print(f"policy sync: {len(drift)} tracked path(s) are not classified by {args.policy}")
    for path in routine:
        print(f"  unclassified: {path}")
    for path in risky:
        print(f"  unclassified (HIGH RISK - hand-edit required): {path}")

    if not args.apply:
        print("\nAdd each path to include_paths as a reviewed publication decision, "
              "or to exclude_paths with a rationale. Re-run with --apply to add the "
              "routine ones automatically.", file=sys.stderr)
        return 1

    if risky:
        print(f"\nrefusing --apply: {len(risky)} high-risk path(s) require a human publication "
              f"decision and an explicit exclude_rationale entry if excluded", file=sys.stderr)
        return 1

    document = json.loads(args.policy.read_text(encoding="utf-8"))
    document["include_paths"] = sorted(set(document["include_paths"]) | set(routine))
    with args.policy.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(document, indent=2, ensure_ascii=False) + "\n")

    updated = load_policy(args.policy)
    print(f"\nadded {len(routine)} path(s); policy digest {policy.digest[:16]}... -> {updated.digest[:16]}...")
    print("Regenerate PUBLIC_EXPORT.json so its policy digest matches, then re-audit.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
