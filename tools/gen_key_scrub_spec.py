#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Generate a `git filter-repo --replace-text` spec that purges the PSP KIRK/amctrl
constants from Git history.

This script contains no key material. It reads the constant values from the local
key file (a private local binding, outside the public candidate) and emits one replacement line per textual encoding
the tree has ever used, so filter-repo rewrites every historical occurrence to a
fixed placeholder regardless of hex/byte-array spelling, case, or padding.

The OUTPUT contains key material and must never be committed. It is written to a
path you choose (use a temp/ignored location) and deleted after the scrub. This
script refuses to write inside the working tree's tracked area by default.

Usage:
  python tools/gen_key_scrub_spec.py --out "$TMP/pgd-key-replacements.txt"
  git filter-repo --replace-text "$TMP/pgd-key-replacements.txt"   # on a mirror clone

See docs/KEY_HISTORY_SCRUB.md for the full procedure.
"""

from __future__ import annotations

import argparse
import os
import sys

# Reuse the exact same encoding set the verifier checks, so "generate" and "verify"
# can never drift apart.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_key_scrub import encodings, key_values  # noqa: E402

PLACEHOLDER = "PSP_CONSTANT_REDACTED"


def build_spec(values: dict[str, bytes]) -> list[str]:
    lines: list[str] = []
    seen: set[str] = set()
    for raw in values.values():
        for form in encodings(raw):
            if form in seen:
                continue
            seen.add(form)
            # literal: prefix disables regex interpretation; exact-match replacement.
            lines.append(f"literal:{form}==>{PLACEHOLDER}")
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", default=os.environ.get("SR_PGD_KEYS") or os.path.join("keys", "pgd_keys.txt"))
    parser.add_argument("--out", required=True, help="output spec path (use a temp/ignored location)")
    parser.add_argument("--force", action="store_true", help="allow writing inside the repo tree")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.keys):
        print(f"key file not found: {args.keys} (private local binding)", file=sys.stderr)
        return 2

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_abs = os.path.abspath(args.out)
    if not args.force and os.path.commonpath([out_abs, repo_root]) == repo_root:
        print(
            f"refusing to write the spec inside the repo ({out_abs}); it contains key\n"
            "material. Choose a temp path, or pass --force if you have gitignored it.",
            file=sys.stderr,
        )
        return 2

    values = key_values(args.keys)
    if not values:
        print(f"no usable constants parsed from {args.keys}", file=sys.stderr)
        return 2

    lines = build_spec(values)
    with open(out_abs, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines) + "\n")
    print(f"wrote {len(lines)} replacement rules for {len(values)} constants -> {out_abs}")
    print("This file contains key material. Delete it after the scrub; never commit it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
