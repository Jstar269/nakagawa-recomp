#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Extract pinned PSPSDK declarations and compare them with Nakagawa HLE."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

import pspdev_lock
from pspsdk_compare import compare_with_nakagawa, render_markdown
from pspsdk_source import SyncError, build_upstream_manifest, verify_source_identity

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "build" / "audit" / "pspsdk-platform-manifest.json"
DEFAULT_COMPARISON = ROOT / "build" / "audit" / "pspsdk-nakagawa-comparison.json"
DEFAULT_REPORT = ROOT / "build" / "audit" / "pspsdk-nakagawa-comparison.md"


def dump_json(obj: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(obj, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
        encoding="ascii",
        newline="\n",
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pspsdk-root", type=Path, required=True)
    parser.add_argument("--lock", type=Path, default=pspdev_lock.DEFAULT_LOCK)
    parser.add_argument("--manifest-out", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--comparison-out", type=Path, default=DEFAULT_COMPARISON)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--source-commit")
    parser.add_argument("--allow-unverified-source", action="store_true")
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--no-compare", action="store_true")
    args = parser.parse_args(argv)

    try:
        lock, _ = pspdev_lock.load_lock(args.lock)
        expected_commit = lock["components"]["pspsdk"]["commit"]
        root = args.pspsdk_root.resolve(strict=True)
        identity = verify_source_identity(
            root,
            expected_commit,
            asserted_commit=args.source_commit,
            allow_unverified_source=args.allow_unverified_source,
            allow_dirty=args.allow_dirty,
        )
        upstream = build_upstream_manifest(root, expected_commit, identity)
        dump_json(upstream, args.manifest_out)
        print(
            f"pspsdk_sync: {upstream['statistics']['imports']} imports, "
            f"{upstream['statistics']['symbols_with_prototypes']} symbols with "
            f"prototypes -> {args.manifest_out}"
        )

        if not args.no_compare:
            sys.path.insert(0, str(ROOT / "tools"))
            import hle_manifest  # noqa: E402

            comparison = compare_with_nakagawa(
                upstream, hle_manifest.build_manifest()
            )
            dump_json(comparison, args.comparison_out)
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(
                render_markdown(upstream, comparison),
                encoding="utf-8",
                newline="\n",
            )
            print(
                f"pspsdk_sync: {len(comparison['findings'])} comparison rows -> "
                f"{args.comparison_out}, {args.report}"
            )
    except (OSError, SyncError, pspdev_lock.LockError) as exc:
        print(f"pspsdk_sync: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
